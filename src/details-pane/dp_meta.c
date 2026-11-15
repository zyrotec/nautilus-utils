/* Details pane readers (PLAN.md §13.2, §13.3). See dp_meta.h. */
#include "dp_meta.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#ifdef DP_HAVE_GEXIV2
#include <gexiv2/gexiv2.h>
#endif
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>
#ifdef DP_HAVE_POPPLER
#include <poppler.h>
#endif
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "dp_format.h"

/* ------------------------------------------------------------------------------------------------ */
/* Rows and groups                                                                                  */

static void
row_free (gpointer data)
{
  DpRow *row = data;
  g_free (row->label);
  g_free (row->value);
  g_free (row->tooltip);
  g_free (row);
}

/* Returns a new group; caller frees with dp_group_free. */
DpGroup *
dp_group_new (const char *title)
{
  DpGroup *group = g_new0 (DpGroup, 1);
  group->title = g_strdup (title);
  group->rows = g_ptr_array_new_with_free_func (row_free);
  return group;
}

void
dp_group_free (DpGroup *group)
{
  if (group == NULL)
    return;
  g_free (group->title);
  g_ptr_array_unref (group->rows);
  g_free (group);
}

void
dp_group_take (DpGroup    *group,
               const char *label,
               char       *value,
               char       *tooltip)
{
  if (value == NULL || *value == '\0')
    {
      g_free (value);
      g_free (tooltip);
      return;
    }
  DpRow *row = g_new0 (DpRow, 1);
  row->label = g_strdup (label);
  row->value = value;     /* taken */
  row->tooltip = tooltip; /* taken */
  g_ptr_array_add (group->rows, row);
}

const DpRow *
dp_group_find (const DpGroup *group,
               const char    *label)
{
  for (guint i = 0; i < group->rows->len; i++)
    {
      const DpRow *row = g_ptr_array_index (group->rows, i);
      if (g_strcmp0 (row->label, label) == 0)
        return row;
    }
  return NULL;
}

/* A NULL-safe, stripped copy of @s, NULL if empty. Takes ownership of @s. */
static char *
clean (char *s)
{
  if (s == NULL)
    return NULL;
  g_strstrip (s);
  if (*s == '\0')
    {
      g_free (s);
      return NULL;
    }
  return s;
}

/* ------------------------------------------------------------------------------------------------ */
/* General information                                                                              */

#define GENERAL_ATTRS                                                                                    \
  G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE ","                          \
  G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE "," G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE ","             \
  G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_STANDARD_ICON ","                                   \
  G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK "," G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET ","                   \
  G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_TIME_CREATED "," G_FILE_ATTRIBUTE_TIME_ACCESS ","    \
  G_FILE_ATTRIBUTE_UNIX_MODE "," G_FILE_ATTRIBUTE_OWNER_USER "," G_FILE_ATTRIBUTE_OWNER_GROUP ","          \
  "access::*,metadata::custom-icon,metadata::custom-icon-name"

static void
info_clear (DpInfo *info)
{
  g_clear_object (&info->file);
  g_clear_pointer (&info->uri, g_free);
  g_clear_pointer (&info->display_name, g_free);
  g_clear_pointer (&info->content_type, g_free);
  g_clear_pointer (&info->mime_type, g_free);
  g_clear_pointer (&info->description, g_free);
  g_clear_pointer (&info->symlink_target, g_free);
  g_clear_object (&info->icon);
  g_clear_pointer (&info->modified, g_date_time_unref);
  g_clear_pointer (&info->created, g_date_time_unref);
  g_clear_pointer (&info->accessed, g_date_time_unref);
  g_clear_pointer (&info->location, g_free);
  g_clear_pointer (&info->perms.owner, g_free);
  g_clear_pointer (&info->perms.group, g_free);
}

DpInfo *
dp_info_ref (DpInfo *info)
{
  return g_atomic_rc_box_acquire (info);
}

void
dp_info_unref (DpInfo *info)
{
  if (info != NULL)
    g_atomic_rc_box_release_full (info, (GDestroyNotify) info_clear);
}

DpKind
dp_kind_for_content_type (const char *content_type,
                          gboolean    is_dir)
{
  if (is_dir)
    return DP_KIND_FOLDER;
  if (content_type == NULL)
    return DP_KIND_OTHER;
  g_autofree char *mime = g_content_type_get_mime_type (content_type);
  const char *m = mime != NULL ? mime : content_type;
  if (g_str_has_prefix (m, "image/"))
    return DP_KIND_IMAGE;
  if (g_str_has_prefix (m, "video/"))
    return DP_KIND_VIDEO;
  if (g_str_has_prefix (m, "audio/"))
    return DP_KIND_AUDIO;
  if (g_str_equal (m, "application/pdf"))
    return DP_KIND_PDF;
  if (g_str_has_prefix (m, "text/") || g_str_has_prefix (m, "application/vnd.oasis.opendocument") ||
      g_str_has_prefix (m, "application/vnd.openxmlformats-officedocument") || g_str_equal (m, "application/msword") ||
      g_str_equal (m, "application/rtf") || g_str_equal (m, "application/epub+zip"))
    return DP_KIND_DOCUMENT;
  return DP_KIND_OTHER;
}

