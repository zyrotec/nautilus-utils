/* DpBackdrop: the preview box's background made from the picture shown in it — scaled to fill the box, blurred
 * and darkened (PLAN.md §13.14 P28). Draws nothing without a paintable. Plain GTK. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DP_TYPE_BACKDROP (dp_backdrop_get_type ())
G_DECLARE_FINAL_TYPE (DpBackdrop, dp_backdrop, DP, BACKDROP, GtkWidget)

GtkWidget *dp_backdrop_new (void);

/* NULL = nothing (the box's own background shows). */
void dp_backdrop_set_paintable (DpBackdrop   *self,
                                GdkPaintable *paintable);

GdkPaintable *dp_backdrop_get_paintable (DpBackdrop *self);

G_END_DECLS
