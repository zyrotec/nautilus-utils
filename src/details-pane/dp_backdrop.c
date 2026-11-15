/* Details pane: blurred picture behind the preview. See dp_backdrop.h. */
#include "dp_backdrop.h"

#define BLUR_FRACTION 0.08 /* blur radius: this much of the box's larger side */
#define DARKEN        0.30 /* black laid over it */

struct _DpBackdrop
{
  GtkWidget     parent_instance;
  GdkPaintable *paintable; /* Owned or NULL */
};

G_DEFINE_FINAL_TYPE (DpBackdrop, dp_backdrop, GTK_TYPE_WIDGET)

static void
dp_backdrop_snapshot (GtkWidget *widget, GtkSnapshot *snap)
{
  DpBackdrop *self = DP_BACKDROP (widget);
  double w = gtk_widget_get_width (widget), h = gtk_widget_get_height (widget);
  if (self->paintable == NULL || w < 1 || h < 1)
    return;
  double ratio = gdk_paintable_get_intrinsic_aspect_ratio (self->paintable);
  if (ratio <= 0)
    ratio = w / h;
  double blur = MAX (w, h) * BLUR_FRACTION;
  /* Fill the box (cropped), plus the blur radius on every side so the edges stay opaque. */
  double cw = w + 2 * blur, ch = h + 2 * blur;
  double pw = MAX (cw, ch * ratio), ph = pw / ratio;
  graphene_rect_t bounds = GRAPHENE_RECT_INIT (0, 0, (float) w, (float) h);
  gtk_snapshot_push_clip (snap, &bounds);
  gtk_snapshot_push_blur (snap, blur);
  gtk_snapshot_save (snap);
  gtk_snapshot_translate (snap, &GRAPHENE_POINT_INIT ((float) ((w - pw) / 2), (float) ((h - ph) / 2)));
  gdk_paintable_snapshot (self->paintable, snap, pw, ph);
  gtk_snapshot_restore (snap);
  gtk_snapshot_pop (snap);
  static const GdkRGBA shade = { 0, 0, 0, (float) DARKEN };
  gtk_snapshot_append_color (snap, &shade, &bounds);
  gtk_snapshot_pop (snap);
}

static void
dp_backdrop_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                     int *min_baseline, int *nat_baseline)
{
  (void) widget, (void) orientation, (void) for_size;
  *min = *nat = 0; /* the box sizes us */
  *min_baseline = *nat_baseline = -1;
}

static void
on_invalidate (GdkPaintable *paintable, gpointer data)
{
  (void) paintable;
  gtk_widget_queue_draw (GTK_WIDGET (data));
}

static void
dp_backdrop_dispose (GObject *object)
{
  dp_backdrop_set_paintable (DP_BACKDROP (object), NULL);
  G_OBJECT_CLASS (dp_backdrop_parent_class)->dispose (object);
}

static void
dp_backdrop_class_init (DpBackdropClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = dp_backdrop_dispose;
  GtkWidgetClass *wc = GTK_WIDGET_CLASS (klass);
  wc->snapshot = dp_backdrop_snapshot;
  wc->measure = dp_backdrop_measure;
  gtk_widget_class_set_css_name (wc, "dp-backdrop");
}

static void
dp_backdrop_init (DpBackdrop *self)
{
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
}

GtkWidget *
dp_backdrop_new (void)
{
  return g_object_new (DP_TYPE_BACKDROP, NULL);
}

void
dp_backdrop_set_paintable (DpBackdrop *self, GdkPaintable *paintable)
{
  g_return_if_fail (DP_IS_BACKDROP (self));
  if (self->paintable == paintable)
    return;
  if (self->paintable != NULL)
    g_signal_handlers_disconnect_by_func (self->paintable, on_invalidate, self);
  g_set_object (&self->paintable, paintable);
  if (paintable != NULL && !(gdk_paintable_get_flags (paintable) & GDK_PAINTABLE_STATIC_CONTENTS))
    g_signal_connect_object (paintable, "invalidate-contents", G_CALLBACK (on_invalidate), self, 0);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

GdkPaintable *
dp_backdrop_get_paintable (DpBackdrop *self)
{
  g_return_val_if_fail (DP_IS_BACKDROP (self), NULL);
  return self->paintable;
}
