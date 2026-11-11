/* PLAN.md §6.5 Phase 2 acceptance: scanning a 10k-file folder never blocks the main loop.
 *
 * A 16 ms timeout measures how late it fires while scans run; any firing > 50 ms late fails.
 * Also reports the time spent inside each completion callback (the only scanner work on the main
 * thread). Not run under valgrind (it serialises threads, so lateness there says nothing about us).
 */
#include <locale.h>

#include "scanner.h"
#include "config.h"
#include "test-util.h"

#define PROBE_MS        16
#define LATE_LIMIT_MS   50

typedef struct
{
  gint64 expected_us;   /* When the probe should fire next. */
  gint64 max_late_us;
  gint64 sum_late_us;
  guint  ticks;
  guint  late_events;   /* Firings more than LATE_LIMIT_MS late. */
} Probe;

static gboolean
probe_tick (gpointer data)
{
  Probe *p = data;
  gint64 now = g_get_monotonic_time ();
  gint64 late = now - p->expected_us;
  if (late < 0)
    late = 0;
  if (late > p->max_late_us)
    p->max_late_us = late;
  p->sum_late_us += late;
  p->ticks++;
  if (late > LATE_LIMIT_MS * 1000)
    {
      p->late_events++;
      g_test_message ("probe fired %.1f ms late", late / 1000.0);
    }
  p->expected_us = now + PROBE_MS * 1000;
  return G_SOURCE_CONTINUE;
}

typedef struct
{
  guint  done;
  guint  candidates_total;
  gint64 max_callback_us;
} Stats;

static void
on_scanned (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) source;
  gint64 t0 = g_get_monotonic_time ();
  Stats *s = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (FpScanResult) r = fp_scan_finish (res, &error);
  g_assert_no_error (error);
  s->candidates_total += r->candidates->len;
  gint64 dt = g_get_monotonic_time () - t0;
  if (dt > s->max_callback_us)
    s->max_callback_us = dt;
  s->done++;
}

static char *root;

static void
test_latency_10k (void)
{
  g_autofree char *big = tu_mkdir (root, "10k");
  tu_touch_n (big, "IMG_%05u.jpg", 10000);
  /* Some noise that must be filtered out. */
  tu_touch_n (big, "notes%u.txt", 500);
  g_autofree char *mid = tu_mkdir (root, "2k");
  tu_touch_n (mid, "clip%u.mp4", 2000);

  g_autoptr (FpWorkers) workers = fp_workers_new (WORKER_THREADS);
  g_autoptr (GFile) big_file = g_file_new_for_path (big);
  g_autoptr (GFile) mid_file = g_file_new_for_path (mid);

  Probe probe = { .expected_us = g_get_monotonic_time () + PROBE_MS * 1000 };
  guint probe_id = g_timeout_add (PROBE_MS, probe_tick, &probe);

  Stats stats = { 0 };
  const guint n_big = 10, n_mid = 10;
  gint64 t0 = g_get_monotonic_time ();
  for (guint i = 0; i < n_big; i++)
    fp_scan_async (workers, big_file, SCAN_WINDOW, 0, NULL, on_scanned, &stats);
  for (guint i = 0; i < n_mid; i++)
    fp_scan_async (workers, mid_file, SCAN_WINDOW, 0, NULL, on_scanned, &stats);
  tu_wait_for (&stats.done, n_big + n_mid);
  gint64 elapsed = g_get_monotonic_time () - t0;
  g_clear_handle_id (&probe_id, g_source_remove);

  g_test_message ("%u scans (10×10.5k + 10×2k entries) in %.0f ms; probe ticks=%u max-late=%.1f ms "
                  "avg-late=%.2f ms late-events(>%d ms)=%u; max callback=%.3f ms",
                  n_big + n_mid, elapsed / 1000.0, probe.ticks, probe.max_late_us / 1000.0,
                  probe.ticks ? probe.sum_late_us / 1000.0 / probe.ticks : 0.0, LATE_LIMIT_MS,
                  probe.late_events, stats.max_callback_us / 1000.0);

  g_assert_cmpuint (stats.candidates_total, ==, (n_big + n_mid) * SCAN_WINDOW);
  g_assert_cmpuint (probe.ticks, >, 0);
  g_assert_cmpuint (probe.late_events, ==, 0);
}

int
main (int argc, char **argv)
{
  root = tu_make_tmpdir ("fp-test-latency");
  g_autofree char *cache = g_build_filename (root, "xdg-cache", NULL);
  g_setenv ("XDG_CACHE_HOME", cache, TRUE);
  /* Nautilus (a GTK app) runs in the user's locale; collation needs this to honour LC_ALL. */
  setlocale (LC_ALL, "");
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/latency/scan-10k", test_latency_10k);
  int ret = g_test_run ();
  tu_rm_rf (root);
  g_clear_pointer (&root, g_free);
  return ret;
}
