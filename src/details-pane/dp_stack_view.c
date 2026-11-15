/* Details pane: the multi-selection stack drawn from live layers. See dp_stack_view.h.
 *
 * Geometry is computed per layer as a "Geo" (bottom-centre anchored rectangle, tilt, depth, opacity and how far
 * its thumbnail has faded in) for the current allocation. Any change computes new targets and animates every
 * layer from where it is drawn now to its target (one clock for all), so changes that arrive mid-animation
 * continue smoothly. */
#include "dp_stack_view.h"

#include <math.h>

#include "composite.h" /* fp_composite_tilt, FpDepth */

/* The folder-preview stack (folder-preview config.h): fractions of the stack width. */
#define CARD_STEP     0.0405
#define CARD_H        0.5975
#define CARD_PAD      0.06
#define CARD_STACK_H  (CARD_H + (DP_STACK_VIEW_MAX - 1) * CARD_STEP)

#define ICON_SHRINK   0.85  /* each icon behind is this much smaller */
#define ICON_MARGIN   0.06  /* room for tilt and shadow around the icon stack (fraction of the front icon) */
#define CARD_ICON_FIT 0.92  /* an icon in a card slot is at most this much of the card's height */
#define MAX_TILT_DEG  4.0

#define DEPTH_DIM_NONE   0.10
#define DEPTH_DIM_BLUR   0.16
#define DEPTH_BLUR       0.006
#define DEPTH_ALPHA      0.30
#define DEPTH_DIM_DARKEN 0.22

#define ANIM_US 220000 /* every change */

typedef struct
{
  double cx, by, w, h; /* bottom centre, size */
  double angle;        /* degrees */
  double depth;        /* 0 = front */
  double alpha;
  double mix;          /* 0 = icon, 1 = thumbnail card */
} Geo;

typedef struct
{
  char         *key;
  GdkPaintable *icon;    /* owned or NULL */
  GdkTexture   *thumb;   /* owned or NULL */
  gboolean      media;
  guint         seed;
  int           index;   /* front index, or -1 while fading out */
  Geo           from, to;
  gboolean      fresh;   /* no geometry yet: appears at its target */
  double        vis_top, vis_bottom; /* @icon's drawn part: transparent rows above / below it (fractions) */
} Layer;

struct _DpStackView
{
  GtkWidget     parent_instance;
  DpStackStyle  style;
  GPtrArray    *layers;  /* Owned Layer* */
  gint64        t0;      /* animation start (frame clock µs), 0 = still */
  guint         tick_id;
  int           alloc_w, alloc_h;
};

G_DEFINE_FINAL_TYPE (DpStackView, dp_stack_view, GTK_TYPE_WIDGET)

static void
on_icon_invalidated (GdkPaintable *paintable, gpointer data)
{
  (void) paintable;
  gtk_widget_queue_draw (GTK_WIDGET (data));
}

/* Where @icon is actually drawn in its square: the fractions of transparent rows above and below it (a folder
 * icon sits low in its square, a document fills it), so the stack can space what you see, not the squares. */
#define MEASURE_PX 64
static void
measure_insets (GdkPaintable *icon, double *top, double *bottom)
{
  *top = *bottom = 0;
  if (icon == NULL)
    return;
  GtkSnapshot *snap = gtk_snapshot_new (); /* consumed below */
  gdk_paintable_snapshot (icon, snap, MEASURE_PX, MEASURE_PX);
  g_autoptr (GskRenderNode) node = gtk_snapshot_free_to_node (snap);
  if (node == NULL)
    return;
  cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_A8, MEASURE_PX, MEASURE_PX);
  cairo_t *cr = cairo_create (surface);
  gsk_render_node_draw (node, cr);
  cairo_destroy (cr);
  cairo_surface_flush (surface);
  const guint8 *data = cairo_image_surface_get_data (surface);
  int stride = cairo_image_surface_get_stride (surface);
  int first = -1, last = -1;
  for (int y = 0; data != NULL && y < MEASURE_PX; y++)
    for (int x = 0; x < MEASURE_PX; x++)
      if (data[y * stride + x] > 40) /* ignore faint shadows */
        {
          if (first < 0)
            first = y;
          last = y;
          break;
        }
  cairo_surface_destroy (surface);
  if (first >= 0)
    {
      *top = (double) first / MEASURE_PX;
      *bottom = (double) (MEASURE_PX - 1 - last) / MEASURE_PX;
    }
}

