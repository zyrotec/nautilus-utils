/* Details pane formatting (PLAN.md §13.1.2, §13.1.3). */
#include <locale.h>

#include "dp_format.h"

#define ASSERT_STR(expr, expected)                                                                        \
  G_STMT_START                                                                                            \
  {                                                                                                       \
    g_autofree char *s_ = (expr);                                                                         \
    g_assert_cmpstr (s_, ==, (expected));                                                                 \
  }                                                                                                       \
  G_STMT_END

static void
assert_strv (GStrv v, const char *const *expected)
{
  g_assert_nonnull (v);
  g_assert_cmpuint (g_strv_length (v), ==, g_strv_length ((char **) expected));
  for (guint i = 0; expected[i] != NULL; i++)
    g_assert_cmpstr (v[i], ==, expected[i]);
  g_strfreev (v);
}

static void
test_sizes (void)
{
  ASSERT_STR (dp_format_size (0), "0 bytes");
  ASSERT_STR (dp_format_size (34600), "34.6\u00a0kB");
  ASSERT_STR (dp_format_size (2660000), "2.7\u00a0MB");
  ASSERT_STR (dp_format_size_exact (1), "1 byte");
  ASSERT_STR (dp_format_size_exact (1234567), "1,234,567 bytes");
}

static void
test_media_values (void)
{
  ASSERT_STR (dp_format_duration (0), "0:00");
  ASSERT_STR (dp_format_duration (999999999), "0:00");
  ASSERT_STR (dp_format_duration (G_GUINT64_CONSTANT (5110000000)), "0:05");
  ASSERT_STR (dp_format_duration (G_GUINT64_CONSTANT (754000000000)), "12:34");
  ASSERT_STR (dp_format_duration (G_GUINT64_CONSTANT (3723000000000)), "1:02:03");
  g_assert_null (dp_format_bitrate (0));
  ASSERT_STR (dp_format_bitrate (80000), "80 kb/s");
  ASSERT_STR (dp_format_bitrate (69564), "70 kb/s");
  ASSERT_STR (dp_format_bitrate (7057010), "7.06 Mb/s");
  ASSERT_STR (dp_format_bitrate (2000000), "2 Mb/s");
  g_assert_null (dp_format_sample_rate (0));
  ASSERT_STR (dp_format_sample_rate (44100), "44.1 kHz");
  ASSERT_STR (dp_format_sample_rate (48000), "48 kHz");
  g_assert_null (dp_format_channels (0));
  ASSERT_STR (dp_format_channels (1), "Mono");
  ASSERT_STR (dp_format_channels (2), "Stereo");
  ASSERT_STR (dp_format_channels (6), "5.1 surround");
  ASSERT_STR (dp_format_channels (8), "7.1 surround");
  ASSERT_STR (dp_format_channels (4), "4 channels");
  g_assert_null (dp_format_frame_rate (30, 0));
  g_assert_null (dp_format_frame_rate (0, 1));
  ASSERT_STR (dp_format_frame_rate (30, 1), "30 fps");
  ASSERT_STR (dp_format_frame_rate (30000, 1001), "29.97 fps");
  ASSERT_STR (dp_format_frame_rate (25, 1), "25 fps");
  g_assert_null (dp_format_dimensions (0, 5));
  ASSERT_STR (dp_format_dimensions (1920, 1080), "1920 × 1080 px");
}

static void
test_page_sizes (void)
{
  ASSERT_STR (dp_format_page_size (595.28, 841.89), "A4 (210 × 297 mm)");
  ASSERT_STR (dp_format_page_size (841.89, 595.28), "A4 (210 × 297 mm), landscape");
  ASSERT_STR (dp_format_page_size (612, 792), "Letter (8.5 × 11 in)");
  ASSERT_STR (dp_format_page_size (612, 1008), "Legal (8.5 × 14 in)");
  ASSERT_STR (dp_format_page_size (400, 400), "141 × 141 mm");
  g_assert_null (dp_format_page_size (0, 100));
  g_assert_null (dp_format_page_size (-1, 100));
}

static void
test_counts (void)
{
  ASSERT_STR (dp_format_item_count (0, 0, FALSE), "Empty folder");
  ASSERT_STR (dp_format_item_count (1, 0, FALSE), "1 item");
  ASSERT_STR (dp_format_item_count (12, 0, FALSE), "12 items");
  ASSERT_STR (dp_format_item_count (12, 3, FALSE), "12 items, 3 hidden");
  ASSERT_STR (dp_format_item_count (100000, 0, TRUE), "more than 100000 items");
  ASSERT_STR (dp_format_kind_counts (0, 0, 0, 0, 0, 0), "");
  ASSERT_STR (dp_format_kind_counts (1, 3, 0, 0, 0, 2), "1 folder, 3 images, 2 other files");
  ASSERT_STR (dp_format_kind_counts (2, 1, 1, 1, 1, 1),
              "2 folders, 1 image, 1 video, 1 audio file, 1 document, 1 other file");
}

