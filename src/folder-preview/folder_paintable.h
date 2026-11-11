/* FpFolderPaintable: the theme's folder icon with a 2×2 preview frame on its front face (PLAN.md §6.1.2, D10).
 *
 * Plain GTK (no Nautilus internals), so it can be tested on its own. The view hook installs one per grid
 * icon as NautilusImage's fallback (D4) and swaps frames in place while hovering, which only
 * invalidates the paintable's contents — Nautilus sees no property change.
 */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

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

/* Borrowed getters. */
GdkPaintable *fp_folder_paintable_get_icon      (FpFolderPaintable *self);
GdkTexture   *fp_folder_paintable_get_frame     (FpFolderPaintable *self);
const char   *fp_folder_paintable_get_frame_uri (FpFolderPaintable *self);

/* Where the frame goes inside a @width × @height icon (logical units). */
void fp_folder_paintable_frame_rect (double           width,
                                     double           height,
                                     graphene_rect_t *out);

/* Frame width in device pixels for a logical icon @size at @scale (HiDPI); the height follows from the
 * face's aspect ratio (fp_composite_frame_height). */
guint fp_folder_paintable_frame_px (int size,
                                    int scale);

G_END_DECLS
