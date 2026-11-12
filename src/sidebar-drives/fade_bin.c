/* SdFadeBin (PLAN.md §9.1.4). See fade_bin.h. */
#include "fade_bin.h"

struct _SdFadeBin
{
  GtkWidget  parent_instance;
  GtkWidget *child;       /* Owned via gtk_widget_set_parent; unparented in dispose / set_child. */
  double     fade_width;  /* px; 0 = no mask */
  double     fade_extra;  /* px added to the followed revealer's width when fully revealed */
  GWeakRef   revealer;    /* The revealer being followed, or empty */
  gulong     reveal_id;   /* notify::reveal-child on the revealer */
  guint      tick_id;     /* Frame tick while the revealer animates, or 0 */
};

G_DEFINE_FINAL_TYPE (SdFadeBin, sd_fade_bin, GTK_TYPE_WIDGET)

double
sd_fade_follow_width (double revealer_width,
                      double full_width,
                      double extra)
{
  if (revealer_width <= 0.0 || full_width <= 0.0)
    return 0.0;
  double progress = CLAMP (revealer_width / full_width, 0.0, 1.0);
  return revealer_width + MAX (extra, 0.0) * progress;
}

/* The fade in effect now: the followed revealer's current (animated) width plus the extra fade (grown in
 * with the slide), read after layout so the mask matches the button on every frame; otherwise the fixed
 * fade width. */
static double
current_fade (SdFadeBin *self)
{
  g_autoptr (GtkRevealer) revealer = g_weak_ref_get (&self->revealer);
  if (revealer == NULL)
    return self->fade_width;
  GtkWidget *child = gtk_revealer_get_child (revealer);
  if (!gtk_widget_get_mapped (GTK_WIDGET (revealer)) || child == NULL || !gtk_widget_get_visible (child))
    return 0.0;
  int full = 0;
  gtk_widget_measure (child, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &full, NULL, NULL); /* incl. margins */
  return sd_fade_follow_width ((double) gtk_widget_get_width (GTK_WIDGET (revealer)), (double) full,
                               self->fade_extra);
}

const GskColorStop sd_fade_mask_stops[SD_FADE_MASK_N_STOPS] = {
  { 0.0f, { 0.0f, 0.0f, 0.0f, 1.0f } },
  { 0.5f, { 0.0f, 0.0f, 0.0f, 0.0f } },
  { 1.0f, { 0.0f, 0.0f, 0.0f, 0.0f } },
};

gboolean
sd_fade_mask_line (float             width,
                   float             fade,
                   gboolean          rtl,
                   graphene_point_t *opaque,
                   graphene_point_t *clear)
{
  fade = MIN (fade, width);
  if (fade <= 0.5f || width <= 0.0f)
    return FALSE;
  /* The button sits at the end of the row: right in left-to-right layouts, left in right-to-left ones. */
  graphene_point_init (opaque, rtl ? fade : width - fade, 0.0f);
  graphene_point_init (clear, rtl ? 0.0f : width, 0.0f);
  return TRUE;
}

static void
sd_fade_bin_snapshot (GtkWidget   *widget,
                      GtkSnapshot *snapshot)
{
  SdFadeBin *self = SD_FADE_BIN (widget);
  float w = (float) gtk_widget_get_width (widget);
  float h = (float) gtk_widget_get_height (widget);
  graphene_point_t opaque, clear;
  if (!sd_fade_mask_line (w, (float) current_fade (self), gtk_widget_get_direction (widget) == GTK_TEXT_DIR_RTL,
                          &opaque, &clear))
    {
      GTK_WIDGET_CLASS (sd_fade_bin_parent_class)->snapshot (widget, snapshot);
      return;
    }
  gtk_snapshot_push_mask (snapshot, GSK_MASK_MODE_ALPHA);
  gtk_snapshot_append_linear_gradient (snapshot, &GRAPHENE_RECT_INIT (0.0f, 0.0f, w, h), &opaque, &clear,
                                       sd_fade_mask_stops, SD_FADE_MASK_N_STOPS);
  gtk_snapshot_pop (snapshot); /* end of mask */
  GTK_WIDGET_CLASS (sd_fade_bin_parent_class)->snapshot (widget, snapshot);
  gtk_snapshot_pop (snapshot); /* end of masked content */
}