/* New ref: the user's custom icon from Files' metadata, or NULL. */
static GIcon *
custom_icon (GFileInfo *fi)
{
  const char *uri = g_file_info_get_attribute_string (fi, "metadata::custom-icon");
  if (uri != NULL && *uri != '\0')
    {
      g_autoptr (GFile) f = g_file_new_for_uri (uri);
      return g_file_icon_new (f);
    }
  const char *name = g_file_info_get_attribute_string (fi, "metadata::custom-icon-name");
  if (name != NULL && *name != '\0')
    return g_themed_icon_new (name);
  return NULL;
}

DpInfo *
dp_info_read (GFile         *file,
              GCancellable  *cancellable,
              GError       **error)
{
  g_autoptr (GFileInfo) fi = g_file_query_info (file, GENERAL_ATTRS, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                cancellable, error);
  if (fi == NULL)
    return NULL;
  gboolean is_symlink = g_file_info_get_is_symlink (fi);
  if (is_symlink)
    {
      /* Show what the link points to (like Files), keeping the link's own name and target. */
      g_autoptr (GFileInfo) target = g_file_query_info (file, GENERAL_ATTRS, G_FILE_QUERY_INFO_NONE, cancellable, NULL);
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        return NULL;
      if (target != NULL)
        {
          g_file_info_set_attribute_byte_string (target, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET,
                                                 g_file_info_get_symlink_target (fi));
          g_file_info_set_display_name (target, g_file_info_get_display_name (fi));
          g_set_object (&fi, target);
        }
    }

  DpInfo *info = g_atomic_rc_box_new0 (DpInfo);
  info->file = g_object_ref (file);
  info->uri = g_file_get_uri (file);
  const char *dn = g_file_info_get_attribute_string (fi, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME);
  info->display_name = dn != NULL ? g_strdup (dn) : g_file_get_basename (file);
  if (info->display_name == NULL)
    info->display_name = g_strdup (info->uri);
  info->is_dir = g_file_info_get_file_type (fi) == G_FILE_TYPE_DIRECTORY;
  info->is_symlink = is_symlink;
  info->is_native = g_file_is_native (file);
  const char *ct = g_file_info_get_attribute_string (fi, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE);
  if (ct == NULL)
    ct = g_file_info_get_attribute_string (fi, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);
  info->content_type = g_strdup (ct);
  info->mime_type = ct != NULL ? g_content_type_get_mime_type (ct) : NULL;
  info->kind = dp_kind_for_content_type (ct, info->is_dir);
  info->description = info->is_dir ? g_strdup ("Folder") : ct != NULL ? g_content_type_get_description (ct) : NULL;
  const char *target = g_file_info_get_attribute_byte_string (fi, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET);
  info->symlink_target = is_symlink ? g_strdup (target) : NULL;
  info->icon = custom_icon (fi);
  info->custom_icon = info->icon != NULL;
  if (info->icon == NULL && g_file_info_get_icon (fi) != NULL)
    info->icon = g_object_ref (g_file_info_get_icon (fi));
  info->size = (guint64) g_file_info_get_attribute_uint64 (fi, G_FILE_ATTRIBUTE_STANDARD_SIZE);
  info->modified = g_file_info_get_modification_date_time (fi);
  info->created = g_file_info_get_creation_date_time (fi);
  info->accessed = g_file_info_get_access_date_time (fi);
  info->mtime_s = g_file_info_get_attribute_uint64 (fi, G_FILE_ATTRIBUTE_TIME_MODIFIED);
  g_autoptr (GFile) parent = g_file_get_parent (file);
  if (parent != NULL)
    info->location = g_file_is_native (parent) ? g_file_get_path (parent) : g_file_get_parse_name (parent);

  DpPerms *p = &info->perms;
  p->has_mode = g_file_info_has_attribute (fi, G_FILE_ATTRIBUTE_UNIX_MODE);
  p->mode = g_file_info_get_attribute_uint32 (fi, G_FILE_ATTRIBUTE_UNIX_MODE);
  p->owner = g_strdup (g_file_info_get_attribute_string (fi, G_FILE_ATTRIBUTE_OWNER_USER));
  p->group = g_strdup (g_file_info_get_attribute_string (fi, G_FILE_ATTRIBUTE_OWNER_GROUP));
  p->has_access = g_file_info_has_attribute (fi, G_FILE_ATTRIBUTE_ACCESS_CAN_READ);
  p->can_read = g_file_info_get_attribute_boolean (fi, G_FILE_ATTRIBUTE_ACCESS_CAN_READ);
  p->can_write = g_file_info_get_attribute_boolean (fi, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE);
  p->can_execute = g_file_info_get_attribute_boolean (fi, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE);
  return info;
}

/* ------------------------------------------------------------------------------------------------ */
/* Details group                                                                                    */