static void
layer_set_icon (DpStackView *self, Layer *l, GdkPaintable *icon)
{
  if (l->icon == icon)
    return;
  measure_insets (icon, &l->vis_top, &l->vis_bottom);
  if (l->icon != NULL)
    g_signal_handlers_disconnect_by_func (l->icon, on_icon_invalidated, self);
  g_set_object (&l->icon, icon);
  if (icon != NULL && !(gdk_paintable_get_flags (icon) & GDK_PAINTABLE_STATIC_CONTENTS))
    g_signal_connect_object (icon, "invalidate-contents", G_CALLBACK (on_icon_invalidated), self, 0);
}

static void
layer_free (gpointer data)
{
  Layer *l = data;
  if (l->icon != NULL)
    g_signal_handlers_disconnect_matched (l->icon, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, on_icon_invalidated, NULL);
  g_clear_object (&l->icon);
  g_clear_object (&l->thumb);
  g_free (l->key);
  g_free (l);
}

/* ---------------------------------------------------------------------------------------------- */
/* Geometry                                                                                       */

static guint
n_shown (DpStackView *self)
{
  guint n = 0;
  for (guint i = 0; i < self->layers->len; i++)
    n += ((Layer *) g_ptr_array_index (self->layers, i))->index >= 0;
  return n;
}

static gboolean
cards_shape (DpStackView *self)
{
  if (self->style.media_square)
    return FALSE;
  for (guint i = 0; i < self->layers->len; i++)
    {
      Layer *l = g_ptr_array_index (self->layers, i);
      if (l->index >= 0 && l->media)
        return TRUE;
    }
  return FALSE;
}

static double
ease (double t)
{
  t = CLAMP (t, 0.0, 1.0);
  return 1.0 - pow (1.0 - t, 3.0); /* ease-out cubic */
}

static double
lerp (double a, double b, double t)
{
  return a + (b - a) * t;
}

static double
progress (DpStackView *self)
{
  if (self->t0 == 0)
    return 1.0;
  GdkFrameClock *clock = gtk_widget_get_frame_clock (GTK_WIDGET (self));
  gint64 now = clock != NULL ? gdk_frame_clock_get_frame_time (clock) : g_get_monotonic_time ();
  return ease ((double) (now - self->t0) / ANIM_US);
}

static Geo
geo_now (DpStackView *self, const Layer *l)
{
  double t = progress (self);
  Geo g = {
    lerp (l->from.cx, l->to.cx, t), lerp (l->from.by, l->to.by, t), lerp (l->from.w, l->to.w, t),
    lerp (l->from.h, l->to.h, t),   lerp (l->from.angle, l->to.angle, t), lerp (l->from.depth, l->to.depth, t),
    lerp (l->from.alpha, l->to.alpha, t), lerp (l->from.mix, l->to.mix, t),
  };
  return g;
}

/* The front icon's size that fits @w × @h (icon shape). */
static double
icon_fit (DpStackView *self, double w, double h)
{
  double s = self->style.icon_size;
  s = MIN (s, h / (1.0 + 2 * ICON_MARGIN));
  s = MIN (s, w / (1.0 + 2 * ICON_MARGIN));
  return MAX (s, 1.0);
}

/* Size of the layer at front index @i in a card stack whose cards are @cw × @ch (an icon: a square). */
static void
card_layer_size (DpStackView *self, const Layer *l, int i, double cw, double ch, double *w, double *h)
{
  if (l->media && l->thumb != NULL)
    {
      *w = cw, *h = ch;
      if (self->style.keep_aspect)
        {
          double b = self->style.border ? self->style.border_width : 0;
          double tw = gdk_texture_get_width (l->thumb), th = gdk_texture_get_height (l->thumb);
          double k = MIN ((cw - 2 * b) / tw, (ch - 2 * b) / th);
          *w = tw * k + 2 * b, *h = th * k + 2 * b; /* its picture's shape */
        }
      return;
    }
  *w = *h = MIN (self->style.icon_size * pow (ICON_SHRINK, i), CARD_ICON_FIT * ch);
}

