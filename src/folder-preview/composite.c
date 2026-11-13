#include "composite.h"

#include <math.h>

#include "config.h"

/* Thumbnails are at most 1024 px (xx-large); anything bigger is not a thumbnail and is refused so a
 * hostile file in the cache can't make us allocate huge buffers. */
#define MAX_SOURCE_PX 1024

/* Fraction of its section a photo may use (before tilting); tilted corners spill into FRAME_PAD. */
#define CELL_FILL 0.96

void
fp_style_init_default (FpStyle *style)
{
  *style = (FpStyle) {
    .layout = FP_LAYOUT_GRID,
    .stack_step = 1,
    .border = TRUE,
    .border_r = 0xff, .border_g = 0xff, .border_b = 0xff,
    .shadow = TRUE,
    .rotation = TRUE,
    .keep_aspect = TRUE,
    .depth = TRUE,
  };
}

gboolean
fp_style_equal (const FpStyle *a, const FpStyle *b)
{
  return a->layout == b->layout && a->stack_step == b->stack_step && a->border == b->border &&
         a->border_r == b->border_r && a->border_g == b->border_g && a->border_b == b->border_b &&
         a->shadow == b->shadow && a->rotation == b->rotation && a->keep_aspect == b->keep_aspect &&
         a->depth == b->depth;
}

/* Returns a new string (caller frees). */
char *
fp_style_key (const FpStyle *style)
{
  gboolean stacked = style->layout == FP_LAYOUT_STACKED;
  return g_strdup_printf ("%s%u-b%d%02x%02x%02x-s%d-r%d-a%d-d%d", stacked ? "stack" : "grid",
                          stacked ? style->stack_step : 0u, style->border != FALSE, style->border_r,
                          style->border_g, style->border_b, style->shadow != FALSE, style->rotation != FALSE,
                          style->keep_aspect != FALSE, stacked && style->depth); /* depth: stacked only */
}

guint
fp_style_slots (const FpStyle *style)
{
  return style->layout == FP_LAYOUT_STACKED ? STACK_CARDS : PREVIEW_SLOTS;
}

guint
fp_style_step (const FpStyle *style)
{
  if (style->layout != FP_LAYOUT_STACKED)
    return PREVIEW_SLOTS;
  return style->stack_step >= STACK_CARDS ? STACK_CARDS : 1;
}

guint
fp_composite_frame_height (const FpStyle *style, guint width)
{
  if (style->layout == FP_LAYOUT_STACKED)
    return (guint) lround ((double) width * (STACK_H + 2 * STACK_PAD) / (1.0 + 2 * STACK_PAD));
  return (guint) lround ((double) width * FRAME_FACE_H / FRAME_FACE_W);
}