DpGroup *
dp_details_group (const DpInfo        *info,
                  const DpFolderCount *count,
                  GDateTime           *now)
{
  DpGroup *g = dp_group_new ("Details");
  if (info->description != NULL)
    {
      dp_group_take (g, "Type", g_strdup (info->description), info->is_dir ? NULL : g_strdup (info->mime_type));
    }
  if (info->is_dir)
    {
      if (count == NULL)
        {
          dp_group_take (g, "Contents", g_strdup ("Counting…"), NULL);
          dp_group_take (g, "Total size", g_strdup ("Calculating…"), NULL);
        }
      else
        {
          dp_group_take (g, "Contents", dp_format_item_count (count->items, count->hidden, count->items_truncated),
                         NULL);
          g_autofree char *size = dp_format_size (count->total);
          dp_group_take (g, "Total size",
                         count->total_truncated ? g_strdup_printf ("At least %s", size) : g_strdup (size),
                         count->total_truncated
                           ? g_strdup_printf ("Stopped after %" G_GUINT64_FORMAT " entries", count->entries)
                           : dp_format_size_exact (count->total));
        }
    }
  else
    dp_group_take (g, "Size", dp_format_size (info->size), dp_format_size_exact (info->size));
  dp_group_take (g, "Location", g_strdup (info->location), NULL);
  if (info->modified != NULL)
    dp_group_take (g, "Modified", dp_format_date (info->modified, now), NULL);
  if (info->created != NULL)
    dp_group_take (g, "Created", dp_format_date (info->created, now), NULL);
  if (info->accessed != NULL)
    dp_group_take (g, "Accessed", dp_format_date (info->accessed, now), NULL);
  dp_group_take (g, "Link target", g_strdup (info->symlink_target), NULL);
  return g;
}

/* ------------------------------------------------------------------------------------------------ */
/* Images: gexiv2, gdk-pixbuf for what exiv2 can't read (PLAN.md §13.7 item 8)                      */

#ifdef DP_HAVE_GEXIV2
static gpointer
init_gexiv2 (gpointer data)
{
  (void) data;
  return GINT_TO_POINTER (gexiv2_initialize ());
}

#endif

/* "JPEG" from "image/jpeg", "SVG" from "image/svg+xml", "CR2" from "image/x-canon-cr2". New string. */
static char *
image_format_name (const char *mime)
{
  if (mime == NULL)
    return NULL;
  const char *slash = strchr (mime, '/');
  const char *sub = slash != NULL ? slash + 1 : mime;
  if (g_str_has_prefix (sub, "x-"))
    sub += 2;
  const char *dash = strrchr (sub, '-');
  if (dash != NULL && g_str_has_prefix (mime, "image/x-"))
    sub = dash + 1; /* x-canon-cr2 → cr2 */
  g_autofree char *base = g_strdup (sub);
  char *plus = strchr (base, '+');
  if (plus != NULL)
    *plus = '\0';
  return g_ascii_strup (base, -1);
}

#ifdef DP_HAVE_GEXIV2
/* Interpreted EXIF value, or NULL. New string. */
static char *
exif_text (GExiv2Metadata *m, const char *tag)
{
  if (!gexiv2_metadata_has_tag (m, tag, NULL))
    return NULL;
  return clean (gexiv2_metadata_get_tag_interpreted_string (m, tag, NULL));
}