/* Targets of the shown layers (@shown: front first, @n of them) in a @w × @h area, into @out. */
static void
targets (DpStackView *self, Layer *const *shown, guint n, double w, double h, Geo *out)
{
  for (guint i = 0; i < n; i++)
    {
      const Layer *l = shown[i];
      Geo *g = &out[i];
      *g = (Geo) { .alpha = 1, .depth = i, .mix = l->thumb != NULL ? 1 : 0, .cx = w / 2 };
      if (self->style.rotation && i > 0)
        g->angle = fp_composite_tilt (l->seed) / 8.0 * MAX_TILT_DEG; /* ±MAX_TILT_DEG, stable per item */
    }
  if (n == 0)
    return;
  /* Every item behind shows the same distance (the spacing option, P29) above the one in front, measured
   * between what is drawn: a picture card's edge, an icon's visible top (not its square's). */
  double d = self->style.spacing;
  double avail_top, avail_bottom; /* the room the stack's visible part must fit in */
  if (!cards_shape (self))
    {
      /* Icons (and squared media): each one behind is smaller; the group is centred vertically below. */
      double s = icon_fit (self, w, h);
      for (guint i = 0; i < n; i++)
        out[i].w = out[i].h = s * pow (ICON_SHRINK, i);
      avail_top = ICON_MARGIN * s;
      avail_bottom = h - ICON_MARGIN * s;
    }
  else
    {
      /* Cards: the folder-preview stack across the width (or as tall as fits); cards behind are narrower. */
      double sw = MIN (w / (1.0 + 2 * CARD_PAD), h / (CARD_STACK_H + 2 * CARD_PAD));
      double ch = CARD_H * sw;
      for (guint i = 0; i < n; i++)
        card_layer_size (self, shown[i], i, sw - 2 * i * CARD_STEP * sw, ch, &out[i].w, &out[i].h);
      avail_top = CARD_PAD * sw;
      avail_bottom = h - CARD_PAD * sw;
    }
  /* Stack them with the front one's bottom at 0 (y grows down): visible tops @d apart; the bottom of each one
   * behind at least @d above the one in front's, so nothing hangs below it. */
  double vt[DP_STACK_VIEW_MAX], vb[DP_STACK_VIEW_MAX];
  for (guint i = 0; i < n; i++)
    {
      gboolean card = shown[i]->media && shown[i]->thumb != NULL;
      double it = card ? 0 : shown[i]->vis_top, ib = card ? 0 : shown[i]->vis_bottom;
      if (!card && self->style.card)
        it = 0.125 + 0.75 * it, ib = 0.125 + 0.75 * ib; /* the icon is drawn at 3/4 on its card */
      vt[i] = it * out[i].h, vb[i] = ib * out[i].h;
      if (i == 0)
        out[i].by = 0;
      else
        {
          double top_prev = out[i - 1].by - out[i - 1].h + vt[i - 1];
          double by = top_prev - d - vt[i] + out[i].h;
          double bottom_prev = out[i - 1].by - vb[i - 1];
          out[i].by = MIN (by, bottom_prev - d + vb[i]);
        }
    }
  /* Fit the visible part [highest top, front's bottom] into the room (shrinking around the front's bottom if
   * needed), then centre it vertically. */
  double highest = 0;
  for (guint i = 0; i < n; i++)
    highest = MIN (highest, out[i].by - out[i].h + vt[i]);
  double extent = (out[0].by - vb[0]) - highest;
  double room = MAX (avail_bottom - avail_top, 1.0);
  double f = extent > room ? room / extent : 1.0;
  double offset = avail_top + (room - extent * f) / 2 - highest * f; /* where y = 0 lands */
  for (guint i = 0; i < n; i++)
    {
      out[i].w *= f, out[i].h *= f;
      out[i].by = offset + out[i].by * f;
    }
}

/* Shown layers, front first (at most DP_STACK_VIEW_MAX). Returns how many. */
static guint
shown_layers (DpStackView *self, Layer **shown)
{
  guint n = 0;
  for (int i = 0; i < DP_STACK_VIEW_MAX; i++)
    for (guint k = 0; k < self->layers->len; k++)
      {
        Layer *l = g_ptr_array_index (self->layers, k);
        if (l->index == i)
          {
            shown[n++] = l;
            break;
          }
      }
  return n;
}

