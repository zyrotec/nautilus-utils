/* Preview frames, Dolphin style (PLAN.md §6.1.2, D17).
 *
 * A frame is a transparent W×H texture covering the folder icon's front face plus a FRAME_PAD margin.
 * The face area is split according to how many thumbnails the frame has (owner's layout):
 *   1 → the whole area;  2 → left half, right half;  3 → top-left, top-right, bottom centre;
 *   4 → the four quarters (left-to-right, top-to-bottom).
 * Each thumbnail is centred in its section and:
 *   - keeps its own aspect ratio (fitted inside the section, never cropped),
 *   - is drawn as a "photo": thin white border + soft shadow,
 *   - is tilted by a pseudo-random angle derived from a per-file seed, so the look is random but stable
 *     across redraws, zoom levels and sessions.
 * Later items are drawn on top of earlier ones (tilted corners may overlap a little).
 *
 * Decoding uses GTK's thread-safe loaders (gdk_texture_new_from_filename, D9); drawing uses cairo image
 * surfaces (rotation + good downscale filtering), whose pixel format equals GDK_MEMORY_DEFAULT.
 * Everything here is safe to run in a worker thread. No Nautilus types (PLAN.md §4.2).
 */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct
{
  const char *path; /* Borrowed: thumbnail file, or NULL for an empty slot. */
  guint       seed; /* Stable per file (e.g. hash of its URI): picks its tilt and nudge. */
} FpCompositeItem;

/* Frame height for a frame @width pixels wide (the frame has the front face's aspect ratio). */
guint fp_composite_frame_height (guint width);

/* Tilt in degrees for @seed, within ±FRAME_MAX_TILT_DEG. Exposed for tests. */
double fp_composite_tilt (guint seed);

/* Section of item @index (0-based) when a frame @width pixels wide has @n_items (1–4) items, in frame
 * pixels. Exposed for tests. */
void fp_composite_slot_rect (guint   n_items,
                             guint   index,
                             guint   width,
                             double *x,
                             double *y,
                             double *w,
                             double *h);

/* Builds a frame @width pixels wide (height from fp_composite_frame_height) from @n_items (0–4) items.
 * Blocking (reads and decodes files): worker threads or tests only.
 * *n_unreadable (optional) receives how many non-NULL paths could not be loaded; their slots stay empty.
 * NULL entries are holes: the layout still uses @n_items sections. Returns a new texture the caller owns,
 * or NULL if @width is out of range (< 16 or > 4096). */
GdkTexture *fp_composite_frame (const FpCompositeItem *items,
                                guint                  n_items,
                                guint                  width,
                                guint                 *n_unreadable);

G_END_DECLS
