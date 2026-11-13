#include "icon_art.h"

#define MAX_SVG_BYTES   (4u * 1024u * 1024u) /* a folder icon is a few KB; refuse anything absurd */
#define MAX_RENDER_PX   4096
#define MAX_RENDERS     32                   /* cached sizes per SVG (zoom levels × scales × 2 uses) */
#define BODY_PROBE_PX   256                  /* canvas size the opaque body is measured on */

struct _FpIconArt
{
  GdkPaintable *svg;        /* Owned: the parsed SVG (a GtkSvg). */
  double        width;      /* Intrinsic size, > 0. */
  double        height;
  GHashTable   *renders;    /* Owned. Key: gint64* (w << 32 | h, freed by the table) → GdkTexture* (owned). */
  gboolean      body_known; /* fp_icon_art_get_body has measured (result in @body_valid / @body). */
  gboolean      body_valid;
  graphene_rect_t body;
};

/* Draws @node (may be NULL = nothing) onto a transparent w×h canvas. Returns a new texture (caller owns). */
static GdkTexture *
node_to_texture (GskRenderNode *node, int w, int h)
{
  cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h); /* owned, freed below */
  if (cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS)
    {
      cairo_surface_destroy (surface);
      return NULL;
    }
  if (node != NULL)
    {
      cairo_t *cr = cairo_create (surface);
      gsk_render_node_draw (node, cr);
      cairo_destroy (cr);
    }
  cairo_surface_flush (surface);
  int stride = cairo_image_surface_get_stride (surface);
  g_autoptr (GBytes) bytes = g_bytes_new (cairo_image_surface_get_data (surface), (gsize) stride * (gsize) h);
  cairo_surface_destroy (surface);
  /* GDK_MEMORY_DEFAULT is documented to equal CAIRO_FORMAT_ARGB32 (gdkmemorytexture.h). */
  return gdk_memory_texture_new (w, h, GDK_MEMORY_DEFAULT, bytes, (gsize) stride);
}

/* Returns a new texture (caller owns) or NULL. */
GdkTexture *
fp_render_paintable (GdkPaintable *paintable, int width, int height)
{
  g_return_val_if_fail (GDK_IS_PAINTABLE (paintable), NULL);
  if (width < 1 || height < 1 || width > MAX_RENDER_PX || height > MAX_RENDER_PX)
    return NULL;
  GtkSnapshot *snapshot = gtk_snapshot_new (); /* consumed by free_to_node */
  gdk_paintable_snapshot (paintable, snapshot, width, height);
  g_autoptr (GskRenderNode) node = gtk_snapshot_free_to_node (snapshot);
  return node_to_texture (node, width, height);
}

gboolean
fp_texture_opaque_bounds (GdkTexture *texture, graphene_rect_t *out)
{
  g_return_val_if_fail (GDK_IS_TEXTURE (texture), FALSE);
  int w = gdk_texture_get_width (texture), h = gdk_texture_get_height (texture);
  gsize stride = (gsize) w * 4;
  g_autofree guint8 *px = g_malloc (stride * (gsize) h);
  g_autoptr (GdkTextureDownloader) d = gdk_texture_downloader_new (texture);
  gdk_texture_downloader_set_format (d, GDK_MEMORY_R8G8B8A8); /* straight alpha */
  gdk_texture_downloader_download_into (d, px, stride);

  int x0 = w, y0 = h, x1 = -1, y1 = -1;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      if (px[(gsize) y * stride + (gsize) x * 4 + 3] >= 128)
        {
          x0 = MIN (x0, x);
          x1 = MAX (x1, x);
          y0 = MIN (y0, y);
          y1 = MAX (y1, y);
        }
  if (x1 < 0)
    return FALSE;
  graphene_rect_init (out, (float) x0 / w, (float) y0 / h, (float) (x1 + 1 - x0) / w, (float) (y1 + 1 - y0) / h);
  return TRUE;
}

#if GTK_CHECK_VERSION(4, 22, 0)
/* GtkSvg::error handler: keeps (in *data, a GError** owned by the caller) the first error that means the
 * file isn't an SVG at all (invalid syntax / not XML). GtkSvg's other reports (unsupported attributes or
 * elements, e.g. Illustrator's data-name) are harmless: those parts are just ignored. */
static void
on_svg_error (GtkSvg *svg, GError *error, gpointer data)
{
  (void) svg;
  GError **first = data;
  if (*first == NULL && error != NULL &&
      (error->domain != GTK_SVG_ERROR || error->code == GTK_SVG_ERROR_INVALID_SYNTAX))
    *first = g_error_copy (error);
}
#endif

