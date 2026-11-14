/* Details pane preview box. See dp_box.h. */
#include "dp_box.h"

#include <math.h>

#define MEDIA_MAX_RATIO 3.0 /* bottom strip: a very wide picture gets at most 3 × the strip height */

struct _DpBox
{
  GtkWidget       parent_instance;
  GtkWidget      *child;       /* Owned (parented to us), or NULL */
  double          ratio;       /* 0 = icon */
  int             icon_height;
  int             max_height;
  GtkOrientation  layout;
  int             cx, cy, cw, ch; /* last child rectangle */
};

G_DEFINE_FINAL_TYPE (DpBox, dp_box, GTK_TYPE_WIDGET)

static gboolean
bottom (DpBox *self)
{
  return self->layout == GTK_ORIENTATION_HORIZONTAL;
}

static GtkSizeRequestMode
dp_box_get_request_mode (GtkWidget *widget)
{
  return bottom (DP_BOX (widget)) ? GTK_SIZE_REQUEST_WIDTH_FOR_HEIGHT : GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

static void
dp_box_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat, int *min_baseline,
                int *nat_baseline)
{
  DpBox *self = DP_BOX (widget);
  *min_baseline = *nat_baseline = -1;
  if (!bottom (self))
    {
      if (orientation == GTK_ORIENTATION_HORIZONTAL)
        {
          *min = 1; /* the pane gives us its width */
          *nat = 1;
          return;
        }
      int h = self->icon_height;
      if (self->ratio > 0)
        h = for_size > 0 ? (int) MIN (ceil (for_size / self->ratio), self->max_height) : self->max_height;
      *min = *nat = MAX (h, 1);
      return;
    }
  /* Bottom: the strip gives us its height. */
  if (orientation == GTK_ORIENTATION_VERTICAL)
    {
      *min = 1;
      *nat = MAX (self->max_height, 1);
      return;
    }
  int h = for_size > 0 ? for_size : self->max_height;
  int w = self->ratio > 0 ? (int) ceil (h * MIN (self->ratio, MEDIA_MAX_RATIO)) : h;
  *min = *nat = MAX (w, 1);
}

static void
dp_box_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  (void) baseline;
  DpBox *self = DP_BOX (widget);
  int x = 0, y = 0, w = width, h = height;
  if (self->ratio > 0 && width > 0 && height > 0)
    {
      /* The largest rectangle of the media's shape that fits, centred. */
      double fw = MIN ((double) width, height * self->ratio);
      double fh = fw / self->ratio;
      w = (int) lround (fw);
      h = (int) lround (fh);
      x = (width - w) / 2;
      y = (height - h) / 2;
    }
  self->cx = x, self->cy = y, self->cw = w, self->ch = h;
  if (self->child != NULL)
    {
      GskTransform *t = gsk_transform_translate (NULL, &GRAPHENE_POINT_INIT (x, y)); /* consumed */
      gtk_widget_allocate (self->child, w, h, -1, t);
    }
}

static void
dp_box_dispose (GObject *object)
{
  DpBox *self = DP_BOX (object);
  g_clear_pointer (&self->child, gtk_widget_unparent);
  G_OBJECT_CLASS (dp_box_parent_class)->dispose (object);
}

static void
dp_box_class_init (DpBoxClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = dp_box_dispose;
  GtkWidgetClass *wc = GTK_WIDGET_CLASS (klass);
  wc->get_request_mode = dp_box_get_request_mode;
  wc->measure = dp_box_measure;
  wc->size_allocate = dp_box_size_allocate;
  gtk_widget_class_set_css_name (wc, "dp-box");
}

static void
dp_box_init (DpBox *self)
{
  self->icon_height = 176;
  self->max_height = 260;
  self->layout = GTK_ORIENTATION_VERTICAL;
}

GtkWidget *
dp_box_new (void)
{
  return g_object_new (DP_TYPE_BOX, NULL);
}

void
dp_box_set_child (DpBox *self, GtkWidget *child)
{
  g_return_if_fail (DP_IS_BOX (self));
  g_clear_pointer (&self->child, gtk_widget_unparent);
  if (child != NULL)
    {
      self->child = child;
      gtk_widget_set_parent (child, GTK_WIDGET (self));
    }
}

static void
changed (DpBox *self)
{
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

void
dp_box_set_ratio (DpBox *self, double ratio)
{
  g_return_if_fail (DP_IS_BOX (self));
  ratio = ratio > 0 && isfinite (ratio) ? ratio : 0;
  if (fabs (ratio - self->ratio) < 1e-6)
    return;
  self->ratio = ratio;
  changed (self);
}

void
dp_box_set_icon_height (DpBox *self, int height)
{
  g_return_if_fail (DP_IS_BOX (self));
  if (height == self->icon_height)
    return;
  self->icon_height = MAX (height, 1);
  changed (self);
}

void
dp_box_set_max_height (DpBox *self, int height)
{
  g_return_if_fail (DP_IS_BOX (self));
  if (height == self->max_height)
    return;
  self->max_height = MAX (height, 1);
  changed (self);
}

void
dp_box_set_layout (DpBox *self, GtkOrientation layout)
{
  g_return_if_fail (DP_IS_BOX (self));
  if (layout == self->layout)
    return;
  self->layout = layout;
  changed (self);
}

void
dp_box_get_child_rect (DpBox *self, int *x, int *y, int *width, int *height)
{
  g_return_if_fail (DP_IS_BOX (self));
  *x = self->cx, *y = self->cy, *width = self->cw, *height = self->ch;
}

gboolean
dp_box_get_ratio_is_media (DpBox *self)
{
  g_return_val_if_fail (DP_IS_BOX (self), FALSE);
  return self->ratio > 0;
}
