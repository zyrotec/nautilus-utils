/* Details pane: turning raw values into the text the pane shows (PLAN.md §13.1.2, §13.1.3).
 *
 * Pure functions, no I/O, no Nautilus types (PLAN.md §4.2): every function returns a new string (g_free) or a
 * new NULL-terminated string array (g_strfreev), never NULL unless documented. Safe in any thread. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* "1.2 MB" (decimal units, like Files). */
char *dp_format_size (guint64 bytes);

/* "1,234,567 bytes" (thousands separators from the locale). */
char *dp_format_size_exact (guint64 bytes);

/* @when as "Today, 17:30", "Yesterday, 09:05" or "14 November 2026, 17:30", relative to @now (both local
 * time). */
char *dp_format_date (GDateTime *when,
                      GDateTime *now);

/* "0:05", "12:34", "1:02:03" from nanoseconds (rounded down to whole seconds; under 1 s → "0:00"). */
char *dp_format_duration (guint64 ns);

/* "80 kb/s", "7.06 Mb/s". NULL for 0 (unknown). */
char *dp_format_bitrate (guint bits_per_second);

/* "44.1 kHz", "48 kHz", "8 kHz". NULL for 0. */
char *dp_format_sample_rate (guint hz);

/* "Mono", "Stereo", "5.1 surround", "7.1 surround", "N channels". NULL for 0. */
char *dp_format_channels (guint channels);

/* "30 fps", "29.97 fps", "25 fps". NULL when unknown (num or den 0). */
char *dp_format_frame_rate (guint num,
                            guint den);

/* "1920 × 1080 px". NULL if either is 0. */
char *dp_format_dimensions (guint width,
                            guint height);

/* A PDF page size given in points: "A4 (210 × 297 mm)", "Letter (8.5 × 11 in)", or "216 × 279 mm" when it
 * isn't a known paper size; landscape pages get ", landscape". NULL if not positive. */
char *dp_format_page_size (double width_pt,
                           double height_pt);

/* Folder contents: "12 items", "1 item", "12 items, 3 hidden", "Empty folder". A @truncated count (stopped
 * at a limit) gets "more than". */
char *dp_format_item_count (guint    items,
                            guint    hidden,
                            gboolean truncated);

/* "3 images, 1 folder, 2 other files": counts in a fixed order, zero counts left out; "" if all zero. */
char *dp_format_kind_counts (guint folders,
                             guint images,
                             guint videos,
                             guint audio,
                             guint documents,
                             guint other);

/* Permissions (PLAN.md §13.1.3) ---------------------------------------------------------------------- */

typedef enum
{
  DP_WHO_OWNER = 0,
  DP_WHO_GROUP = 1,
  DP_WHO_OTHERS = 2,
} DpWho;

/* Chips for what @who may do under Unix @mode: files "Read", "Write", "Run"; folders "List files",
 * "Create & delete", "Open"; none → "No access". */
GStrv dp_permission_chips (guint32  mode,
                           DpWho    who,
                           gboolean is_dir);

/* Chips for the current user's effective access (from access::can-*), same words; none → "No access". */
GStrv dp_access_chips (gboolean can_read,
                       gboolean can_write,
                       gboolean can_execute,
                       gboolean is_dir);

/* "Set user ID", "Set group ID", "Sticky" for the bits set in @mode; empty array if none. */
GStrv dp_special_chips (guint32 mode);

/* Accessible label for one chip: "Owner can read", "Group can create and delete files", "Others have no
 * access", "You can run". */
char *dp_chip_accessible_label (const char *who_label,
                                const char *chip);

G_END_DECLS