/* Sets every shown layer's target for a @w × @h area. */
static void
set_targets (DpStackView *self, double w, double h)
{
  Layer *shown[DP_STACK_VIEW_MAX] = { NULL };
  Geo geos[DP_STACK_VIEW_MAX];
  guint n = shown_layers (self, shown);
  targets (self, shown, n, w, h, geos);
  for (guint i = 0; i < n; i++)
    shown[i]->to = geos[i];
}

static void
tick_stop (DpStackView *self)
{
  if (self->tick_id != 0)
    gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->tick_id);
  self->tick_id = 0;
  self->t0 = 0;
}

static gboolean
animations_enabled (DpStackView *self)
{
  gboolean on = TRUE;
  g_object_get (gtk_widget_get_settings (GTK_WIDGET (self)), "gtk-enable-animations", &on, NULL);
  return on && gtk_widget_get_mapped (GTK_WIDGET (self));
}

/* Drops faded-out layers once the animation is over. */
static void
settle (DpStackView *self)
{
  for (guint i = self->layers->len; i-- > 0;)
    {
      Layer *l = g_ptr_array_index (self->layers, i);
      if (l->index < 0)
        g_ptr_array_remove_index (self->layers, i);
      else
        l->from = l->to;
    }
  tick_stop (self);
}

static gboolean
on_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  (void) data;
  DpStackView *self = DP_STACK_VIEW (widget);
  gtk_widget_queue_draw (widget);
  if (gdk_frame_clock_get_frame_time (clock) - self->t0 >= ANIM_US)
    {
      self->tick_id = 0; /* removed by returning REMOVE */
      self->t0 = 0;
      settle (self);
      return G_SOURCE_REMOVE;
    }
  return G_SOURCE_CONTINUE;
}

