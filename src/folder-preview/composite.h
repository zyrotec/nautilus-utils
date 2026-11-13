/* Preview frames (PLAN.md §6.1.2, D17; stacked layout and style options: §11).
 *
 * A frame is a transparent W×H texture with the folder's photos, drawn according to an FpStyle:
 *
 * Grid layout (v1): covers the folder icon's front face plus a FRAME_PAD margin. The face area is split
 * according to how many thumbnails the frame has (owner's layout):
 *   1 → the whole area;  2 → left half, right half;  3 → top-left, top-right, bottom centre;
 *   4 → the four quarters (left-to-right, top-to-bottom).
 * Each thumbnail is centred in its section, keeps its own aspect ratio (fitted, never cropped) and is drawn
 * as a "photo": optional border (style colour) + optional soft shadow.
 *
 * Stacked layout: up to STACK_CARDS rounded cards stacked like the owner's mock-up (config.h). Item 0 is the
 * front card; each item behind it is inset, raised and dimmed a little. Each picture fills its card
 * (cropped); a NULL or unreadable item is drawn as a blank card so the stack keeps its shape.
 *
 * With style->rotation, every photo/card is tilted by a pseudo-random angle derived from a per-file seed, so
 * the look is random but stable across redraws, zoom levels and sessions. Later items are drawn on top
 * (stacked: front last).
 *
 * Decoding uses GTK's thread-safe loaders (gdk_texture_new_from_filename, D9); drawing uses cairo image
 * surfaces (rotation + good downscale filtering), whose pixel format equals GDK_MEMORY_DEFAULT.
 * Everything here is safe to run in a worker thread. No Nautilus types (PLAN.md §4.2).
 */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum
{
  FP_LAYOUT_GRID    = 0,
  FP_LAYOUT_STACKED = 1,
} FpLayout;

/* Stacked: how cards further back look (the front card is always fully visible). */
typedef enum
{
  FP_DEPTH_NONE         = 0, /* only slightly darker (STACK_DIM per card) */
  FP_DEPTH_BLUR         = 1, /* blurred and darkened */
  FP_DEPTH_TRANSPARENCY = 2, /* more transparent */
  FP_DEPTH_DARKEN       = 3, /* darker, no blur */
} FpDepth;

/* Everything that changes how frames look. Plain values (copied freely, also into worker threads). */
typedef struct
{
  FpLayout layout;
  guint    stack_step;  /* Stacked: photos per hover step (1 or 3); ignored by the grid. */
  gboolean border;
  guint8   border_r, border_g, border_b;
  guint    border_width;     /* Tenths of a pixel at the 64 × 64 icon size (scaled; at least 1 device px). */
  gboolean shadow;
  gboolean rotation;
  gboolean keep_aspect; /* Grid: fit (TRUE) or fill the section, cropped; stacked: card in the picture's
                         * shape (TRUE) or picture fills the card, cropped. */
  FpDepth  depth;            /* Stacked only. */
  guint    corner_radius;    /* Each picture's corner radius, px at the 64 × 64 icon size (scaled). */
  guint    fade;             /* Stacked: bottom fade over this % of the stack height (0 = off, max 100). */
  guint    view_top_radius;  /* Stacked: the stack area clips all pictures with these corner radii (px at */
  guint    view_bottom_radius; /* 64 × 64); both 0 = no clipping. */
  guint    stack_padding;    /* Stacked: space to the icon's left and right edges, px at 64 × 64 (max 20). */
} FpStyle;

/* The default look: grid, white border, shadow, rotation, aspect ratio kept, 2 px corners; stacked: blur
 * depth, no fade, no view clipping, step 1. */
void fp_style_init_default (FpStyle *style);

gboolean fp_style_equal (const FpStyle *a,
                         const FpStyle *b);

/* Short text identifying the look (used in disk cache names). Caller owns the result (g_free). */
char *fp_style_key (const FpStyle *style);

/* Items one frame shows at most: 4 (grid) or STACK_CARDS (stacked). */
guint fp_style_slots (const FpStyle *style);

/* How many photos further each hover frame starts: 4 (grid) or style->stack_step (stacked, 1 or 3). */
guint fp_style_step (const FpStyle *style);

typedef struct
{
  const char *path; /* Borrowed: thumbnail file, or NULL for an empty slot. */
  guint       seed; /* Stable per file (e.g. hash of its URI): picks its tilt. */
} FpCompositeItem;

/* Frame height for a frame @width pixels wide in @style's layout (each layout has a fixed aspect ratio). */
guint fp_composite_frame_height (const FpStyle *style,
                                 guint          width);

/* Grid tilt in degrees for @seed, within ±FRAME_MAX_TILT_DEG. Exposed for tests. */
double fp_composite_tilt (guint seed);

/* Grid: section of item @index (0-based) when a frame @width pixels wide has @n_items (1–4) items, in frame
 * pixels. Exposed for tests. */
void fp_composite_slot_rect (guint   n_items,
                             guint   index,
                             guint   width,
                             double *x,
                             double *y,
                             double *w,
                             double *h);

/* Stacked: the stack's width as a fraction of the icon (1 − 2 × padding / 64, at least 0.25). */
double fp_style_stack_width (const FpStyle *style);

/* Icon size in pixels that a frame @width pixels wide belongs to (the frame covers a fixed part of the icon
 * in each layout); used to scale the "px at 64 × 64" options. */
double fp_composite_icon_px (const FpStyle *style,
                             guint          width);

/* Stacked: the stack area (cards before tilting, without the frame's margin), in frame pixels. */
void fp_composite_stack_rect (guint   width,
                              double *x,
                              double *y,
                              double *w,
                              double *h);

/* Stacked: rectangle (before tilting) of card @index (0 = front) in a frame @width pixels wide, in frame
 * pixels. Exposed for tests. */
void fp_composite_card_rect (guint   index,
                             guint   width,
                             double *x,
                             double *y,
                             double *w,
                             double *h);

/* Builds a frame @width pixels wide (height from fp_composite_frame_height) from @n_items items
 * (0 .. fp_style_slots). Blocking (reads and decodes files): worker threads or tests only.
 * *n_unreadable (optional) receives how many non-NULL paths could not be loaded.
 * Grid: NULL entries are holes, the layout still uses @n_items sections. Stacked: NULL/unreadable entries
 * are blank cards. Returns a new texture the caller owns, or NULL if @width is out of range (< 16 or
 * > 4096). */
GdkTexture *fp_composite_frame (const FpStyle         *style,
                                const FpCompositeItem *items,
                                guint                  n_items,
                                guint                  width,
                                guint                 *n_unreadable);

G_END_DECLS
