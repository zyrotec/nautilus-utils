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
  };
}

gboolean
fp_style_equal (const FpStyle *a, const FpStyle *b)
{
  return a->layout == b->layout && a->stack_step == b->stack_step && a->border == b->border &&
         a->border_r == b->border_r && a->border_g == b->border_g && a->border_b == b->border_b &&
         a->shadow == b->shadow && a->rotation == b->rotation;
}

/* Returns a new string (caller frees). */
char *
fp_style_key (const FpStyle *style)
{
  return g_strdup_printf ("%s%u-b%d%02x%02x%02x-s%d-r%d", style->layout == FP_LAYOUT_STACKED ? "stack" : "grid",
                          style->layout == FP_LAYOUT_STACKED ? style->stack_step : 0u, style->border != FALSE,
                          style->border_r, style->border_g, style->border_b, style->shadow != FALSE,
                          style->rotation != FALSE);
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

/* Soft shadow under a w×h shape centred at the origin: a few stacked translucent shapes, offset down-right.
 * @radius < 0 = plain rectangles. */
static void
draw_shadow (cairo_t *cr, double w, double h, double radius, double s)
{
  for (int i = 3; i >= 1; i--)
    {
      double grow = s * i * 0.6;
      if (radius < 0)
        cairo_rectangle (cr, -w / 2 - grow + s * 0.6, -h / 2 - grow + s, w + 2 * grow, h + 2 * grow);
      else
        rounded_rect (cr, -w / 2 - grow + s * 0.6, -h / 2 - grow + s, w + 2 * grow, h + 2 * grow, radius + grow);
      cairo_set_source_rgba (cr, 0, 0, 0, 0.07);
      cairo_fill (cr);
    }
}

/* Draws one grid photo centred at (cx, cy), fitted in a cw×ch section, tilted by @seed. */
static void
draw_photo (cairo_t *cr, const FpStyle *style, cairo_surface_t *img, double cx, double cy, double cw, double ch,
            guint seed)
{
  double iw = cairo_image_surface_get_width (img);
  double ih = cairo_image_surface_get_height (img);
  double border = style->border ? MAX (1.0, round (MIN (cw, ch) * 0.045)) : 0.0;

  /* Fit the bordered photo inside the cell, keeping the image's aspect ratio. */
  double scale = MIN ((cw * CELL_FILL - 2 * border) / iw, (ch * CELL_FILL - 2 * border) / ih);
  if (scale <= 0)
    return;
  double pw = iw * scale, ph = ih * scale; /* picture size */
  double fw = pw + 2 * border, fh = ph + 2 * border; /* photo size incl. border */

  cairo_save (cr);
  cairo_translate (cr, cx, cy);
  if (style->rotation)
    cairo_rotate (cr, fp_composite_tilt (seed) * G_PI / 180.0);

  if (style->shadow)
    draw_shadow (cr, fw, fh, -1, MAX (1.0, MIN (cw, ch) * 0.025));

  if (border > 0)
    {
      cairo_rectangle (cr, -fw / 2, -fh / 2, fw, fh);
      set_border_colour (cr, style);
      cairo_fill (cr);
    }

  /* The picture, downscaled with a proper filter. */
  cairo_rectangle (cr, -pw / 2, -ph / 2, pw, ph);
  cairo_clip (cr);
  cairo_translate (cr, -pw / 2, -ph / 2);
  cairo_scale (cr, scale, scale);
  cairo_set_source_surface (cr, img, 0, 0);
  cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_GOOD);
  cairo_paint (cr);
  cairo_restore (cr);
}

/* Draws one stacked card (x, y, w, h before tilting) with @img filling it (cropped), or a blank card if
 * @img is NULL; darkened by @dim (0 = front card). */
static void
draw_card (cairo_t *cr, const FpStyle *style, cairo_surface_t *img, double x, double y, double w, double h,
           double radius, guint seed, double dim)
{
  cairo_save (cr);
  cairo_translate (cr, x + w / 2, y + h / 2);
  if (style->rotation)
    cairo_rotate (cr, unit (seed, 0x9e3779b9U) * STACK_MAX_TILT_DEG * G_PI / 180.0);

  if (style->shadow)
    draw_shadow (cr, w, h, radius, MAX (1.0, w * 0.012));

  double border = style->border ? MAX (1.0, round (w * 0.022)) : 0.0;
  if (border > 0)
    {
      rounded_rect (cr, -w / 2, -h / 2, w, h, radius);
      set_border_colour (cr, style);
      cairo_fill (cr);
    }

  /* The picture area: inside the border. */
  double pw = w - 2 * border, ph = h - 2 * border;
  cairo_save (cr);
  rounded_rect (cr, -pw / 2, -ph / 2, pw, ph, MAX (0.0, radius - border));
  cairo_clip (cr);
  if (img != NULL)
    {
      double iw = cairo_image_surface_get_width (img);
      double ih = cairo_image_surface_get_height (img);
      double scale = MAX (pw / iw, ph / ih); /* cover: fill the card, crop the rest */
      cairo_translate (cr, -iw * scale / 2, -ih * scale / 2);
      cairo_scale (cr, scale, scale);
      cairo_set_source_surface (cr, img, 0, 0);
      cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_GOOD);
      cairo_paint (cr);
    }
  else if (border == 0)
    {
      cairo_set_source_rgb (cr, 0.95, 0.95, 0.95); /* blank card (with a border it is the border colour) */
      cairo_paint (cr);
    }
  cairo_restore (cr);

  if (dim > 0)
    {
      rounded_rect (cr, -w / 2, -h / 2, w, h, radius);
      cairo_set_source_rgba (cr, 0, 0, 0, dim);
      cairo_fill (cr);
    }
  cairo_restore (cr);
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
          draw_card (cr, style, img, sx, sy, sw, sh, radius, items[i].seed, i * STACK_DIM);
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