/* New targets for every layer; @animate moves them there from where they are drawn now. */
static void
relayout (DpStackView *self, gboolean animate)
{
  double w = self->alloc_w, h = self->alloc_h;
  if (w <= 0 || h <= 0)
    {
      /* Not allocated yet: everything appears at its target once it is. */
      for (guint i = 0; i < self->layers->len; i++)
        ((Layer *) g_ptr_array_index (self->layers, i))->fresh = TRUE;
      return;
    }
  animate = animate && animations_enabled (self);
  /* Where everything is drawn now, then the new targets. */
  Geo *now_all = g_new (Geo, self->layers->len);
  for (guint k = 0; k < self->layers->len; k++)
    now_all[k] = geo_now (self, g_ptr_array_index (self->layers, k));
  set_targets (self, w, h);
  for (guint k = 0; k < self->layers->len; k++)
    {
      Layer *l = g_ptr_array_index (self->layers, k);
      Geo now = now_all[k];
      Geo to = l->to;
      if (l->index < 0)
        {
          to = now; /* fading out where it is, sinking back a little */
          to.alpha = 0;
          to.depth = now.depth + 1;
        }
      if (l->fresh)
        {
          /* Appears: from its place, transparent and a little smaller. */
          now = to;
          now.alpha = 0;
          now.w *= 0.9, now.h *= 0.9;
          l->fresh = FALSE;
        }
      l->from = animate ? now : to;
      l->to = to;
    }
  g_free (now_all);
  if (!animate)
    {
      settle (self);
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return;
    }
  GdkFrameClock *clock = gtk_widget_get_frame_clock (GTK_WIDGET (self));
  self->t0 = clock != NULL ? gdk_frame_clock_get_frame_time (clock) : g_get_monotonic_time ();
  if (self->tick_id == 0)
    self->tick_id = gtk_widget_add_tick_callback (GTK_WIDGET (self), on_tick, NULL, NULL);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ---------------------------------------------------------------------------------------------- */
/* Drawing                                                                                        */

static void
rounded (GskRoundedRect *r, double x, double y, double w, double h, double radius)
{
  radius = CLAMP (radius, 0.0, MIN (w, h) / 2);
  gsk_rounded_rect_init_from_rect (r, &GRAPHENE_RECT_INIT (x, y, w, h), (float) radius);
}

/* @l's icon in the square at the bottom centre of the w × h rectangle at the origin. */
static void
draw_icon (DpStackView *self, GtkSnapshot *snap, const Layer *l, double w, double h)
{
  double s = MIN (w, h);
  double x = (w - s) / 2, y = h - s;
  double icon = self->style.card ? s * 0.75 : s;
  if (self->style.shadow)
    {
      /* The shadow of what is drawn (its alpha), so a folder casts a folder's shadow. */
      GskShadow shadow = { .color = { 0, 0, 0, 0.30f }, .dx = (float) (s * 0.004), .dy = (float) (s * 0.018),
                           .radius = (float) MAX (1.0, s * 0.045) };
      gtk_snapshot_push_shadow (snap, &shadow, 1);
    }
  if (self->style.card)
    {
      GdkRGBA fg;
      gtk_widget_get_color (GTK_WIDGET (self), &fg);
      fg.alpha = 0.10f;
      GskRoundedRect r;
      rounded (&r, x, y, s, s, s / 10);
      gtk_snapshot_push_rounded_clip (snap, &r);
      gtk_snapshot_append_color (snap, &fg, &r.bounds);
      gtk_snapshot_pop (snap);
    }
  if (l->icon != NULL)
    {
      gtk_snapshot_save (snap);
      gtk_snapshot_translate (snap, &GRAPHENE_POINT_INIT ((float) (x + (s - icon) / 2), (float) (y + (s - icon) / 2)));
      gdk_paintable_snapshot (l->icon, snap, icon, icon);
      gtk_snapshot_restore (snap);
    }
  if (self->style.shadow)
    gtk_snapshot_pop (snap);
}

/* @l's thumbnail as a card filling the w × h rectangle at the origin: border, corners, shadow. */
static void
draw_card (DpStackView *self, GtkSnapshot *snap, const Layer *l, double w, double h)
{
  const DpStackStyle *st = &self->style;
  double b = st->border ? MIN (st->border_width, MIN (w, h) / 4) : 0;
  GskRoundedRect outer, inner;
  rounded (&outer, 0, 0, w, h, st->corner_radius);
  rounded (&inner, b, b, w - 2 * b, h - 2 * b, MAX (0.0, st->corner_radius - b));
  if (st->shadow)
    {
      static const GdkRGBA black = { 0, 0, 0, 0.38f };
      gtk_snapshot_append_outset_shadow (snap, &outer, &black, (float) (w * 0.004), (float) (w * 0.014), 0,
                                         (float) MAX (2.0, w * 0.045));
    }
  if (b > 0)
    {
      gtk_snapshot_push_rounded_clip (snap, &outer);
      gtk_snapshot_append_color (snap, &st->border_color, &outer.bounds);
      gtk_snapshot_pop (snap);
    }
  double iw = gdk_texture_get_width (l->thumb), ih = gdk_texture_get_height (l->thumb);
  double pw = w - 2 * b, ph = h - 2 * b;
  double k = MAX (pw / iw, ph / ih); /* covers (cropped) unless the card already has its shape */
  gtk_snapshot_push_rounded_clip (snap, &inner);
  gtk_snapshot_append_scaled_texture (snap, l->thumb, GSK_SCALING_FILTER_TRILINEAR,
                                      &GRAPHENE_RECT_INIT ((float) (b + (pw - iw * k) / 2), (float) (b + (ph - ih * k) / 2),
                                                           (float) (iw * k), (float) (ih * k)));
  gtk_snapshot_pop (snap);
}

static void
draw_layer (DpStackView *self, GtkSnapshot *snap, const Layer *l, const Geo *g)
{
  if (g->alpha <= 0.005 || g->w < 1 || g->h < 1)
    return;
  double d = MAX (g->depth, 0.0), dim = 0, blur = 0, alpha = g->alpha;
  switch (self->style.depth)
    {
    case FP_DEPTH_BLUR:
      dim = d * DEPTH_DIM_BLUR;
      blur = d * g->w * DEPTH_BLUR;
      break;
    case FP_DEPTH_TRANSPARENCY:
      alpha *= MAX (0.1, 1.0 - d * DEPTH_ALPHA);
      break;
    case FP_DEPTH_DARKEN:
      dim = d * DEPTH_DIM_DARKEN;
      break;
    case FP_DEPTH_NONE:
    default:
      dim = d * DEPTH_DIM_NONE;
      break;
    }
  /* Effects are pushed under a plain translation and the tilt applied inside them: GTK draws a blur pushed
   * under a rotation in the wrong place. */
  gtk_snapshot_save (snap);
  gtk_snapshot_translate (snap, &GRAPHENE_POINT_INIT ((float) g->cx, (float) (g->by - g->h / 2)));
  int pushed = 0;
  if (alpha < 0.999)
    gtk_snapshot_push_opacity (snap, alpha), pushed++;
  if (blur >= 0.5)
    gtk_snapshot_push_blur (snap, blur), pushed++;
  if (dim > 0.001)
    {
      graphene_matrix_t m;
      float k = (float) MAX (0.0, 1.0 - dim);
      graphene_matrix_init_scale (&m, k, k, k);
      gtk_snapshot_push_color_matrix (snap, &m, graphene_vec4_zero ());
      pushed++;
    }
  gtk_snapshot_save (snap);
  if (fabs (g->angle) > 0.01)
    gtk_snapshot_rotate (snap, (float) g->angle);
  gtk_snapshot_translate (snap, &GRAPHENE_POINT_INIT ((float) (-g->w / 2), (float) (-g->h / 2)));
  gboolean card = l->thumb != NULL;
  double mix = card ? CLAMP (g->mix, 0.0, 1.0) : 0.0;
  if (mix <= 0.001)
    draw_icon (self, snap, l, g->w, g->h);
  else if (mix >= 0.999)
    draw_card (self, snap, l, g->w, g->h);
  else
    {
      gtk_snapshot_push_cross_fade (snap, mix);
      draw_icon (self, snap, l, g->w, g->h);
      gtk_snapshot_pop (snap);
      draw_card (self, snap, l, g->w, g->h);
      gtk_snapshot_pop (snap);
    }
  gtk_snapshot_restore (snap);
  while (pushed-- > 0)
    gtk_snapshot_pop (snap);
  gtk_snapshot_restore (snap);
}

typedef struct
{
  const Layer *l;
  Geo          g;
} Drawn;

static int
by_depth (gconstpointer a, gconstpointer b)
{
  double da = ((const Drawn *) a)->g.depth, db = ((const Drawn *) b)->g.depth;
  return da < db ? 1 : da > db ? -1 : 0; /* back first */
}

static void
dp_stack_view_snapshot (GtkWidget *widget, GtkSnapshot *snap)
{
  DpStackView *self = DP_STACK_VIEW (widget);
  guint n = self->layers->len;
  if (n == 0)
    return;
  Drawn *drawn = g_new (Drawn, n);
  double top = G_MAXDOUBLE, bottom = 0;
  for (guint i = 0; i < n; i++)
    {
      drawn[i].l = g_ptr_array_index (self->layers, i);
      drawn[i].g = geo_now (self, drawn[i].l);
      top = MIN (top, drawn[i].g.by - drawn[i].g.h);
      bottom = MAX (bottom, drawn[i].g.by);
    }
  qsort (drawn, n, sizeof *drawn, by_depth);
  gboolean fade = self->style.fade > 0 && bottom > top;
  if (fade)
    {
      /* Opaque above the fade, transparent at the stack's bottom edge. */
      double start = top + (bottom - top) * (1.0 - MIN (self->style.fade, 100) / 100.0);
      graphene_rect_t all = GRAPHENE_RECT_INIT (0, 0, self->alloc_w, self->alloc_h);
      GskColorStop stops[] = { { 0, { 0, 0, 0, 1 } }, { 1, { 0, 0, 0, 0 } } };
      gtk_snapshot_push_mask (snap, GSK_MASK_MODE_ALPHA);
      gtk_snapshot_append_linear_gradient (snap, &all, &GRAPHENE_POINT_INIT (0, (float) start),
                                           &GRAPHENE_POINT_INIT (0, (float) MAX (bottom, start + 1)), stops, 2);
      gtk_snapshot_pop (snap);
    }
  for (guint i = 0; i < n; i++)
    draw_layer (self, snap, drawn[i].l, &drawn[i].g);
  if (fade)
    gtk_snapshot_pop (snap);
  g_free (drawn);
}

/* ---------------------------------------------------------------------------------------------- */
/* Widget                                                                                         */

static void
dp_stack_view_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  (void) baseline;
  DpStackView *self = DP_STACK_VIEW (widget);
  if (width == self->alloc_w && height == self->alloc_h)
    return;
  gboolean first = self->alloc_w <= 0 || self->alloc_h <= 0;
  self->alloc_w = width, self->alloc_h = height;
  if (self->t0 != 0 && !first)
    {
      /* Resized mid-animation (the box follows the stack's shape): keep going towards the new targets. */
      set_targets (self, width, height);
      return;
    }
  relayout (self, FALSE);
}