static void
test_permission_chips (void)
{
  /* -rw-r--r-- */
  assert_strv (dp_permission_chips (0100644, DP_WHO_OWNER, FALSE), (const char *[]) { "Read", "Write", NULL });
  assert_strv (dp_permission_chips (0100644, DP_WHO_GROUP, FALSE), (const char *[]) { "Read", NULL });
  assert_strv (dp_permission_chips (0100644, DP_WHO_OTHERS, FALSE), (const char *[]) { "Read", NULL });
  /* -rwx------ */
  assert_strv (dp_permission_chips (0700, DP_WHO_OWNER, FALSE), (const char *[]) { "Read", "Write", "Run", NULL });
  assert_strv (dp_permission_chips (0700, DP_WHO_OTHERS, FALSE), (const char *[]) { "No access", NULL });
  /* drwxr-x--x */
  assert_strv (dp_permission_chips (040751, DP_WHO_OWNER, TRUE),
               (const char *[]) { "List files", "Create & delete", "Open", NULL });
  assert_strv (dp_permission_chips (040751, DP_WHO_GROUP, TRUE), (const char *[]) { "List files", "Open", NULL });
  assert_strv (dp_permission_chips (040751, DP_WHO_OTHERS, TRUE), (const char *[]) { "Open", NULL });
  assert_strv (dp_access_chips (TRUE, FALSE, TRUE, FALSE), (const char *[]) { "Read", "Run", NULL });
  assert_strv (dp_access_chips (FALSE, FALSE, FALSE, TRUE), (const char *[]) { "No access", NULL });
  assert_strv (dp_special_chips (0755), (const char *[]) { NULL });
  assert_strv (dp_special_chips (04755), (const char *[]) { "Set user ID", NULL });
  assert_strv (dp_special_chips (07777), (const char *[]) { "Set user ID", "Set group ID", "Sticky", NULL });
  /* Every combination of bits gives at least one chip. */
  for (guint32 mode = 0; mode < 01000; mode++)
    for (int who = 0; who < 3; who++)
      for (int dir = 0; dir < 2; dir++)
        {
          GStrv v = dp_permission_chips (mode, (DpWho) who, dir);
          g_assert_cmpuint (g_strv_length (v), >=, 1);
          g_assert_cmpuint (g_strv_length (v), <=, 3);
          g_strfreev (v);
        }
}

static void
test_accessible_labels (void)
{
  ASSERT_STR (dp_chip_accessible_label ("Owner", "Read"), "Owner can read");
  ASSERT_STR (dp_chip_accessible_label ("Group", "Create & delete"), "Group can create and delete files");
  ASSERT_STR (dp_chip_accessible_label ("Others", "No access"), "Others have no access");
  ASSERT_STR (dp_chip_accessible_label ("Owner", "No access"), "Owner has no access");
  ASSERT_STR (dp_chip_accessible_label ("You", "Run"), "You can run");
  ASSERT_STR (dp_chip_accessible_label ("Special", "Sticky"), "Special sticky");
  ASSERT_STR (dp_chip_accessible_label ("X", "Unknown"), "X: Unknown");
}

static void
test_dates (void)
{
  g_autoptr (GTimeZone) utc = g_time_zone_new_utc ();
  g_autoptr (GDateTime) now = g_date_time_new (utc, 2026, 11, 14, 17, 30, 0);
  g_autoptr (GDateTime) today = g_date_time_new (utc, 2026, 11, 14, 9, 5, 0);
  g_autoptr (GDateTime) yesterday = g_date_time_new (utc, 2026, 11, 13, 23, 59, 0);
  g_autoptr (GDateTime) older = g_date_time_new (utc, 2022, 8, 4, 13, 9, 0);
  g_autoptr (GDateTime) new_year = g_date_time_new (utc, 2027, 1, 1, 0, 1, 0);
  g_autoptr (GDateTime) last_year = g_date_time_new (utc, 2026, 12, 31, 22, 0, 0);
  ASSERT_STR (dp_format_date (today, now), "Today, 09:05");
  ASSERT_STR (dp_format_date (yesterday, now), "Yesterday, 23:59");
  ASSERT_STR (dp_format_date (older, now), "4 August 2022, 13:09");
  ASSERT_STR (dp_format_date (last_year, new_year), "Yesterday, 22:00");
}

int
main (int argc, char **argv)
{
  setlocale (LC_ALL, "");
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/dp-format/sizes", test_sizes);
  g_test_add_func ("/dp-format/media-values", test_media_values);
  g_test_add_func ("/dp-format/page-sizes", test_page_sizes);
  g_test_add_func ("/dp-format/counts", test_counts);
  g_test_add_func ("/dp-format/permission-chips", test_permission_chips);
  g_test_add_func ("/dp-format/accessible-labels", test_accessible_labels);
  g_test_add_func ("/dp-format/dates", test_dates);
  return g_test_run ();
}