/* A small, well-mixed integer hash (so similar seeds give unrelated values). */
static guint32
mix (guint32 x)
{
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

/* Maps @seed (and a @salt per quantity) to [-1, 1]. */
static double
unit (guint seed, guint32 salt)
{
  return (double) mix ((guint32) seed ^ salt) / (double) G_MAXUINT32 * 2.0 - 1.0;
}

double
fp_composite_tilt (guint seed)
{
  return unit (seed, 0x9e3779b9U) * FRAME_MAX_TILT_DEG;
}

/* Decodes @path with GTK's thread-safe loader into a new cairo image surface (caller owns; destroy with
 * cairo_surface_destroy), or NULL if it can't be read, decoded, or is unreasonably large. */
static cairo_surface_t *
load_surface (const char *path)
{
  g_autoptr (GdkTexture) texture = gdk_texture_new_from_filename (path, NULL);
  if (texture == NULL)
    return NULL;
  int w = gdk_texture_get_width (texture);
  int h = gdk_texture_get_height (texture);
  if (w <= 0 || h <= 0 || w > MAX_SOURCE_PX || h > MAX_SOURCE_PX)
    return NULL;

  cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
  if (cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS)
    {
      cairo_surface_destroy (surface);
      return NULL;
    }
  cairo_surface_flush (surface);
  /* GDK_MEMORY_DEFAULT is documented to equal CAIRO_FORMAT_ARGB32 (gdkmemorytexture.h). */
  g_autoptr (GdkTextureDownloader) downloader = gdk_texture_downloader_new (texture);
  gdk_texture_downloader_set_format (downloader, GDK_MEMORY_DEFAULT);
  gdk_texture_downloader_download_into (downloader, cairo_image_surface_get_data (surface),
                                        (gsize) cairo_image_surface_get_stride (surface));
  cairo_surface_mark_dirty (surface);
  return surface;
}

void
fp_composite_slot_rect (guint n_items, guint index, guint width, double *x, double *y, double *w, double *h)
{
  /* The face area: the frame minus the FRAME_PAD margin on each side. */
  FpStyle grid;
  fp_style_init_default (&grid);
  double fw = width / (1.0 + 2 * FRAME_PAD);
  double fh = fp_composite_frame_height (&grid, width) / (1.0 + 2 * FRAME_PAD);
  double ox = fw * FRAME_PAD, oy = fh * FRAME_PAD;
  double hw = fw / 2, hh = fh / 2;

  switch (CLAMP (n_items, 1u, 4u))
    {
    case 1: /* the whole area */
      *x = ox, *y = oy, *w = fw, *h = fh;
      break;
    case 2: /* left half, right half */
      *x = ox + hw * (index % 2), *y = oy, *w = hw, *h = fh;
      break;
    case 3: /* two on top, one centred below */
      if (index < 2)
        *x = ox + hw * index, *y = oy;
      else
        *x = ox + hw / 2, *y = oy + hh;
      *w = hw, *h = hh;
      break;
    default: /* quarters */
      *x = ox + hw * (index % 2), *y = oy + hh * (index / 2), *w = hw, *h = hh;
      break;
    }
}

void
fp_composite_card_rect (guint index, guint width, double *x, double *y, double *w, double *h)
{
  double sw = width / (1.0 + 2 * STACK_PAD); /* stack width */
  double inset = MIN (index, (guint) STACK_CARDS - 1) * STACK_STEP * sw;
  *x = STACK_PAD * sw + inset;
  *y = STACK_PAD * sw + (STACK_CARDS - 1) * STACK_STEP * sw - inset;
  *w = sw - 2 * inset;
  *h = STACK_CARD_H * sw;
}

/* Sets the style's border colour as the cairo source. */
static void
set_border_colour (cairo_t *cr, const FpStyle *style)
{
  cairo_set_source_rgb (cr, style->border_r / 255.0, style->border_g / 255.0, style->border_b / 255.0);
}

/* Adds a rounded rectangle path. */
static void
rounded_rect (cairo_t *cr, double x, double y, double w, double h, double r)
{
  r = CLAMP (r, 0.0, MIN (w, h) / 2);
  cairo_new_sub_path (cr);
  cairo_arc (cr, x + w - r, y + r, r, -G_PI / 2, 0);
  cairo_arc (cr, x + w - r, y + h - r, r, 0, G_PI / 2);
  cairo_arc (cr, x + r, y + h - r, r, G_PI / 2, G_PI);
  cairo_arc (cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
  cairo_close_path (cr);
}

/* In-place blur of a w×h image with @bpp bytes per pixel (1 = A8, 4 = premultiplied ARGB32): three box
 * blurs of radius @r each way, close to a Gaussian. Pixels outside count as transparent. */
static void
box_blur (guint8 *data, int w, int h, int stride, int bpp, int r)
{
  if (r < 1 || w < 1 || h < 1)
    return;
  int n = MAX (w, h);
  g_autofree guint32 *line = g_new0 (guint32, (gsize) n); /* zeroed: every read is provably initialised */
  int div = 2 * r + 1;
  for (int pass = 0; pass < 3; pass++)
    for (int dir = 0; dir < 2; dir++)
      {
        int len = dir == 0 ? w : h, lines = dir == 0 ? h : w;
        for (int l = 0; l < lines; l++)
          for (int c = 0; c < bpp; c++)
            {
              /* Element k of this line is at data[base + k·step]. */
              gsize base = dir == 0 ? (gsize) l * (gsize) stride + (gsize) c : (gsize) l * (gsize) bpp + (gsize) c;
              gsize step = dir == 0 ? (gsize) bpp : (gsize) stride;
              for (int k = 0; k < len; k++)
                line[k] = data[base + (gsize) k * step];
              guint32 sum = 0;
              for (int k = 0; k <= MIN (r, len - 1); k++)
                sum += line[k];
              for (int k = 0; k < len; k++)
                {
                  data[base + (gsize) k * step] = (guint8) (sum / (guint32) div);
                  if (k + r + 1 < len)
                    sum += line[k + r + 1];
                  if (k - r >= 0)
                    sum -= line[k - r];
                }
            }
      }
}

/* One photo or card, drawn upright into its own surface first, so that its drop shadow can follow its real
 * shape (a transparent picture casts the shadow of what is drawn, not of a box) and it can be blurred. */
typedef struct
{
  cairo_surface_t *surface; /* Owned (cairo_surface_destroy), ARGB32; NULL if it couldn't be created. */
  double           w, h;    /* The item's size; it is drawn at (m, m) in @surface. */
  double           m;       /* Transparent margin on every side (room for blur and shadow). */
} Item;

/* Creates an item surface for a w×h item with margin m; returns a cairo context drawing at (m, m), or NULL. */
static cairo_t *
item_begin (Item *item, double w, double h, double m)
{
  item->w = w, item->h = h, item->m = m;
  int sw = (int) ceil (w + 2 * m), sh = (int) ceil (h + 2 * m);
  item->surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, sw, sh);
  if (cairo_surface_status (item->surface) != CAIRO_STATUS_SUCCESS)
    {
      g_clear_pointer (&item->surface, cairo_surface_destroy);
      return NULL;
    }
  cairo_t *cr = cairo_create (item->surface); /* caller destroys */
  cairo_set_antialias (cr, CAIRO_ANTIALIAS_GOOD);
  cairo_translate (cr, m, m);
  return cr;
}

/* Paints @img into the w×h box at the context's origin: fitted (aspect kept, centred) or covering it. */
static void
paint_image (cairo_t *cr, cairo_surface_t *img, double w, double h, gboolean cover)
{
  double iw = cairo_image_surface_get_width (img), ih = cairo_image_surface_get_height (img);
  double scale = cover ? MAX (w / iw, h / ih) : MIN (w / iw, h / ih);
  cairo_translate (cr, (w - iw * scale) / 2, (h - ih * scale) / 2);
  cairo_scale (cr, scale, scale);
  cairo_set_source_surface (cr, img, 0, 0);
  cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_GOOD);
  cairo_paint (cr);
}

