/* Phase 6 profiling tool (not a test): times each pipeline stage for frame 0 of every folder under a
 * directory, single-threaded, outside Nautilus.
 *   XDG_CACHE_HOME=<bench>/xdg-cache build/tests/bench-core <bench>/parent [width]
 * Stages: scan (enumerate + filter + sort), resolve (thumbnail lookup; generation if missing), composite
 * (decode + draw), png (encode for the disk cache). Prints per-stage average / p95 / max in ms. */
#include <locale.h>

#include "composite.h"
#include "config.h"
#include "scanner.h"
#include "thumbs.h"

static FpStyle grid_style; /* The v1 look, set in main. */

static int
cmp_double (gconstpointer a, gconstpointer b)
{
  double x = *(const double *) a, y = *(const double *) b;
  return x < y ? -1 : x > y;
}

static void
report (const char *name, GArray *ms)
{
  g_array_sort (ms, cmp_double);
  double sum = 0;
  for (guint i = 0; i < ms->len; i++)
    sum += g_array_index (ms, double, i);
  guint n = ms->len;
  g_print ("%-10s n=%u avg=%.2f ms  p95=%.2f ms  max=%.2f ms\n", name, n, n ? sum / n : 0,
           n ? g_array_index (ms, double, MIN (n - 1, n * 95 / 100)) : 0, n ? g_array_index (ms, double, n - 1) : 0);
}

int
main (int argc, char **argv)
{
  fp_style_init_default (&grid_style);
  setlocale (LC_ALL, "");
  if (argc < 2)
    {
      g_printerr ("usage: %s PARENT_DIR [frame-width]\n", argv[0]);
      return 2;
    }
  guint width = argc > 2 ? (guint) g_ascii_strtoull (argv[2], NULL, 10) : 88;
  g_autoptr (FpThumbs) thumbs = fp_thumbs_new (1);
  g_autoptr (GArray) t_scan = g_array_new (FALSE, FALSE, sizeof (double));
  g_autoptr (GArray) t_resolve = g_array_new (FALSE, FALSE, sizeof (double));
  g_autoptr (GArray) t_comp = g_array_new (FALSE, FALSE, sizeof (double));
  g_autoptr (GArray) t_png = g_array_new (FALSE, FALSE, sizeof (double));
  g_autoptr (GArray) t_total = g_array_new (FALSE, FALSE, sizeof (double));

  g_autoptr (GDir) d = g_dir_open (argv[1], 0, NULL);
  g_return_val_if_fail (d != NULL, 1);
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
  const char *n;
  while ((n = g_dir_read_name (d)) != NULL)
    g_ptr_array_add (names, g_strdup (n));
  g_ptr_array_sort_values (names, (GCompareFunc) g_strcmp0);

  for (guint i = 0; i < names->len; i++)
    {
      g_autofree char *path = g_build_filename (argv[1], g_ptr_array_index (names, i), NULL);
      g_autoptr (GFile) folder = g_file_new_for_path (path);
      gint64 t0 = g_get_monotonic_time ();
      g_autoptr (FpScanResult) scan = fp_scan_folder (folder, SCAN_WINDOW, NULL, NULL);
      gint64 t1 = g_get_monotonic_time ();
      if (scan == NULL)
        continue;
      g_autoptr (FpResolved) res = fp_thumbs_resolve (thumbs, scan, PREVIEW_SLOTS, NULL, NULL);
      gint64 t2 = g_get_monotonic_time ();
      FpCompositeItem items[PREVIEW_SLOTS] = { { NULL, 0 } };
      guint k = MIN (res->thumbs->len, (guint) PREVIEW_SLOTS);
      for (guint j = 0; j < k; j++)
        {
          FpThumb *t = g_ptr_array_index (res->thumbs, j);
          items[j].path = t->thumbnail_path;
          items[j].seed = j;
        }
      g_autoptr (GdkTexture) tex = fp_composite_frame (&grid_style, items, k, width, NULL);
      gint64 t3 = g_get_monotonic_time ();
      g_autoptr (GBytes) png = tex != NULL ? gdk_texture_save_to_png_bytes (tex) : NULL;
      gint64 t4 = g_get_monotonic_time ();
      double v;
      v = (t1 - t0) / 1000.0; g_array_append_val (t_scan, v);
      v = (t2 - t1) / 1000.0; g_array_append_val (t_resolve, v);
      v = (t3 - t2) / 1000.0; g_array_append_val (t_comp, v);
      v = (t4 - t3) / 1000.0; g_array_append_val (t_png, v);
      v = (t4 - t0) / 1000.0; g_array_append_val (t_total, v);
    }
  report ("scan", t_scan);
  report ("resolve", t_resolve);
  report ("composite", t_comp);
  report ("png", t_png);
  report ("total", t_total);
  return 0;
}