static void
dp_stack_view_measure (GtkWidget *widget, GtkOrientation orientation, int for_size, int *min, int *nat,
                       int *min_baseline, int *nat_baseline)
{
  (void) widget, (void) orientation, (void) for_size;
  *min = *nat = 0; /* the box sizes us */
  *min_baseline = *nat_baseline = -1;
}

static void
dp_stack_view_unmap (GtkWidget *widget)
{
  DpStackView *self = DP_STACK_VIEW (widget);
  if (self->t0 != 0)
    settle (self);
  GTK_WIDGET_CLASS (dp_stack_view_parent_class)->unmap (widget);
}

static void
dp_stack_view_dispose (GObject *object)
{
  DpStackView *self = DP_STACK_VIEW (object);
  tick_stop (self);
  if (self->layers != NULL)
    g_ptr_array_set_size (self->layers, 0);
  G_OBJECT_CLASS (dp_stack_view_parent_class)->dispose (object);
}

static void
dp_stack_view_finalize (GObject *object)
{
  DpStackView *self = DP_STACK_VIEW (object);
  g_clear_pointer (&self->layers, g_ptr_array_unref);
  G_OBJECT_CLASS (dp_stack_view_parent_class)->finalize (object);
}

static void
dp_stack_view_class_init (DpStackViewClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS (klass);
  GtkWidgetClass *wc = GTK_WIDGET_CLASS (klass);
  oc->dispose = dp_stack_view_dispose;
  oc->finalize = dp_stack_view_finalize;
  wc->snapshot = dp_stack_view_snapshot;
  wc->size_allocate = dp_stack_view_size_allocate;
  wc->measure = dp_stack_view_measure;
  wc->unmap = dp_stack_view_unmap;
  gtk_widget_class_set_css_name (wc, "dp-stack");
  gtk_widget_class_set_accessible_role (wc, GTK_ACCESSIBLE_ROLE_IMG);
}