/* Draws @item centred at (cx, cy), tilted by @angle degrees, after its drop shadow (if @shadow > 0: blur
 * radius in pixels; the shadow is the blurred alpha of the item, offset down). Frees the item surface. */
static void
item_finish (cairo_t *cr, Item *item, double cx, double cy, double angle, double shadow, double blur)
{
  if (item->surface == NULL)
    return;
  cairo_surface_flush (item->surface);
  int sw = cairo_image_surface_get_width (item->surface), sh = cairo_image_surface_get_height (item->surface);
  if (blur >= 1)
    {
      box_blur (cairo_image_surface_get_data (item->surface), sw, sh, cairo_image_surface_get_stride (item->surface),
                4, (int) round (blur));
      cairo_surface_mark_dirty (item->surface);
    }

  cairo_save (cr);
  cairo_translate (cr, cx, cy);
  if (angle != 0)
    cairo_rotate (cr, angle * G_PI / 180.0);
  double ox = -item->w / 2 - item->m, oy = -item->h / 2 - item->m;
  if (shadow >= 1)
    {
      cairo_surface_t *mask = cairo_image_surface_create (CAIRO_FORMAT_A8, sw, sh); /* owned, freed below */
      if (cairo_surface_status (mask) == CAIRO_STATUS_SUCCESS)
        {
          cairo_t *mcr = cairo_create (mask);
          cairo_set_source_surface (mcr, item->surface, 0, 0);
          cairo_paint (mcr); /* copies the alpha */
          cairo_destroy (mcr);
          cairo_surface_flush (mask);
          box_blur (cairo_image_surface_get_data (mask), sw, sh, cairo_image_surface_get_stride (mask), 1,
                    (int) round (shadow));
          cairo_surface_mark_dirty (mask);
          cairo_set_source_rgba (cr, 0, 0, 0, 0.42);
          cairo_mask_surface (cr, mask, ox + shadow * 0.15, oy + shadow * 0.6);
        }
      cairo_surface_destroy (mask);
    }
  cairo_set_source_surface (cr, item->surface, ox, oy);
  cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_GOOD);
  cairo_paint (cr);
  cairo_restore (cr);
  g_clear_pointer (&item->surface, cairo_surface_destroy);
}