/* "2026:11:14 12:34:56" → formatted date, or NULL. New string. */
static char *
exif_date (GExiv2Metadata *m, const char *tag)
{
  g_autofree char *raw = gexiv2_metadata_has_tag (m, tag, NULL) ? gexiv2_metadata_get_tag_string (m, tag, NULL) : NULL;
  int y, mo, d, h, mi, s;
  if (raw == NULL || sscanf (raw, "%d:%d:%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6)
    return NULL;
  g_autoptr (GTimeZone) tz = g_time_zone_new_local ();
  g_autoptr (GDateTime) when = g_date_time_new (tz, y, mo, d, h, mi, s);
  if (when == NULL)
    return NULL;
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  return dp_format_date (when, now);
}

static const char *
orientation_text (GExiv2Orientation o)
{
  switch (o)
    {
    case GEXIV2_ORIENTATION_ROT_90:
      return "Rotated 90° clockwise";
    case GEXIV2_ORIENTATION_ROT_180:
      return "Rotated 180°";
    case GEXIV2_ORIENTATION_ROT_270:
      return "Rotated 90° anticlockwise";
    case GEXIV2_ORIENTATION_HFLIP:
    case GEXIV2_ORIENTATION_VFLIP:
    case GEXIV2_ORIENTATION_ROT_90_HFLIP:
    case GEXIV2_ORIENTATION_ROT_90_VFLIP:
      return "Mirrored";
    default:
      return NULL; /* normal or unknown: not worth a row */
    }
}

#endif

static DpGroup *
read_image (DpInfo *info, const char *path)
{
  DpGroup *g = dp_group_new ("Image");
  g_autofree char *mime = info->content_type != NULL ? g_content_type_get_mime_type (info->content_type) : NULL;
  int width = 0, height = 0;
  g_autofree char *format = NULL;
#ifdef DP_HAVE_GEXIV2
  g_autoptr (GExiv2Metadata) m = NULL;
  if (info->size <= DP_PARSE_MAX_BYTES)
    {
      static GOnce once = G_ONCE_INIT;
      g_once (&once, init_gexiv2, NULL);
      m = gexiv2_metadata_new ();
      if (!gexiv2_metadata_open_path (m, path, NULL))
        g_clear_object (&m);
    }
  if (m != NULL)
    {
      width = gexiv2_metadata_get_pixel_width (m);
      height = gexiv2_metadata_get_pixel_height (m);
    }
  format = image_format_name (m != NULL ? gexiv2_metadata_get_mime_type (m) : mime);
#else
  format = image_format_name (mime);
#endif
  if (width <= 0 || height <= 0)
    {
      /* SVG, ICO, … : GTK's loaders (glycin, sandboxed; slower, PLAN.md §13.7 item 8). */
      GdkPixbufFormat *pf = gdk_pixbuf_get_file_info (path, &width, &height); /* static, not owned */
      if (pf != NULL && format == NULL)
        format = g_ascii_strup (gdk_pixbuf_format_get_name (pf), -1);
    }
  if (width > 0 && height > 0)
    {
      dp_group_take (g, "Dimensions", dp_format_dimensions ((guint) width, (guint) height), NULL);
      dp_group_take (g, "Width", g_strdup_printf ("%d px", width), NULL);
      dp_group_take (g, "Height", g_strdup_printf ("%d px", height), NULL);
    }
  dp_group_take (g, "Format", g_steal_pointer (&format), NULL);
#ifdef DP_HAVE_GEXIV2
  if (m != NULL && gexiv2_metadata_get_supports_exif (m))
    {
      g_autofree char *make = exif_text (m, "Exif.Image.Make");
      g_autofree char *model = exif_text (m, "Exif.Image.Model");
      char *camera = NULL;
      if (model != NULL && make != NULL && !g_str_has_prefix (model, make))
        camera = g_strdup_printf ("%s %s", make, model);
      else
        camera = g_strdup (model != NULL ? model : make);
      dp_group_take (g, "Camera", camera, NULL);
      dp_group_take (g, "Date taken", exif_date (m, "Exif.Photo.DateTimeOriginal"), NULL);
      dp_group_take (g, "Exposure", exif_text (m, "Exif.Photo.ExposureTime"), NULL);
      dp_group_take (g, "Aperture", exif_text (m, "Exif.Photo.FNumber"), NULL);
      int iso = gexiv2_metadata_get_iso_speed (m, NULL);
      dp_group_take (g, "ISO", iso > 0 ? g_strdup_printf ("%d", iso) : NULL, NULL);
      dp_group_take (g, "Focal length", exif_text (m, "Exif.Photo.FocalLength"), NULL);
      const char *o = orientation_text (gexiv2_metadata_get_orientation (m, NULL));
      dp_group_take (g, "Orientation", g_strdup (o), NULL);
    }
#endif
  return g;
}

/* ------------------------------------------------------------------------------------------------ */
/* Video and audio: GstDiscoverer                                                                   */

static char *
codec_name (GstDiscovererStreamInfo *stream)
{
  g_autoptr (GstCaps) caps = gst_discoverer_stream_info_get_caps (stream);
  return caps != NULL ? clean (gst_pb_utils_get_codec_description (caps)) : NULL;
}

/* Merges every tag list of @info (container and streams) into one. Caller owns the result (may be empty). */
static GstTagList *
all_tags (GstDiscovererInfo *info)
{
  GstTagList *tags = gst_tag_list_new_empty ();
  GList *streams = gst_discoverer_info_get_stream_list (info);
  for (GList *l = streams; l != NULL; l = l->next)
    {
      const GstTagList *t = GST_IS_DISCOVERER_CONTAINER_INFO (l->data)
                              ? gst_discoverer_container_info_get_tags (l->data)
                              : gst_discoverer_stream_info_get_tags (l->data);
      if (t != NULL)
        gst_tag_list_insert (tags, t, GST_TAG_MERGE_KEEP);
    }
  gst_discoverer_stream_info_list_free (streams);
  return tags;
}

static char *
tag_string (const GstTagList *tags, const char *tag)
{
  char *s = NULL;
  return gst_tag_list_get_string (tags, tag, &s) ? clean (s) : NULL;
}

static char *
tag_year (const GstTagList *tags)
{
  GstDateTime *dt = NULL;
  if (gst_tag_list_get_date_time (tags, GST_TAG_DATE_TIME, &dt))
    {
      char *year = gst_date_time_has_year (dt) ? g_strdup_printf ("%d", gst_date_time_get_year (dt)) : NULL;
      gst_date_time_unref (dt);
      return year;
    }
  GDate *date = NULL;
  if (gst_tag_list_get_date (tags, GST_TAG_DATE, &date))
    {
      char *year = g_date_valid (date) ? g_strdup_printf ("%d", g_date_get_year (date)) : NULL;
      g_date_free (date);
      return year;
    }
  return NULL;
}

static guint discover_timeout_s = DP_DISCOVER_TIMEOUT_S; /* set before any read (tests) */

void
dp_meta_set_discover_timeout (guint seconds)
{
  discover_timeout_s = seconds > 0 ? seconds : DP_DISCOVER_TIMEOUT_S;
}

static gpointer
init_gst (gpointer data)
{
  (void) data;
  return GINT_TO_POINTER (gst_init_check (NULL, NULL, NULL));
}

static DpGroup *
read_media (DpInfo *info, GCancellable *cancellable)
{
  static GOnce once = G_ONCE_INIT;
  if (!GPOINTER_TO_INT (g_once (&once, init_gst, NULL)))
    return NULL;
  g_autoptr (GstDiscoverer) dc = gst_discoverer_new (discover_timeout_s * GST_SECOND, NULL);
  if (dc == NULL || g_cancellable_is_cancelled (cancellable))
    return NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GstDiscovererInfo) di = gst_discoverer_discover_uri (dc, info->uri, &error);
  if (g_cancellable_is_cancelled (cancellable))
    return NULL;
  GstDiscovererResult result = di != NULL ? gst_discoverer_info_get_result (di) : GST_DISCOVERER_ERROR;
  if (result != GST_DISCOVERER_OK && result != GST_DISCOVERER_MISSING_PLUGINS)
    {
      g_debug ("cannot read %s: result %d: %s", info->uri, result, error != NULL ? error->message : "-");
      if (di == NULL || result != GST_DISCOVERER_TIMEOUT)
        return NULL;
      /* A timeout may still have found the streams (slow disks, valgrind): use what is there. */
    }

  GList *video = gst_discoverer_info_get_video_streams (di);
  GList *audio = gst_discoverer_info_get_audio_streams (di);
  /* Cover art is reported as an "image" video stream: only a real video stream makes it a video. */
  GstDiscovererVideoInfo *v = NULL;
  for (GList *l = video; l != NULL && v == NULL; l = l->next)
    if (!gst_discoverer_video_info_is_image (l->data))
      v = l->data;
  GstDiscovererAudioInfo *a = audio != NULL ? audio->data : NULL;
  DpGroup *g = NULL;
  if (v != NULL || a != NULL)
    {
      g = dp_group_new (v != NULL ? "Video" : "Audio");
      g_autoptr (GstTagList) tags = all_tags (di);
      GstClockTime duration = gst_discoverer_info_get_duration (di);
      if (GST_CLOCK_TIME_IS_VALID (duration) && duration > 0)
        dp_group_take (g, "Duration", dp_format_duration (duration), NULL);
      dp_group_take (g, "Title", tag_string (tags, GST_TAG_TITLE), NULL);
      if (v == NULL)
        {
          dp_group_take (g, "Artist", tag_string (tags, GST_TAG_ARTIST), NULL);
          dp_group_take (g, "Album", tag_string (tags, GST_TAG_ALBUM), NULL);
          guint track = 0;
          if (gst_tag_list_get_uint (tags, GST_TAG_TRACK_NUMBER, &track) && track > 0)
            dp_group_take (g, "Track", g_strdup_printf ("%u", track), NULL);
          dp_group_take (g, "Year", tag_year (tags), NULL);
          dp_group_take (g, "Genre", tag_string (tags, GST_TAG_GENRE), NULL);
        }
      if (v != NULL)
        {
          guint w = gst_discoverer_video_info_get_width (v), h = gst_discoverer_video_info_get_height (v);
          dp_group_take (g, "Width", w > 0 ? g_strdup_printf ("%u px", w) : NULL, NULL);
          dp_group_take (g, "Height", h > 0 ? g_strdup_printf ("%u px", h) : NULL, NULL);
          dp_group_take (g, "Frame rate",
                         dp_format_frame_rate (gst_discoverer_video_info_get_framerate_num (v),
                                               gst_discoverer_video_info_get_framerate_denom (v)), NULL);
          dp_group_take (g, "Video codec", codec_name (GST_DISCOVERER_STREAM_INFO (v)), NULL);
          dp_group_take (g, "Video bitrate", dp_format_bitrate (gst_discoverer_video_info_get_bitrate (v)), NULL);
        }
      if (a != NULL)
        {
          const char *codec_label = v != NULL ? "Audio codec" : "Codec";
          dp_group_take (g, codec_label, codec_name (GST_DISCOVERER_STREAM_INFO (a)), NULL);
          dp_group_take (g, v != NULL ? "Audio bitrate" : "Bitrate",
                         dp_format_bitrate (gst_discoverer_audio_info_get_bitrate (a)), NULL);
          dp_group_take (g, "Sample rate", dp_format_sample_rate (gst_discoverer_audio_info_get_sample_rate (a)), NULL);
          dp_group_take (g, "Channels", dp_format_channels (gst_discoverer_audio_info_get_channels (a)), NULL);
        }
      else if (v != NULL)
        dp_group_take (g, "Audio", g_strdup ("None"), NULL);
      if (info->content_type != NULL)
        dp_group_take (g, "Format", g_content_type_get_description (info->content_type), NULL);
    }
  gst_discoverer_stream_info_list_free (video);
  gst_discoverer_stream_info_list_free (audio);
  return g;
}

/* ------------------------------------------------------------------------------------------------ */
/* PDF: poppler                                                                                     */

#ifdef DP_HAVE_POPPLER
static DpGroup *
read_pdf (DpInfo *info, GDateTime *now)
{
  DpGroup *g = dp_group_new ("Document");
  g_autoptr (GError) error = NULL;
  g_autoptr (PopplerDocument) doc = poppler_document_new_from_file (info->uri, NULL, &error);
  if (doc == NULL)
    {
      if (g_error_matches (error, POPPLER_ERROR, POPPLER_ERROR_ENCRYPTED))
        dp_group_take (g, "Encrypted", g_strdup ("Yes, password required"), NULL);
      return g;
    }
  int pages = poppler_document_get_n_pages (doc);
  dp_group_take (g, "Pages", pages > 0 ? g_strdup_printf ("%d", pages) : NULL, NULL);
  if (pages > 0)
    {
      g_autoptr (PopplerPage) page = poppler_document_get_page (doc, 0);
      double w = 0, h = 0;
      if (page != NULL)
        poppler_page_get_size (page, &w, &h);
      dp_group_take (g, "Page size", dp_format_page_size (w, h), NULL);
    }
  dp_group_take (g, "Title", clean (poppler_document_get_title (doc)), NULL);
  dp_group_take (g, "Author", clean (poppler_document_get_author (doc)), NULL);
  dp_group_take (g, "Subject", clean (poppler_document_get_subject (doc)), NULL);
  dp_group_take (g, "Keywords", clean (poppler_document_get_keywords (doc)), NULL);
  dp_group_take (g, "Creator", clean (poppler_document_get_creator (doc)), NULL);
  dp_group_take (g, "Producer", clean (poppler_document_get_producer (doc)), NULL);
  dp_group_take (g, "PDF version", clean (poppler_document_get_pdf_version_string (doc)), NULL);
  g_autoptr (GDateTime) created = poppler_document_get_creation_date_time (doc);
  if (created != NULL)
    dp_group_take (g, "Document created", dp_format_date (created, now), NULL);
  g_autoptr (GDateTime) modified = poppler_document_get_modification_date_time (doc);
  if (modified != NULL)
    dp_group_take (g, "Document modified", dp_format_date (modified, now), NULL);
  if (poppler_document_is_linearized (doc))
    dp_group_take (g, "Fast web view", g_strdup ("Yes"), NULL);
  return g;
}

#endif

GPtrArray *
dp_extra_read (DpInfo        *info,
               GCancellable  *cancellable,
               GError       **error)
{
  GPtrArray *groups = g_ptr_array_new_with_free_func ((GDestroyNotify) dp_group_free);
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    {
      g_ptr_array_unref (groups);
      return NULL;
    }
  /* Network/FUSE files: general rows only (PLAN.md §13.3 rule 3). */
  g_autofree char *path = info->is_native ? g_file_get_path (info->file) : NULL;
  if (path == NULL || info->is_dir)
    return groups;
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  DpGroup *g = NULL;
  switch (info->kind)
    {
    case DP_KIND_IMAGE:
      g = read_image (info, path);
      break;
    case DP_KIND_VIDEO:
    case DP_KIND_AUDIO:
      g = read_media (info, cancellable);
      break;
#ifdef DP_HAVE_POPPLER
    case DP_KIND_PDF:
      if (info->size <= DP_PARSE_MAX_BYTES)
        g = read_pdf (info, now);
      break;
#endif
    default:
      break;
    }
  (void) now; /* only PDFs use it */
  if (g != NULL && g->rows->len > 0)
    g_ptr_array_add (groups, g);
  else
    dp_group_free (g);
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    {
      g_ptr_array_unref (groups);
      return NULL;
    }
  return groups;
}

/* ------------------------------------------------------------------------------------------------ */
/* Folder contents                                                                                  */

typedef struct
{
  GCancellable *cancellable;
  guint         max_entries;
  gint64        deadline;
  const char   *fs_id;        /* Borrowed: filesystem of the top folder (no crossing mount points) */
  DpFolderCount *out;
} Walk;

/* Counts the rest of the top folder's direct children (cheap: no recursion) after the total stopped. */
static void
count_rest (Walk *w, GFileEnumerator *e)
{
  GFileInfo *child = NULL; /* borrowed from the enumerator */
  while (!g_cancellable_is_cancelled (w->cancellable) &&
         g_file_enumerator_iterate (e, &child, NULL, w->cancellable, NULL) && child != NULL)
    {
      if (w->out->items >= w->max_entries)
        {
          w->out->items_truncated = TRUE;
          return;
        }
      w->out->items++;
      if (g_file_info_get_is_hidden (child))
        w->out->hidden++;
    }
}

/* Adds the sizes below @dir to w->out (and, for the top folder, counts its children). FALSE when a limit
 * was hit or on cancellation. */
static gboolean
walk (Walk *w, GFile *dir, gboolean top)
{
  g_autoptr (GFileEnumerator) e = g_file_enumerate_children (
    dir,
    G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE ","
    G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," G_FILE_ATTRIBUTE_ID_FILESYSTEM,
    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, w->cancellable, NULL);
  if (e == NULL)
    return !g_cancellable_is_cancelled (w->cancellable); /* unreadable subfolder: skip it */
  for (;;)
    {
      GFileInfo *child = NULL; /* borrowed from the enumerator */
      GFile *child_file = NULL;
      if (!g_file_enumerator_iterate (e, &child, &child_file, w->cancellable, NULL) || child == NULL)
        break;
      if (top)
        {
          w->out->items++;
          if (g_file_info_get_is_hidden (child))
            w->out->hidden++;
        }
      if (w->out->entries >= w->max_entries || g_get_monotonic_time () > w->deadline)
        {
          w->out->total_truncated = TRUE;
          if (top)
            count_rest (w, e);
          return FALSE;
        }
      w->out->entries++;
      GFileType type = g_file_info_get_file_type (child);
      if (type == G_FILE_TYPE_DIRECTORY)
        {
          const char *fs = g_file_info_get_attribute_string (child, G_FILE_ATTRIBUTE_ID_FILESYSTEM);
          if (w->fs_id != NULL && fs != NULL && !g_str_equal (fs, w->fs_id))
            continue; /* a mount point */
          if (!walk (w, child_file, FALSE))
            {
              if (top)
                count_rest (w, e);
              return FALSE;
            }
        }
      else if (type == G_FILE_TYPE_REGULAR)
        w->out->total += (guint64) g_file_info_get_size (child);
    }
  return !g_cancellable_is_cancelled (w->cancellable);
}

gboolean
dp_folder_count (GFile          *dir,
                 guint           max_entries,
                 gint64          max_us,
                 GCancellable   *cancellable,
                 DpFolderCount  *out,
                 GError        **error)
{
  memset (out, 0, sizeof *out);
  g_autoptr (GFileInfo) top = g_file_query_info (dir, G_FILE_ATTRIBUTE_ID_FILESYSTEM "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                                 G_FILE_QUERY_INFO_NONE, cancellable, error);
  if (top == NULL)
    return FALSE;
  /* Fail early (with the real error) if the folder itself can't be listed. */
  g_autoptr (GFileEnumerator) probe = g_file_enumerate_children (dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                                 G_FILE_QUERY_INFO_NONE, cancellable, error);
  if (probe == NULL)
    return FALSE;
  g_clear_object (&probe);
  Walk w = {
    .cancellable = cancellable,
    .max_entries = max_entries > 0 ? max_entries : DP_COUNT_MAX_ENTRIES,
    .deadline = g_get_monotonic_time () + (max_us > 0 ? max_us : DP_COUNT_MAX_US),
    .fs_id = g_file_info_get_attribute_string (top, G_FILE_ATTRIBUTE_ID_FILESYSTEM),
    .out = out,
  };
  walk (&w, dir, TRUE);
  return !g_cancellable_set_error_if_cancelled (cancellable, error);
}

/* ------------------------------------------------------------------------------------------------ */
/* Multiple selection                                                                               */

void
dp_summary_free (DpSummary *summary)
{
  if (summary == NULL)
    return;
  g_clear_pointer (&summary->infos, g_ptr_array_unref);
  g_clear_pointer (&summary->group, dp_group_free);
  g_free (summary);
}

DpSummary *
dp_summary_read (GPtrArray     *files,
                 guint          n_selected,
                 GCancellable  *cancellable,
                 GError       **error)
{
  g_autoptr (GPtrArray) infos = g_ptr_array_new_with_free_func ((GDestroyNotify) dp_info_unref);
  guint counts[DP_KIND_DOCUMENT + 1] = { 0 };
  guint64 total = 0;
  g_autofree char *location = NULL;
  gboolean same_location = TRUE;
  for (guint i = 0; i < files->len && i < DP_MULTI_MAX; i++)
    {
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        return NULL;
      DpInfo *info = dp_info_read (g_ptr_array_index (files, i), cancellable, NULL);
      if (info == NULL)
        continue;
      counts[info->kind]++;
      if (!info->is_dir)
        total += info->size;
      if (infos->len == 0)
        location = g_strdup (info->location);
      else if (g_strcmp0 (location, info->location) != 0)
        same_location = FALSE;
      g_ptr_array_add (infos, info); /* takes the ref */
    }
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return NULL;

  DpSummary *s = g_new0 (DpSummary, 1); /* returned: caller owns it */
  s->n_selected = n_selected;
  s->n_read = infos->len;
  s->infos = g_steal_pointer (&infos);
  s->group = dp_group_new ("Selection");
  /* Partial: more were selected than read (DP_MULTI_MAX); unreadable files don't count as partial. */
  gboolean partial = MIN (files->len, DP_MULTI_MAX) < n_selected;
  guint n_files = s->n_read - counts[DP_KIND_FOLDER];
  if (n_files > 0)
    {
      g_autofree char *exact = dp_format_size_exact (total);
      dp_group_take (s->group, "Size (files)", dp_format_size (total),
                     partial ? g_strdup_printf ("%s in the first %u selected", exact, s->n_read) : g_steal_pointer (&exact));
    }
  char *kinds = dp_format_kind_counts (counts[DP_KIND_FOLDER], counts[DP_KIND_IMAGE], counts[DP_KIND_VIDEO],
                                       counts[DP_KIND_AUDIO], counts[DP_KIND_PDF] + counts[DP_KIND_DOCUMENT],
                                       counts[DP_KIND_OTHER]);
  dp_group_take (s->group, "Contains", kinds,
                 partial ? g_strdup_printf ("Counted the first %u of %u", s->n_read, n_selected) : NULL);
  dp_group_take (s->group, "Location", same_location ? g_steal_pointer (&location) : g_strdup ("Several folders"),
                 NULL);
  return s;
}

/* ------------------------------------------------------------------------------------------------ */
/* Async wrappers                                                                                   */

static void
info_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  (void) source;
  GError *error = NULL;
  DpInfo *info = dp_info_read (data, cancellable, &error);
  if (info != NULL)
    g_task_return_pointer (task, info, (GDestroyNotify) dp_info_unref);
  else
    g_task_return_error (task, error);
}

void
dp_info_read_async (GFile *file, GCancellable *cancellable, GAsyncReadyCallback cb, gpointer data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, cb, data);
  g_task_set_source_tag (task, dp_info_read_async);
  g_task_set_task_data (task, g_object_ref (file), g_object_unref);
  g_task_run_in_thread (task, info_thread);
}