static void
dp_stack_view_init (DpStackView *self)
{
  self->layers = g_ptr_array_new_with_free_func (layer_free);
  self->style = (DpStackStyle) {
    .icon_size = 128, .border_color = { 1, 1, 1, 1 }, .border_width = 2, .corner_radius = 4, .shadow = TRUE,
    .rotation = TRUE, .keep_aspect = TRUE, .depth = FP_DEPTH_BLUR, .card = TRUE, .spacing = 12,
  };
  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
}

GtkWidget *
dp_stack_view_new (void)
{
  return g_object_new (DP_TYPE_STACK_VIEW, NULL);
}

static gboolean
style_equal (const DpStackStyle *a, const DpStackStyle *b)
{
  return a->icon_size == b->icon_size && a->border == b->border && gdk_rgba_equal (&a->border_color, &b->border_color) &&
         a->border_width == b->border_width && a->corner_radius == b->corner_radius && a->shadow == b->shadow &&
         a->rotation == b->rotation && a->keep_aspect == b->keep_aspect && a->depth == b->depth &&
         a->fade == b->fade && a->card == b->card && a->media_square == b->media_square && a->spacing == b->spacing;
}

void
dp_stack_view_set_style (DpStackView *self, const DpStackStyle *style)
{
  g_return_if_fail (DP_IS_STACK_VIEW (self) && style != NULL);
  if (style_equal (&self->style, style))
    return;
  self->style = *style;
  self->style.icon_size = CLAMP (self->style.icon_size, 16, 512);
  self->style.border_width = CLAMP (self->style.border_width, 0.0, 32.0);
  self->style.spacing = CLAMP (self->style.spacing, 0, 64);
  relayout (self, FALSE);
}

