/* Custom folder SVGs for the stacked layout (PLAN.md §11.1, V3–V5), plus paintable measuring helpers.
 *
 * An FpIconArt is one user-chosen SVG, parsed once with GTK's own SVG renderer (GtkSvg, GTK ≥ 4.22;
 * animations and external resources off) and rasterised on demand into textures at exact device sizes
 * (cached, so each zoom level is rendered once). Rendering draws render nodes with cairo
 * (gsk_render_node_draw): no display or GPU renderer is needed.
 *
 * Plain GTK, no Nautilus types (PLAN.md §4.2). Main thread only (GtkSvg is not thread-safe).
 */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _FpIconArt FpIconArt;

/* Loads the SVG at @path (at most 4 MiB). Errors: G_IO_ERROR_* for unreadable/too large files,
 * G_IO_ERROR_INVALID_DATA if it has no usable size (not an SVG, or no width/height/viewBox),
 * G_IO_ERROR_NOT_SUPPORTED with GTK < 4.22. Caller owns the result (fp_icon_art_free). */
FpIconArt *fp_icon_art_new_from_file (const char  *path,
                                      GError     **error);

void fp_icon_art_free (FpIconArt *art);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FpIconArt, fp_icon_art_free)

/* The SVG's own size (> 0), in SVG units. */
double fp_icon_art_get_width  (FpIconArt *art);
double fp_icon_art_get_height (FpIconArt *art);

/* The SVG drawn as large as fits, centred, aspect ratio kept, on a transparent @width × @height canvas
 * (1 … 4096 px). Cached per size. Returns a new reference, or NULL if the size is out of range. */
GdkTexture *fp_icon_art_render (FpIconArt *art,
                                int        width,
                                int        height);

/* Opaque bounding box of @art drawn as in fp_icon_art_render on a square canvas, as fractions of the canvas
 * (0 … 1): the pixels with alpha ≥ 50 %, so soft shadows are left out. Computed once. Returns FALSE if the
 * SVG is fully transparent. */
gboolean fp_icon_art_get_body (FpIconArt       *art,
                               graphene_rect_t *out);

/* Draws @paintable at @width × @height px into a new texture (caller owns), or NULL if out of range. */
GdkTexture *fp_render_paintable (GdkPaintable *paintable,
                                 int           width,
                                 int           height);

/* Opaque bounding box (alpha ≥ 50 %) of @texture as fractions of its size. FALSE if fully transparent. */
gboolean fp_texture_opaque_bounds (GdkTexture      *texture,
                                   graphene_rect_t *out);

G_END_DECLS
