/* Details pane readers (PLAN.md §13.2, §13.3) on the committed fixtures (data/make-fixtures.sh). */
#include <glib/gstdio.h>
#include <locale.h>

#include "config.h"
#include "dp_meta.h"
#include "test-util.h"

static char *work; /* Owned: per-run copy of the fixtures, under the build dir (PLAN.md §4.2). */

static const char *const fixtures[] = {
  "photo.jpg", "pic.png", "pic.svg", "clip.webm", "clip.mp4", "tone.ogg", "doc.pdf", "broken.pdf", "notes.txt",
};

static GFile *
fixture (const char *name)
{
  g_autofree char *path = g_build_filename (work, name, NULL);
  return g_file_new_for_path (path);
}

static DpInfo *
read_info (const char *name)
{
  g_autoptr (GFile) f = fixture (name);
  g_autoptr (GError) error = NULL;
  DpInfo *info = dp_info_read (f, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (info);
  return info;
}

/* The one extra group for @name (asserts there is exactly one, titled @title). Caller frees the array. */
static GPtrArray *
read_extra (const char *name, const char *title)
{
  g_autoptr (DpInfo) info = read_info (name);
  g_autoptr (GError) error = NULL;
  GPtrArray *groups = dp_extra_read (info, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (groups);
  if (title == NULL)
    g_assert_cmpuint (groups->len, ==, 0);
  else
    {
      g_assert_cmpuint (groups->len, ==, 1);
      g_assert_cmpstr (((DpGroup *) g_ptr_array_index (groups, 0))->title, ==, title);
    }
  return groups;
}

static void
assert_row (GPtrArray *groups, const char *label, const char *value)
{
  const DpGroup *g = g_ptr_array_index (groups, 0);
  const DpRow *row = dp_group_find (g, label);
  if (row == NULL)
    g_error ("no row \"%s\" in group %s", label, g->title);
  if (value != NULL)
    g_assert_cmpstr (row->value, ==, value);
}

static void
assert_no_row (GPtrArray *groups, const char *label)
{
  g_assert_null (dp_group_find (g_ptr_array_index (groups, 0), label));
}

static void
test_info_file (void)
{
  g_autoptr (DpInfo) info = read_info ("photo.jpg");
  g_assert_cmpstr (info->display_name, ==, "photo.jpg");
  g_assert_cmpint (info->kind, ==, DP_KIND_IMAGE);
  g_assert_false (info->is_dir);
  g_assert_true (info->is_native);
  g_assert_cmpstr (info->location, ==, work);
  g_assert_cmpuint (info->size, >, 0);
  g_assert_nonnull (info->modified);
  g_assert_nonnull (info->icon);
  g_assert_true (info->perms.has_mode);
  g_assert_true (info->perms.has_access);
  g_assert_true (info->perms.can_read);
  g_assert_nonnull (info->perms.owner);

  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autoptr (DpGroup) d = dp_details_group (info, NULL, now);
  g_assert_cmpstr (dp_group_find (d, "Type")->value, ==, "JPEG image");
  g_assert_cmpstr (dp_group_find (d, "Type")->tooltip, ==, "image/jpeg");
  g_assert_nonnull (dp_group_find (d, "Size"));
  g_assert_cmpstr (dp_group_find (d, "Location")->value, ==, work);
  g_assert_true (g_str_has_prefix (dp_group_find (d, "Modified")->value, "Today, "));
  g_assert_null (dp_group_find (d, "Contents"));
  g_assert_null (dp_group_find (d, "Link target"));
}

static void
test_kinds (void)
{
  static const struct
  {
    const char *name;
    DpKind      kind;
  } cases[] = {
    { "pic.png", DP_KIND_IMAGE }, { "pic.svg", DP_KIND_IMAGE },  { "clip.webm", DP_KIND_VIDEO },
    { "clip.mp4", DP_KIND_VIDEO }, { "tone.ogg", DP_KIND_AUDIO }, { "doc.pdf", DP_KIND_PDF },
    { "notes.txt", DP_KIND_DOCUMENT },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autoptr (DpInfo) info = read_info (cases[i].name);
      g_assert_cmpint (info->kind, ==, cases[i].kind);
    }
  g_assert_cmpint (dp_kind_for_content_type (NULL, FALSE), ==, DP_KIND_OTHER);
  g_assert_cmpint (dp_kind_for_content_type ("application/x-tar", FALSE), ==, DP_KIND_OTHER);
  g_assert_cmpint (dp_kind_for_content_type ("application/x-tar", TRUE), ==, DP_KIND_FOLDER);
}

static void
test_info_folder_and_symlink (void)
{
  g_autofree char *dir = tu_mkdir (work, "folder");
  tu_touch (dir, "a.txt");
  tu_touch (dir, ".hidden");
  g_autoptr (GFile) df = g_file_new_for_path (dir);
  g_autoptr (DpInfo) info = dp_info_read (df, NULL, NULL);
  g_assert_nonnull (info);
  g_assert_true (info->is_dir);
  g_assert_cmpint (info->kind, ==, DP_KIND_FOLDER);
  g_assert_cmpstr (info->description, ==, "Folder");

  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autoptr (DpGroup) pending = dp_details_group (info, NULL, now);
  g_assert_cmpstr (dp_group_find (pending, "Contents")->value, ==, "Counting…");
  g_assert_cmpstr (dp_group_find (pending, "Total size")->value, ==, "Calculating…");
  g_assert_null (dp_group_find (pending, "Size"));
  DpFolderCount count = { 0 };
  g_assert_true (dp_folder_count (df, 0, 0, NULL, &count, NULL));
  g_autoptr (DpGroup) done = dp_details_group (info, &count, now);
  g_assert_cmpstr (dp_group_find (done, "Contents")->value, ==, "2 items, 1 hidden");
  g_assert_cmpstr (dp_group_find (done, "Total size")->value, ==, "2 bytes");

  g_autofree char *link = g_build_filename (work, "link.jpg", NULL);
  g_autoptr (GFile) lf = g_file_new_for_path (link);
  g_assert_true (g_file_make_symbolic_link (lf, "photo.jpg", NULL, NULL));
  g_autoptr (DpInfo) li = dp_info_read (lf, NULL, NULL);
  g_assert_true (li->is_symlink);
  g_assert_cmpstr (li->symlink_target, ==, "photo.jpg");
  g_assert_cmpstr (li->display_name, ==, "link.jpg");
  g_assert_cmpint (li->kind, ==, DP_KIND_IMAGE); /* what it points to */
  g_autoptr (DpGroup) lg = dp_details_group (li, NULL, now);
  g_assert_cmpstr (dp_group_find (lg, "Link target")->value, ==, "photo.jpg");
}

static void
test_info_errors (void)
{
  g_autoptr (GFile) missing = fixture ("does-not-exist");
  g_autoptr (GError) error = NULL;
  g_assert_null (dp_info_read (missing, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);
  g_autoptr (GCancellable) cancel = g_cancellable_new ();
  g_cancellable_cancel (cancel);
  g_autoptr (GFile) f = fixture ("photo.jpg");
  g_assert_null (dp_info_read (f, cancel, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&error);
  g_autoptr (DpInfo) info = read_info ("photo.jpg");
  g_assert_null (dp_extra_read (info, cancel, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

static void
test_image_jpeg (void)
{
  g_autoptr (GPtrArray) g = read_extra ("photo.jpg", "Image");
  assert_row (g, "Dimensions", "120 × 80 px");
  assert_row (g, "Width", "120 px");
  assert_row (g, "Height", "80 px");
  assert_row (g, "Format", "JPEG");
#ifdef DP_HAVE_GEXIV2
  assert_row (g, "Camera", "Canon EOS R6"); /* model already starts with the make: not repeated */
  assert_row (g, "ISO", "400");
  assert_row (g, "Aperture", "F2.8");
  assert_row (g, "Exposure", "1/250 s");
  assert_row (g, "Focal length", NULL);
  assert_row (g, "Date taken", "9 March 2024, 12:34");
  assert_no_row (g, "Orientation"); /* normal orientation: no row */
#endif
}

static void
test_image_png_svg (void)
{
  g_autoptr (GPtrArray) png = read_extra ("pic.png", "Image");
  assert_row (png, "Dimensions", "30 × 20 px");
  assert_row (png, "Format", "PNG");
  assert_no_row (png, "Camera");
  g_autoptr (GPtrArray) svg = read_extra ("pic.svg", "Image");
  assert_row (svg, "Dimensions", "40 × 25 px");
  assert_row (svg, "Format", "SVG");
}

static void
test_video (void)
{
  g_autoptr (GPtrArray) webm = read_extra ("clip.webm", "Video");
  assert_row (webm, "Duration", "0:00");
  assert_row (webm, "Width", "64 px");
  assert_row (webm, "Height", "48 px");
  assert_row (webm, "Frame rate", "10 fps");
  assert_row (webm, "Video codec", "VP8");
  assert_row (webm, "Audio codec", "Vorbis");
  assert_row (webm, "Channels", "Stereo");
  assert_row (webm, "Sample rate", "44.1 kHz");
  assert_row (webm, "Format", "WebM video");
  g_autoptr (GPtrArray) mp4 = read_extra ("clip.mp4", "Video");
  const DpRow *codec = dp_group_find (g_ptr_array_index (mp4, 0), "Video codec");
  g_assert_nonnull (codec);
  g_assert_true (g_str_has_prefix (codec->value, "H.264"));
  assert_row (mp4, "Audio codec", "MPEG-4 AAC");
  assert_row (mp4, "Channels", "Mono");
  assert_row (mp4, "Format", "MPEG-4 video");
}

static void
test_audio (void)
{
  g_autoptr (GPtrArray) g = read_extra ("tone.ogg", "Audio");
  assert_row (g, "Duration", "0:00");
  assert_row (g, "Title", "Test Tone");
  assert_row (g, "Artist", "Nautilus Utils");
  assert_row (g, "Album", "Fixtures");
  assert_row (g, "Track", "3");
  assert_row (g, "Codec", "Vorbis");
  assert_row (g, "Channels", "Stereo");
  assert_row (g, "Sample rate", "44.1 kHz");
  assert_no_row (g, "Width");
  assert_no_row (g, "Video codec");
}

static void
test_pdf (void)
{
#ifdef DP_HAVE_POPPLER
  g_autoptr (GPtrArray) g = read_extra ("doc.pdf", "Document");
  assert_row (g, "Pages", "2");
  assert_row (g, "Page size", "A4 (210 × 297 mm)");
  assert_row (g, "Title", "Fixture Document");
  assert_row (g, "Author", "Nautilus Utils");
  assert_row (g, "Subject", "Tests");
  assert_row (g, "Keywords", "pdf, fixture");
  assert_row (g, "Creator", "make-fixtures.sh");
  assert_row (g, "Producer", "python3");
  assert_row (g, "PDF version", "PDF-1.5");
  assert_row (g, "Document created", "9 March 2024, 12:00");
  g_autoptr (GPtrArray) broken = read_extra ("broken.pdf", NULL); /* unreadable: no group, no error */
#else
  g_test_skip ("built without poppler-glib");
#endif
}

static void
test_no_extra (void)
{
  g_autoptr (GPtrArray) txt = read_extra ("notes.txt", NULL);
  g_autofree char *dir = tu_mkdir (work, "no-extra-dir");
  g_autoptr (GFile) df = g_file_new_for_path (dir);
  g_autoptr (DpInfo) info = dp_info_read (df, NULL, NULL);
  g_autoptr (GPtrArray) groups = dp_extra_read (info, NULL, NULL);
  g_assert_cmpuint (groups->len, ==, 0);
}

static void
test_folder_count (void)
{
  g_autofree char *top = tu_mkdir (work, "count");
  tu_touch_n (top, "f%03u.txt", 5); /* tu_touch writes 1 byte */
  tu_touch (top, ".dot");
  g_autofree char *sub = tu_mkdir (top, "sub/deeper");
  tu_touch_n (sub, "d%03u.txt", 20);
  g_autoptr (GFile) f = g_file_new_for_path (top);
  DpFolderCount c;
  g_assert_true (dp_folder_count (f, 0, 0, NULL, &c, NULL));
  g_assert_cmpuint (c.items, ==, 7); /* 5 files, .dot, sub */
  g_assert_cmpuint (c.hidden, ==, 1);
  g_assert_false (c.items_truncated);
  g_assert_false (c.total_truncated);
  g_assert_cmpuint (c.total, ==, 26);
  g_assert_cmpuint (c.entries, ==, 7 + 1 + 20);

  /* Entry limit below the tree's 28 entries: the total stops, the 7 direct children are still counted. */
  g_assert_true (dp_folder_count (f, 8, 0, NULL, &c, NULL));
  g_assert_true (c.total_truncated);
  g_assert_cmpuint (c.items, ==, 7);
  g_assert_cmpuint (c.hidden, ==, 1);
  g_assert_false (c.items_truncated);
  g_assert_cmpuint (c.entries, <=, 8);
  /* More direct children than the limit: the item count is a lower bound too. */
  g_assert_true (dp_folder_count (f, 1, 0, NULL, &c, NULL));
  g_assert_true (c.items_truncated);
  g_assert_cmpuint (c.items, >=, 1);
  g_assert_cmpuint (c.items, <, 7);
  /* Time limit already passed. */
  g_assert_true (dp_folder_count (f, 0, 1, NULL, &c, NULL));
  g_assert_cmpuint (c.items, ==, 7);

  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) missing = fixture ("no-such-dir");
  g_assert_false (dp_folder_count (missing, 0, 0, NULL, &c, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);
  g_autoptr (GFile) file = fixture ("notes.txt");
  g_assert_false (dp_folder_count (file, 0, 0, NULL, &c, &error));
  g_assert_nonnull (error);
  g_clear_error (&error);
  g_autoptr (GCancellable) cancel = g_cancellable_new ();
  g_cancellable_cancel (cancel);
  g_assert_false (dp_folder_count (f, 0, 0, cancel, &c, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

static void
test_summary (void)
{
  g_autoptr (GPtrArray) files = g_ptr_array_new_with_free_func (g_object_unref);
  for (const char *const *n = (const char *[]) { "photo.jpg", "pic.png", "clip.webm", "doc.pdf", "missing", NULL };
       *n != NULL; n++)
    g_ptr_array_add (files, fixture (*n));
  g_autofree char *dir = tu_mkdir (work, "sum-dir");
  g_ptr_array_add (files, g_file_new_for_path (dir));
  g_autoptr (DpSummary) s = dp_summary_read (files, files->len, NULL, NULL);
  g_assert_nonnull (s);
  g_assert_cmpuint (s->n_selected, ==, 6);
  g_assert_cmpuint (s->n_read, ==, 5); /* "missing" skipped */
  g_assert_cmpuint (s->infos->len, ==, 5);
  g_assert_cmpstr (dp_group_find (s->group, "Contains")->value, ==, "1 folder, 2 images, 1 video, 1 document");
  g_assert_cmpstr (dp_group_find (s->group, "Location")->value, ==, work);
  g_assert_nonnull (dp_group_find (s->group, "Size (files)"));

  /* Different folders; only folders (no size row). */
  g_autoptr (GPtrArray) dirs = g_ptr_array_new_with_free_func (g_object_unref);
  g_ptr_array_add (dirs, g_file_new_for_path (dir));
  g_ptr_array_add (dirs, g_file_new_for_path (work));
  g_autoptr (DpSummary) s2 = dp_summary_read (dirs, 2, NULL, NULL);
  g_assert_cmpstr (dp_group_find (s2->group, "Location")->value, ==, "Several folders");
  g_assert_null (dp_group_find (s2->group, "Size (files)"));

  /* More selected than read: the counts say so. */
  g_autoptr (DpSummary) s3 = dp_summary_read (dirs, 1000, NULL, NULL);
  g_assert_nonnull (dp_group_find (s3->group, "Contains")->tooltip);

  g_autoptr (GCancellable) cancel = g_cancellable_new ();
  g_cancellable_cancel (cancel);
  g_autoptr (GError) error = NULL;
  g_assert_null (dp_summary_read (files, files->len, cancel, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

/* Async wrappers ------------------------------------------------------------------------------------ */

typedef struct
{
  guint done, ok, cancelled;
} Tally;

static void
tally_result (Tally *t, gboolean ok, GError *error)
{
  t->done++;
  if (ok)
    t->ok++;
  else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    t->cancelled++;
}

static void
on_info (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  g_autoptr (GError) error = NULL;
  g_autoptr (DpInfo) info = dp_info_read_finish (res, &error);
  tally_result (data, info != NULL, error);
}

static void
on_extra (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) groups = dp_extra_read_finish (res, &error);
  tally_result (data, groups != NULL, error);
}

static void
on_count (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  g_autoptr (GError) error = NULL;
  DpFolderCount c;
  gboolean ok = dp_folder_count_finish (res, &c, &error);
  tally_result (data, ok, error);
}

static void
on_summary (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  g_autoptr (GError) error = NULL;
  g_autoptr (DpSummary) s = dp_summary_read_finish (res, &error);
  tally_result (data, s != NULL, error);
}

static void
test_async (void)
{
  Tally t = { 0 };
  g_autoptr (GFile) f = fixture ("clip.webm");
  g_autoptr (GFile) dir = g_file_new_for_path (work);
  g_autoptr (DpInfo) info = read_info ("clip.webm");
  g_autoptr (GPtrArray) files = g_ptr_array_new_with_free_func (g_object_unref);
  g_ptr_array_add (files, g_object_ref (f));
  dp_info_read_async (f, NULL, on_info, &t);
  dp_extra_read_async (info, NULL, on_extra, &t);
  dp_folder_count_async (dir, NULL, on_count, &t);
  dp_summary_read_async (files, 1, NULL, on_summary, &t);
  tu_wait_for (&t.done, 4);
  g_assert_cmpuint (t.ok, ==, 4);
}

/* Selection changes cancel the previous reads (PLAN.md §13.3 rule 1): many start → cancel → restart cycles,
 * with the cancelled reads finishing in any order. */
static void
test_async_cancel_stress (void)
{
  Tally t = { 0 };
  g_autoptr (GFile) dir = g_file_new_for_path (work);
  guint started = 0;
  for (guint i = 0; i < 200; i++)
    {
      g_autoptr (GCancellable) cancel = g_cancellable_new ();
      const char *name = fixtures[i % G_N_ELEMENTS (fixtures)];
      g_autoptr (GFile) f = fixture (name);
      g_autoptr (DpInfo) info = read_info (name);
      dp_info_read_async (f, cancel, on_info, &t);
      dp_extra_read_async (info, cancel, on_extra, &t);
      dp_folder_count_async (dir, cancel, on_count, &t);
      started += 3;
      if (i % 3 != 0)
        g_cancellable_cancel (cancel);
      if (i % 10 == 0)
        while (g_main_context_iteration (NULL, FALSE))
          ;
    }
  tu_wait_for (&t.done, started);
  g_assert_cmpuint (t.ok + t.cancelled, ==, started);
  g_assert_cmpuint (t.ok, >=, 67 * 3);
}

int
main (int argc, char **argv)
{
  setlocale (LC_ALL, "");
  g_test_init (&argc, &argv, NULL);
  dp_meta_set_discover_timeout (120); /* valgrind runs ~20× slower than the 3 s production limit */
  work = tu_make_tmpdir_in (g_getenv ("G_TEST_BUILDDIR"), "dp-meta");
  const char *src = g_test_get_dir (G_TEST_DIST);
  g_autofree char *data = g_build_filename (src, "data", NULL);
  for (gsize i = 0; i < G_N_ELEMENTS (fixtures); i++)
    tu_copy (data, fixtures[i], work, fixtures[i]);

  g_test_add_func ("/dp-meta/info-file", test_info_file);
  g_test_add_func ("/dp-meta/kinds", test_kinds);
  g_test_add_func ("/dp-meta/info-folder-symlink", test_info_folder_and_symlink);
  g_test_add_func ("/dp-meta/info-errors", test_info_errors);
  g_test_add_func ("/dp-meta/image-jpeg", test_image_jpeg);
  g_test_add_func ("/dp-meta/image-png-svg", test_image_png_svg);
  g_test_add_func ("/dp-meta/video", test_video);
  g_test_add_func ("/dp-meta/audio", test_audio);
  g_test_add_func ("/dp-meta/pdf", test_pdf);
  g_test_add_func ("/dp-meta/no-extra", test_no_extra);
  g_test_add_func ("/dp-meta/folder-count", test_folder_count);
  g_test_add_func ("/dp-meta/summary", test_summary);
  g_test_add_func ("/dp-meta/async", test_async);
  g_test_add_func ("/dp-meta/async-cancel-stress", test_async_cancel_stress);
  int ret = g_test_run ();
  tu_rm_rf (work);
  g_free (work);
  return ret;
}