DpInfo *
dp_info_read_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
extra_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  (void) source;
  GError *error = NULL;
  GPtrArray *groups = dp_extra_read (data, cancellable, &error);
  if (groups != NULL)
    g_task_return_pointer (task, groups, (GDestroyNotify) g_ptr_array_unref);
  else
    g_task_return_error (task, error);
}

void
dp_extra_read_async (DpInfo *info, GCancellable *cancellable, GAsyncReadyCallback cb, gpointer data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, cb, data);
  g_task_set_source_tag (task, dp_extra_read_async);
  g_task_set_task_data (task, dp_info_ref (info), (GDestroyNotify) dp_info_unref);
  g_task_run_in_thread (task, extra_thread);
}

GPtrArray *
dp_extra_read_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
count_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  (void) source;
  GError *error = NULL;
  DpFolderCount *count = g_new0 (DpFolderCount, 1);
  if (dp_folder_count (data, 0, 0, cancellable, count, &error))
    g_task_return_pointer (task, count, g_free);
  else
    {
      g_free (count);
      g_task_return_error (task, error);
    }
}

void
dp_folder_count_async (GFile *dir, GCancellable *cancellable, GAsyncReadyCallback cb, gpointer data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, cb, data);
  g_task_set_source_tag (task, dp_folder_count_async);
  g_task_set_task_data (task, g_object_ref (dir), g_object_unref);
  g_task_run_in_thread (task, count_thread);
}