/* Draws one grid photo centred at (cx, cy) in a cw×ch section, tilted by @seed: fitted (aspect kept) or
 * filling the section (cropped), with optional border and drop shadow. */
static void
draw_photo (cairo_t *cr, const FpStyle *style, cairo_surface_t *img, double cx, double cy, double cw, double ch,
            guint seed)
{
  double iw = cairo_image_surface_get_width (img);
  double ih = cairo_image_surface_get_height (img);
  double border = style->border ? MAX (1.0, round (MIN (cw, ch) * 0.045)) : 0.0;
  double aw = cw * CELL_FILL - 2 * border, ah = ch * CELL_FILL - 2 * border; /* room for the picture */
  if (aw <= 0 || ah <= 0)
    return;
  double pw = aw, ph = ah; /* picture size */
  if (style->keep_aspect)
    {
      double scale = MIN (aw / iw, ah / ih);
      pw = iw * scale, ph = ih * scale;
    }
  double fw = pw + 2 * border, fh = ph + 2 * border; /* photo size incl. border */
  double shadow = style->shadow ? MAX (1.0, MIN (cw, ch) * 0.035) : 0.0;

  Item item;
  cairo_t *icr = item_begin (&item, fw, fh, ceil (shadow * 3 + 2));
  if (icr == NULL)
    return;
  if (border > 0)
    {
      cairo_rectangle (icr, 0, 0, fw, fh);
      set_border_colour (icr, style);
      cairo_fill (icr);
    }
  cairo_rectangle (icr, border, border, pw, ph);
  cairo_clip (icr);
  cairo_translate (icr, border, border);
  paint_image (icr, img, pw, ph, !style->keep_aspect);
  cairo_destroy (icr);
  item_finish (cr, &item, cx, cy, style->rotation ? fp_composite_tilt (seed) : 0, shadow, 0);
}

/* Draws stacked card @depth (0 = front) in the card rectangle x, y, w, h: the picture fills the card
 * (cropped), or with keep_aspect the card takes the picture's shape (fitted, centred, top-aligned so the
 * stack's top edges stay in order); a blank card if @img is NULL. Further back = darker (and blurred with
 * the depth effect). */