static Layer *
find_layer (DpStackView *self, const char *key)
{
  for (guint i = 0; key != NULL && i < self->layers->len; i++)
    {
      Layer *l = g_ptr_array_index (self->layers, i);
      if (g_strcmp0 (l->key, key) == 0)
        return l;
    }
  return NULL;
}

void
dp_stack_view_set_items (DpStackView *self, const DpStackViewItem *items, guint n_items)
{
  g_return_if_fail (DP_IS_STACK_VIEW (self));
  g_return_if_fail (n_items == 0 || items != NULL);
  n_items = MIN (n_items, DP_STACK_VIEW_MAX);
  for (guint i = 0; i < self->layers->len; i++)
    ((Layer *) g_ptr_array_index (self->layers, i))->index = -1;
  for (guint i = 0; i < n_items; i++)
    {
      Layer *l = find_layer (self, items[i].key);
      if (l == NULL || l->index >= 0) /* new (or a duplicate key) */
        {
          l = g_new0 (Layer, 1);
          l->key = g_strdup (items[i].key);
          l->fresh = TRUE;
          g_ptr_array_add (self->layers, l);
        }
      layer_set_icon (self, l, items[i].icon);
      if (!items[i].media)
        g_clear_object (&l->thumb);
      l->media = items[i].media;
      l->seed = items[i].seed;
      l->index = (int) i;
    }
  /* Nothing left to show: drop everything at once (the pane shows something else). */
  if (n_items == 0)
    {
      tick_stop (self);
      g_ptr_array_set_size (self->layers, 0);
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return;
    }
  relayout (self, TRUE);
}

void
dp_stack_view_set_thumbnail (DpStackView *self, const char *key, GdkTexture *thumbnail)
{
  g_return_if_fail (DP_IS_STACK_VIEW (self));
  Layer *l = find_layer (self, key);
  if (l == NULL || l->index < 0 || !l->media || l->thumb == thumbnail)
    return;
  g_set_object (&l->thumb, thumbnail);
  relayout (self, TRUE);
}

double
dp_stack_view_get_ratio (DpStackView *self, int *icon_height)
{
  g_return_val_if_fail (DP_IS_STACK_VIEW (self), 0);
  if (cards_shape (self))
    {
      if (icon_height != NULL)
        *icon_height = 0;
      return (1.0 + 2 * CARD_PAD) / (CARD_STACK_H + 2 * CARD_PAD);
    }
  if (icon_height != NULL)
    *icon_height = (int) ceil (self->style.icon_size * (1.0 + 2 * ICON_MARGIN) + (DP_STACK_VIEW_MAX - 1) * self->style.spacing);
  return 0;
}

guint
dp_stack_view_get_n_items (DpStackView *self)
{
  g_return_val_if_fail (DP_IS_STACK_VIEW (self), 0);
  return n_shown (self);
}

const char *
dp_stack_view_get_shape (DpStackView *self)
{
  g_return_val_if_fail (DP_IS_STACK_VIEW (self), NULL);
  return cards_shape (self) ? "cards" : "icons";
}

gboolean
dp_stack_view_is_animating (DpStackView *self)
{
  g_return_val_if_fail (DP_IS_STACK_VIEW (self), FALSE);
  return self->t0 != 0;
}

gboolean
dp_stack_view_has_thumbnail (DpStackView *self, const char *key)
{
  g_return_val_if_fail (DP_IS_STACK_VIEW (self), FALSE);
  Layer *l = find_layer (self, key);
  return l != NULL && l->index >= 0 && l->thumb != NULL;
}

guint
dp_stack_view_get_n_thumbnails (DpStackView *self)
{
  g_return_val_if_fail (DP_IS_STACK_VIEW (self), 0);
  guint n = 0;
  for (guint i = 0; i < self->layers->len; i++)
    {
      Layer *l = g_ptr_array_index (self->layers, i);
      n += l->index >= 0 && l->thumb != NULL;
    }
  return n;
}