/* Returns a new FpIconArt (caller frees) or NULL with @error set. */
FpIconArt *
fp_icon_art_new_from_file (const char *path, GError **error)
{
  g_return_val_if_fail (path != NULL, NULL);
#if GTK_CHECK_VERSION(4, 22, 0)
  g_autoptr (GFile) file = g_file_new_for_path (path);
  g_autoptr (GFileInfo) info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE,
                                                  NULL, error);
  if (info == NULL)
    return NULL;
  if (g_file_info_get_size (info) > MAX_SVG_BYTES)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s is larger than %u bytes", path, MAX_SVG_BYTES);
      return NULL;
    }
  g_autofree char *data = NULL;
  gsize len = 0;
  if (!g_file_get_contents (path, &data, &len, error))
    return NULL;
  g_autoptr (GBytes) bytes = g_bytes_new_take (g_steal_pointer (&data), len);

  /* Only what a static icon needs: no animations, no external files, no GTK extensions. */
  g_autoptr (GtkSvg) svg = gtk_svg_new ();
  gtk_svg_set_features (svg, GTK_SVG_SYSTEM_RESOURCES);
  g_autoptr (GError) parse_error = NULL;
  gulong handler = g_signal_connect (svg, "error", G_CALLBACK (on_svg_error), &parse_error);
  gtk_svg_load_from_bytes (svg, bytes);
  g_signal_handler_disconnect (svg, handler);
  if (parse_error != NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s is not an SVG: %s", path, parse_error->message);
      return NULL;
    }
  double w = gdk_paintable_get_intrinsic_width (GDK_PAINTABLE (svg));
  double h = gdk_paintable_get_intrinsic_height (GDK_PAINTABLE (svg));
  if (w <= 0 || h <= 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s is not an SVG with a size", path);
      return NULL;
    }
  FpIconArt *art = g_new0 (FpIconArt, 1);
  art->svg = GDK_PAINTABLE (g_steal_pointer (&svg));
  art->width = w;
  art->height = h;
  art->renders = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, g_object_unref);
  return art;
#else
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "custom folder icons need GTK 4.22 (%s)", path);
  return NULL;
#endif
}

void
fp_icon_art_free (FpIconArt *art)
{
  if (art == NULL)
    return;
  g_clear_object (&art->svg);
  g_clear_pointer (&art->renders, g_hash_table_unref);
  g_free (art);
}

double
fp_icon_art_get_width (FpIconArt *art)
{
  g_return_val_if_fail (art != NULL, 0);
  return art->width;
}

double
fp_icon_art_get_height (FpIconArt *art)
{
  g_return_val_if_fail (art != NULL, 0);
  return art->height;
}

/* Returns a new reference (caller owns) or NULL. */
GdkTexture *
fp_icon_art_render (FpIconArt *art, int width, int height)
{
  g_return_val_if_fail (art != NULL, NULL);
  if (width < 1 || height < 1 || width > MAX_RENDER_PX || height > MAX_RENDER_PX)
    return NULL;
  gint64 key = ((gint64) width << 32) | (gint64) height;
  GdkTexture *cached = g_hash_table_lookup (art->renders, &key); /* borrowed */
  if (cached != NULL)
    return g_object_ref (cached);

  double scale = MIN (width / art->width, height / art->height);
  double dw = art->width * scale, dh = art->height * scale;
  GtkSnapshot *snapshot = gtk_snapshot_new (); /* consumed by free_to_node */
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT ((float) ((width - dw) / 2), (float) ((height - dh) / 2)));
  gdk_paintable_snapshot (art->svg, snapshot, dw, dh);
  g_autoptr (GskRenderNode) node = gtk_snapshot_free_to_node (snapshot);
  GdkTexture *texture = node_to_texture (node, width, height); /* owned; one ref goes to the cache */
  if (texture == NULL)
    return NULL;

  if (g_hash_table_size (art->renders) >= MAX_RENDERS)
    g_hash_table_remove_all (art->renders);
  gint64 *owned_key = g_new (gint64, 1);
  *owned_key = key;
  g_hash_table_insert (art->renders, owned_key, g_object_ref (texture));
  return texture;
}

gboolean
fp_icon_art_get_body (FpIconArt *art, graphene_rect_t *out)
{
  g_return_val_if_fail (art != NULL, FALSE);
  if (!art->body_known)
    {
      art->body_known = TRUE;
      g_autoptr (GdkTexture) probe = fp_icon_art_render (art, BODY_PROBE_PX, BODY_PROBE_PX);
      art->body_valid = probe != NULL && fp_texture_opaque_bounds (probe, &art->body);
      /* The probe size isn't a display size: don't keep it in the cache. */
      gint64 key = ((gint64) BODY_PROBE_PX << 32) | BODY_PROBE_PX;
      g_hash_table_remove (art->renders, &key);
    }
  if (art->body_valid)
    *out = art->body;
  return art->body_valid;
}