static gboolean
on_tick (GtkWidget     *widget,
         GdkFrameClock *clock,
         gpointer       data)
{
  (void) clock;
  (void) data;
  SdFadeBin *self = SD_FADE_BIN (widget);
  g_autoptr (GtkRevealer) revealer = g_weak_ref_get (&self->revealer);
  gtk_widget_queue_draw (widget); /* the mask follows the revealer's width (current_fade) */
  if (revealer == NULL ||
      gtk_revealer_get_child_revealed (revealer) == gtk_revealer_get_reveal_child (revealer))
    {
      self->tick_id = 0;
      return G_SOURCE_REMOVE;
    }
  return G_SOURCE_CONTINUE;
}

/* The revealer started showing or hiding its child: follow its width every frame until it settles. */
static void
on_reveal_changed (GObject    *revealer,
                   GParamSpec *pspec,
                   gpointer    data)
{
  (void) revealer;
  (void) pspec;
  SdFadeBin *self = data;
  if (self->tick_id == 0)
    self->tick_id = gtk_widget_add_tick_callback (GTK_WIDGET (self), on_tick, NULL, NULL);
}

void
sd_fade_bin_follow (SdFadeBin   *self,
                    GtkRevealer *revealer)
{
  g_return_if_fail (SD_IS_FADE_BIN (self));
  g_autoptr (GtkRevealer) old = g_weak_ref_get (&self->revealer);
  if (old != NULL)
    g_clear_signal_handler (&self->reveal_id, old);
  self->reveal_id = 0;
  if (self->tick_id != 0)
    gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->tick_id);
  self->tick_id = 0;
  g_weak_ref_set (&self->revealer, revealer);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  if (revealer == NULL)
    return;
  /* connect_object: the handler goes away with @self, so the revealer never calls into a freed bin. */
  self->reveal_id = g_signal_connect_object (revealer, "notify::reveal-child", G_CALLBACK (on_reveal_changed),
                                             self, 0);
  on_reveal_changed (G_OBJECT (revealer), NULL, self);
}

void
sd_fade_bin_set_fade_width (SdFadeBin *self,
                            double     width)
{
  g_return_if_fail (SD_IS_FADE_BIN (self));
  width = MAX (width, 0.0);
  if (width == self->fade_width)
    return;
  self->fade_width = width;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
sd_fade_bin_set_fade_extra (SdFadeBin *self,
                            double     extra)
{
  g_return_if_fail (SD_IS_FADE_BIN (self));
  self->fade_extra = MAX (extra, 0.0);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

double
sd_fade_bin_get_fade_width (SdFadeBin *self)
{
  g_return_val_if_fail (SD_IS_FADE_BIN (self), 0.0);
  return self->fade_width;
}

void
sd_fade_bin_set_child (SdFadeBin *self,
                       GtkWidget *child)
{
  g_return_if_fail (SD_IS_FADE_BIN (self));
  if (self->child == child)
    return;
  g_clear_pointer (&self->child, gtk_widget_unparent);
  if (child != NULL)
    {
      self->child = child;
      gtk_widget_set_parent (child, GTK_WIDGET (self));
    }
}

GtkWidget *
sd_fade_bin_get_child (SdFadeBin *self)
{
  g_return_val_if_fail (SD_IS_FADE_BIN (self), NULL);
  return self->child;
}

static void
sd_fade_bin_dispose (GObject *object)
{
  SdFadeBin *self = SD_FADE_BIN (object);
  g_autoptr (GtkRevealer) revealer = g_weak_ref_get (&self->revealer);
  if (revealer != NULL)
    g_clear_signal_handler (&self->reveal_id, revealer);
  self->reveal_id = 0;
  if (self->tick_id != 0)
    gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->tick_id);
  self->tick_id = 0;
  g_clear_pointer (&self->child, gtk_widget_unparent);
  G_OBJECT_CLASS (sd_fade_bin_parent_class)->dispose (object);
}

static void
sd_fade_bin_finalize (GObject *object)
{
  g_weak_ref_clear (&SD_FADE_BIN (object)->revealer);
  G_OBJECT_CLASS (sd_fade_bin_parent_class)->finalize (object);
}

static void
sd_fade_bin_class_init (SdFadeBinClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  object_class->dispose = sd_fade_bin_dispose;
  object_class->finalize = sd_fade_bin_finalize;
  widget_class->snapshot = sd_fade_bin_snapshot;
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "sd-fade-bin");
}

static void
sd_fade_bin_init (SdFadeBin *self)
{
  g_weak_ref_init (&self->revealer, NULL);
}

/* Returns a new floating widget. */
GtkWidget *
sd_fade_bin_new (void)
{
  return g_object_new (SD_TYPE_FADE_BIN, NULL);
}
