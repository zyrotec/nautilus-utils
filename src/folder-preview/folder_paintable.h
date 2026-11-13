/* FpFolderPaintable: the theme's folder icon with a preview frame on it (PLAN.md §6.1.2, D10; §11).
 *
 * What is drawn, bottom to top: the folder icon (or, while a frame is shown, the decor's custom back
 * texture), the frame at the decor's frame rectangle, and (while a frame is shown) the decor's overlay
 * texture. Without a decor it is the v1 grid: the theme icon with the frame on its front face.
 *
 * Plain GTK (no Nautilus internals), so it can be tested on its own. The view hook installs one per grid
 * icon as NautilusImage's fallback (D4) and swaps frames in place while hovering, which only
 * invalidates the paintable's contents — Nautilus sees no property change.
 */
#pragma once

#include <gtk/gtk.h>

#include "composite.h"

G_BEGIN_DECLS

/* Stacked: where the stack goes vertically (PLAN.md §11.5b). */
typedef enum
{
  FP_STACK_TOP    = 0, /* STACK_EDGE (8 px at 64 × 64) below the icon's top */
  FP_STACK_CENTRE = 1, /* inside the folder: STACK_BODY_TOP below the top of its visible body */
  FP_STACK_BOTTOM = 2, /* STACK_EDGE above the icon's bottom */
} FpStackPosition;

/* Where things go on one icon. Rectangles are fractions of the icon size (0 … 1, may exceed slightly). */
typedef struct
{
  FpLayout        layout;
  GdkTexture     *back;         /* Borrowed here (the paintable takes its own ref): drawn instead of the icon
                                 * while a frame is shown; NULL = the icon. */
  GdkTexture     *overlay;      /* Borrowed (same): drawn over the frame while a frame is shown, over the
                                 * whole icon (the same size as the back, V4); NULL = none. */
  graphene_rect_t frame_rect;   /* The frame texture, including its transparent margin. */
} FpDecor;

/* The v1 grid: frame on the front face (FRAME_FACE_*), no back or overlay. */
void fp_decor_init_grid (FpDecor *decor);

/* Stacked: the card stack, STACK_SIDE from the icon's left and right edges, at @position on a folder whose
 * opaque body is @body (fractions of the icon; used by FP_STACK_CENTRE), moved down by @offset px at the
 * 64 × 64 size (negative = up). No back or overlay yet. */
void fp_decor_init_stacked (FpDecor               *decor,
                            const graphene_rect_t *body,
                            FpStackPosition        position,
                            int                    offset);

/* Frame width in device pixels for a logical icon @size at @scale (HiDPI). */
guint fp_decor_frame_px (const FpDecor *decor,
                         int            size,
                         int            scale);

#define FP_TYPE_FOLDER_PAINTABLE (fp_folder_paintable_get_type ())
G_DECLARE_FINAL_TYPE (FpFolderPaintable, fp_folder_paintable, FP, FOLDER_PAINTABLE, GObject)

/* @icon is ref'd (may be NULL: then only the frame is drawn). @size is the logical icon size, reported
 * as the intrinsic size. Caller owns the result. */
FpFolderPaintable *fp_folder_paintable_new (GdkPaintable *icon,
                                            int           size);

/* Replaces the folder icon (ref'd) and/or size; invalidates as needed. */
void fp_folder_paintable_set_icon (FpFolderPaintable *self,
                                   GdkPaintable      *icon);
void fp_folder_paintable_set_size (FpFolderPaintable *self,
                                   int                size);

/* Shows @frame (ref'd; NULL = icon only) and remembers which folder it belongs to (@uri copied; used to
 * verify that recycled cells never show another folder's preview). Invalidates the contents. */
void fp_folder_paintable_set_frame (FpFolderPaintable *self,
                                    GdkTexture        *frame,
                                    const char        *uri);

/* Copies @decor (NULL = fp_decor_init_grid), taking refs on its textures; invalidates the contents. */
void fp_folder_paintable_set_decor (FpFolderPaintable *self,
                                    const FpDecor     *decor);

/* Borrowed getters. */
GdkPaintable *fp_folder_paintable_get_icon      (FpFolderPaintable *self);
GdkTexture   *fp_folder_paintable_get_frame     (FpFolderPaintable *self);
const char   *fp_folder_paintable_get_frame_uri (FpFolderPaintable *self);

/* Grid: where the frame goes inside a @width × @height icon (logical units). */
void fp_folder_paintable_frame_rect (double           width,
                                     double           height,
                                     graphene_rect_t *out);

/* Grid: frame width in device pixels for a logical icon @size at @scale (HiDPI); the height follows from the
 * face's aspect ratio (fp_composite_frame_height). */
guint fp_folder_paintable_frame_px (int size,
                                    int scale);

G_END_DECLS
