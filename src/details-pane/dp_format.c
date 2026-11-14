/* Details pane formatting (PLAN.md §13.1.2–§13.1.3). See dp_format.h. */
#include "dp_format.h"

#include <math.h>
#include <string.h>

/* Unix mode bits (POSIX values; spelled out so no feature-test macros are needed under -std=c11). */
#define M_SUID 04000
#define M_SGID 02000
#define M_SVTX 01000

char *
dp_format_size (guint64 bytes)
{
  return g_format_size (bytes);
}

char *
dp_format_size_exact (guint64 bytes)
{
  /* %'… groups digits the locale's way (GLib's printf supports the ' flag). */
  return g_strdup_printf (bytes == 1 ? "%'" G_GUINT64_FORMAT " byte" : "%'" G_GUINT64_FORMAT " bytes", bytes);
}

char *
dp_format_date (GDateTime *when,
                GDateTime *now)
{
  g_return_val_if_fail (when != NULL && now != NULL, NULL);
  g_autoptr (GDateTime) local = g_date_time_to_local (when);
  g_autoptr (GDateTime) local_now = g_date_time_to_local (now);
  int y, m, d, ny, nm, nd;
  g_date_time_get_ymd (local, &y, &m, &d);
  g_date_time_get_ymd (local_now, &ny, &nm, &nd);
  g_autoptr (GDateTime) yesterday = g_date_time_add_days (local_now, -1);
  int yy, ym, yd;
  g_date_time_get_ymd (yesterday, &yy, &ym, &yd);
  g_autofree char *time = g_date_time_format (local, "%H:%M");
  if (y == ny && m == nm && d == nd)
    return g_strdup_printf ("Today, %s", time);
  if (y == yy && m == ym && d == yd)
    return g_strdup_printf ("Yesterday, %s", time);
  g_autofree char *day = g_date_time_format (local, "%-d %B %Y");
  return g_strdup_printf ("%s, %s", day, time);
}

char *
dp_format_duration (guint64 ns)
{
  guint64 s = ns / G_GUINT64_CONSTANT (1000000000);
  guint64 h = s / 3600, mins = (s / 60) % 60, secs = s % 60;
  if (h > 0)
    return g_strdup_printf ("%" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT, h, mins, secs);
  return g_strdup_printf ("%" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT, mins, secs);
}

/* "7.06", "44.1", "48": up to @decimals decimals, trailing zeros dropped. New string. */
static char *
trim_number (double v, int decimals)
{
  g_autofree char *s = g_strdup_printf ("%.*f", decimals, v);
  if (decimals > 0 && strchr (s, '.') != NULL)
    {
      char *end = s + strlen (s) - 1;
      while (*end == '0')
        *end-- = '\0';
      if (*end == '.')
        *end = '\0';
    }
  return g_steal_pointer (&s);
}

char *
dp_format_bitrate (guint bits_per_second)
{
  if (bits_per_second == 0)
    return NULL;
  if (bits_per_second >= 1000000)
    {
      g_autofree char *n = trim_number (bits_per_second / 1e6, 2);
      return g_strdup_printf ("%s Mb/s", n);
    }
  return g_strdup_printf ("%u kb/s", (guint) lround (bits_per_second / 1e3));
}

char *
dp_format_sample_rate (guint hz)
{
  if (hz == 0)
    return NULL;
  g_autofree char *n = trim_number (hz / 1e3, 1);
  return g_strdup_printf ("%s kHz", n);
}

char *
dp_format_channels (guint channels)
{
  switch (channels)
    {
    case 0:
      return NULL;
    case 1:
      return g_strdup ("Mono");
    case 2:
      return g_strdup ("Stereo");
    case 6:
      return g_strdup ("5.1 surround");
    case 8:
      return g_strdup ("7.1 surround");
    default:
      return g_strdup_printf ("%u channels", channels);
    }
}

char *
dp_format_frame_rate (guint num,
                      guint den)
{
  if (num == 0 || den == 0)
    return NULL;
  g_autofree char *n = trim_number ((double) num / den, 2);
  return g_strdup_printf ("%s fps", n);
}

char *
dp_format_dimensions (guint width,
                      guint height)
{
  if (width == 0 || height == 0)
    return NULL;
  return g_strdup_printf ("%u × %u px", width, height);
}

typedef struct
{
  const char *name;
  double      w_mm, h_mm;
  gboolean    inches;
} Paper;

static const Paper papers[] = {
  { "A3", 297, 420, FALSE },     { "A4", 210, 297, FALSE },        { "A5", 148, 210, FALSE },
  { "A6", 105, 148, FALSE },     { "B5", 176, 250, FALSE },        { "Letter", 215.9, 279.4, TRUE },
  { "Legal", 215.9, 355.6, TRUE }, { "Tabloid", 279.4, 431.8, TRUE },
};

