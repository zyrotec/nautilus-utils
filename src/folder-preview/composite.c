#include "composite.h"

#include <math.h>

#include "config.h"

/* Thumbnails are at most 1024 px (xx-large); anything bigger is not a thumbnail and is refused so a
 * hostile file in the cache can't make us allocate huge buffers. */
#define MAX_SOURCE_PX 1024

/* Fraction of its section a photo may use (before tilting); tilted corners spill into FRAME_PAD. */
#define CELL_FILL 0.96

guint
fp_composite_frame_height (guint width)
{
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
  double fw = width / (1.0 + 2 * FRAME_PAD);
  double fh = fp_composite_frame_height (width) / (1.0 + 2 * FRAME_PAD);
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

/* Draws one photo centred at (cx, cy), fitted in a cw×ch section, tilted by @seed. */
static void
draw_photo (cairo_t *cr, cairo_surface_t *img, double cx, double cy, double cw, double ch, guint seed)
{
  double iw = cairo_image_surface_get_width (img);
  double ih = cairo_image_surface_get_height (img);
  double border = MAX (1.0, round (MIN (cw, ch) * 0.045));

  /* Fit the bordered photo inside the cell, keeping the image's aspect ratio. */
  double scale = MIN ((cw * CELL_FILL - 2 * border) / iw, (ch * CELL_FILL - 2 * border) / ih);
  if (scale <= 0)
    return;
  double pw = iw * scale, ph = ih * scale; /* picture size */
  double fw = pw + 2 * border, fh = ph + 2 * border; /* photo size incl. border */

  cairo_save (cr);
  cairo_translate (cr, cx, cy);
  cairo_rotate (cr, fp_composite_tilt (seed) * G_PI / 180.0);

  /* Soft shadow: a few stacked translucent rectangles, offset down-right. */
  double s = MAX (1.0, MIN (cw, ch) * 0.025);
  for (int i = 3; i >= 1; i--)
    {
      double grow = s * i * 0.6;
      cairo_rectangle (cr, -fw / 2 - grow + s * 0.6, -fh / 2 - grow + s, fw + 2 * grow, fh + 2 * grow);
      cairo_set_source_rgba (cr, 0, 0, 0, 0.07);
      cairo_fill (cr);
    }

  /* White border. */
  cairo_rectangle (cr, -fw / 2, -fh / 2, fw, fh);
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_fill (cr);

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

GdkTexture *
fp_composite_frame (const FpCompositeItem *items,
                    guint                  n_items,
                    guint                  width,
                    guint                 *n_unreadable)
{
  g_return_val_if_fail (n_items <= PREVIEW_SLOTS, NULL);
  g_return_val_if_fail (n_items == 0 || items != NULL, NULL);

  if (n_unreadable != NULL)
    *n_unreadable = 0;
  if (width < 16 || width > 4096)
    return NULL;
  guint height = MAX (1u, fp_composite_frame_height (width));

  cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, (int) width, (int) height);
  if (cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS)
    {
      cairo_surface_destroy (surface);
      return NULL;
    }
  cairo_t *cr = cairo_create (surface); /* starts fully transparent */
  cairo_set_antialias (cr, CAIRO_ANTIALIAS_GOOD);

  for (guint i = 0; i < n_items; i++)
    {
      if (items[i].path == NULL)
        continue;
      cairo_surface_t *img = load_surface (items[i].path);
      if (img == NULL)
        {
          if (n_unreadable != NULL)
            (*n_unreadable)++;
          continue;
        }
      double sx, sy, sw, sh;
      fp_composite_slot_rect (n_items, i, width, &sx, &sy, &sw, &sh);
      draw_photo (cr, img, sx + sw / 2, sy + sh / 2, sw, sh, items[i].seed);
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