static void
draw_card (cairo_t *cr, const FpStyle *style, cairo_surface_t *img, double x, double y, double w, double h,
           double radius, guint seed, guint depth)
{
  double border = style->border ? MAX (1.0, round (w * 0.022)) : 0.0;
  double cw = w, ch = h; /* this card's size */
  if (img != NULL && style->keep_aspect)
    {
      double iw = cairo_image_surface_get_width (img), ih = cairo_image_surface_get_height (img);
      double scale = MIN ((w - 2 * border) / iw, (h - 2 * border) / ih);
      cw = iw * scale + 2 * border, ch = ih * scale + 2 * border;
    }
  double shadow = style->shadow ? MAX (1.0, w * 0.022) : 0.0;
  double dim = depth * (style->depth ? STACK_DEPTH_DIM : STACK_DIM);
  double blur = style->depth ? depth * w * STACK_DEPTH_BLUR : 0.0;

  Item item;
  cairo_t *icr = item_begin (&item, cw, ch, ceil (MAX (shadow, blur) * 3 + 2));
  if (icr == NULL)
    return;
  if (border > 0)
    {
      rounded_rect (icr, 0, 0, cw, ch, radius);
      set_border_colour (icr, style);
      cairo_fill (icr);
    }
  double pw = cw - 2 * border, ph = ch - 2 * border;
  cairo_save (icr);
  rounded_rect (icr, border, border, pw, ph, MAX (0.0, radius - border));
  cairo_clip (icr);
  cairo_translate (icr, border, border);
  if (img != NULL)
    paint_image (icr, img, pw, ph, TRUE);
  else if (border == 0)
    {
      cairo_set_source_rgb (icr, 0.95, 0.95, 0.95); /* blank card (with a border it is the border colour) */
      cairo_paint (icr);
    }
  cairo_restore (icr);
  if (dim > 0)
    {
      /* Darken only what is drawn (ATOP), so a transparent picture stays transparent. */
      cairo_set_operator (icr, CAIRO_OPERATOR_ATOP);
      cairo_set_source_rgba (icr, 0, 0, 0, dim);
      cairo_paint (icr);
    }
  cairo_destroy (icr);
  double angle = style->rotation ? unit (seed, 0x9e3779b9U) * STACK_MAX_TILT_DEG : 0;
  item_finish (cr, &item, x + w / 2, y + ch / 2, angle, shadow, blur);
}

GdkTexture *
fp_composite_frame (const FpStyle         *style,
                    const FpCompositeItem *items,
                    guint                  n_items,
                    guint                  width,
                    guint                 *n_unreadable)
{
  g_return_val_if_fail (style != NULL, NULL);
  g_return_val_if_fail (n_items <= fp_style_slots (style), NULL);
  g_return_val_if_fail (n_items == 0 || items != NULL, NULL);

  if (n_unreadable != NULL)
    *n_unreadable = 0;
  if (width < 16 || width > 4096)
    return NULL;
  guint height = MAX (1u, fp_composite_frame_height (style, width));

  cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, (int) width, (int) height);
  if (cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS)
    {
      cairo_surface_destroy (surface);
      return NULL;
    }
  cairo_t *cr = cairo_create (surface); /* starts fully transparent */
  cairo_set_antialias (cr, CAIRO_ANTIALIAS_GOOD);

  gboolean stacked = style->layout == FP_LAYOUT_STACKED;
  double radius = STACK_RADIUS * width / (1.0 + 2 * STACK_PAD);
  /* Stacked: back to front, so the front card (item 0) ends up on top. */
  for (guint k = 0; k < n_items; k++)
    {
      guint i = stacked ? n_items - 1 - k : k;
      cairo_surface_t *img = items[i].path != NULL ? load_surface (items[i].path) : NULL; /* owned */
      if (items[i].path != NULL && img == NULL && n_unreadable != NULL)
        (*n_unreadable)++;
      double sx, sy, sw, sh;
      if (stacked)
        {
          fp_composite_card_rect (i, width, &sx, &sy, &sw, &sh);
          draw_card (cr, style, img, sx, sy, sw, sh, radius, items[i].seed, i);
        }
      else if (img != NULL)
        {
          fp_composite_slot_rect (n_items, i, width, &sx, &sy, &sw, &sh);
          draw_photo (cr, style, img, sx + sw / 2, sy + sh / 2, sw, sh, items[i].seed);
        }
      if (img != NULL)
        cairo_surface_destroy (img);
    }
  cairo_destroy (cr);
  cairo_surface_flush (surface);

  /* Copy the pixels into a texture (the surface is freed right after). */
  int stride = cairo_image_surface_get_stride (surface);
  g_autoptr (GBytes) bytes = g_bytes_new (cairo_image_surface_get_data (surface), (gsize) stride * height);
  cairo_surface_destroy (surface);
  return gdk_memory_texture_new ((int) width, (int) height, GDK_MEMORY_DEFAULT, bytes, (gsize) stride);
}