gboolean
dp_folder_count_finish (GAsyncResult *result, DpFolderCount *out, GError **error)
{
  g_autofree DpFolderCount *count = g_task_propagate_pointer (G_TASK (result), error);
  if (count == NULL)
    return FALSE;
  *out = *count;
  return TRUE;
}

typedef struct
{
  GPtrArray *files; /* Owned; GFile* (owned) */
  guint      n_selected;
} SummaryData;

static void
summary_data_free (gpointer data)
{
  SummaryData *d = data;
  g_ptr_array_unref (d->files);
  g_free (d);
}

static void
summary_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  (void) source;
  SummaryData *d = data;
  GError *error = NULL;
  DpSummary *s = dp_summary_read (d->files, d->n_selected, cancellable, &error);
  if (s != NULL)
    g_task_return_pointer (task, s, (GDestroyNotify) dp_summary_free);
  else
    g_task_return_error (task, error);
}

void
dp_summary_read_async (GPtrArray *files, guint n_selected, GCancellable *cancellable, GAsyncReadyCallback cb,
                       gpointer data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, cb, data);
  g_task_set_source_tag (task, dp_summary_read_async);
  SummaryData *d = g_new0 (SummaryData, 1);
  d->files = g_ptr_array_new_full (files->len, g_object_unref);
  for (guint i = 0; i < files->len && i < DP_MULTI_MAX; i++)
    g_ptr_array_add (d->files, g_object_ref (g_ptr_array_index (files, i)));
  d->n_selected = n_selected;
  g_task_set_task_data (task, d, summary_data_free);
  g_task_run_in_thread (task, summary_thread);
}

DpSummary *
dp_summary_read_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}