char *
dp_format_page_size (double width_pt,
                     double height_pt)
{
  if (!(width_pt > 0) || !(height_pt > 0))
    return NULL;
  double w = width_pt * 25.4 / 72, h = height_pt * 25.4 / 72;
  gboolean landscape = w > h + 0.5;
  double s = MIN (w, h), l = MAX (w, h);
  const char *suffix = landscape ? ", landscape" : "";
  for (gsize i = 0; i < G_N_ELEMENTS (papers); i++)
    if (fabs (s - papers[i].w_mm) < 2 && fabs (l - papers[i].h_mm) < 2)
      {
        if (papers[i].inches)
          {
            g_autofree char *a = trim_number (papers[i].w_mm / 25.4, 1);
            g_autofree char *b = trim_number (papers[i].h_mm / 25.4, 1);
            return g_strdup_printf ("%s (%s × %s in)%s", papers[i].name, a, b, suffix);
          }
        return g_strdup_printf ("%s (%.0f × %.0f mm)%s", papers[i].name, papers[i].w_mm, papers[i].h_mm, suffix);
      }
  return g_strdup_printf ("%.0f × %.0f mm%s", w, h, suffix);
}

char *
dp_format_item_count (guint    items,
                      guint    hidden,
                      gboolean truncated)
{
  if (items == 0 && !truncated)
    return g_strdup ("Empty folder");
  const char *more = truncated ? "more than " : "";
  g_autofree char *main = g_strdup_printf (items == 1 && !truncated ? "%s%u item" : "%s%u items", more, items);
  if (hidden == 0)
    return g_steal_pointer (&main);
  return g_strdup_printf ("%s, %u hidden", main, hidden);
}

char *
dp_format_kind_counts (guint folders,
                       guint images,
                       guint videos,
                       guint audio,
                       guint documents,
                       guint other)
{
  const struct
  {
    guint       n;
    const char *one, *many;
  } parts[] = {
    { folders, "folder", "folders" },   { images, "image", "images" },
    { videos, "video", "videos" },      { audio, "audio file", "audio files" },
    { documents, "document", "documents" }, { other, "other file", "other files" },
  };
  GString *s = g_string_new (NULL);
  for (gsize i = 0; i < G_N_ELEMENTS (parts); i++)
    if (parts[i].n > 0)
      g_string_append_printf (s, "%s%u %s", s->len > 0 ? ", " : "", parts[i].n,
                              parts[i].n == 1 ? parts[i].one : parts[i].many);
  return g_string_free (s, FALSE);
}

/* Permissions ---------------------------------------------------------------------------------------- */

static GStrv
chips (gboolean r, gboolean w, gboolean x, gboolean is_dir)
{
  g_autoptr (GStrvBuilder) b = g_strv_builder_new ();
  if (r)
    g_strv_builder_add (b, is_dir ? "List files" : "Read");
  if (w)
    g_strv_builder_add (b, is_dir ? "Create & delete" : "Write");
  if (x)
    g_strv_builder_add (b, is_dir ? "Open" : "Run");
  if (!r && !w && !x)
    g_strv_builder_add (b, "No access");
  return g_strv_builder_end (b);
}

GStrv
dp_permission_chips (guint32  mode,
                     DpWho    who,
                     gboolean is_dir)
{
  static const guint32 bits[3][3] = {
    { 0400, 0200, 0100 }, /* owner */
    { 0040, 0020, 0010 }, /* group */
    { 0004, 0002, 0001 }, /* others */
  };
  g_return_val_if_fail (who <= DP_WHO_OTHERS, NULL);
  return chips ((mode & bits[who][0]) != 0, (mode & bits[who][1]) != 0, (mode & bits[who][2]) != 0, is_dir);
}

GStrv
dp_access_chips (gboolean can_read,
                 gboolean can_write,
                 gboolean can_execute,
                 gboolean is_dir)
{
  return chips (can_read, can_write, can_execute, is_dir);
}

GStrv
dp_special_chips (guint32 mode)
{
  g_autoptr (GStrvBuilder) b = g_strv_builder_new ();
  if (mode & M_SUID)
    g_strv_builder_add (b, "Set user ID");
  if (mode & M_SGID)
    g_strv_builder_add (b, "Set group ID");
  if (mode & M_SVTX)
    g_strv_builder_add (b, "Sticky");
  return g_strv_builder_end (b);
}

char *
dp_chip_accessible_label (const char *who_label,
                          const char *chip)
{
  static const struct
  {
    const char *chip, *verb;
  } verbs[] = {
    { "Read", "can read" },           { "Write", "can write" },
    { "Run", "can run" },             { "List files", "can list files" },
    { "Create & delete", "can create and delete files" }, { "Open", "can open" },
    { "Set user ID", "set user ID" }, { "Set group ID", "set group ID" },
    { "Sticky", "sticky" },
  };
  if (g_strcmp0 (chip, "No access") == 0)
    return g_strdup_printf ("%s %s no access", who_label, g_strcmp0 (who_label, "You") == 0 ||
                                                         g_strcmp0 (who_label, "Others") == 0 ? "have" : "has");
  for (gsize i = 0; i < G_N_ELEMENTS (verbs); i++)
    if (g_strcmp0 (chip, verbs[i].chip) == 0)
      return g_strdup_printf ("%s %s", who_label, verbs[i].verb);
  return g_strdup_printf ("%s: %s", who_label, chip);
}
