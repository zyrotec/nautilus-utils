/* Details pane: stall sampler. See dp_stall.h. */
#define _GNU_SOURCE 1
#include "dp_stall.h"

#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>

#define SAMPLE_SIG   (SIGRTMIN + 7)
#define MAX_SAMPLES  64
#define MAX_FRAMES   24
#define ARM_AFTER_US 3000

typedef struct
{
  int   n;
  void *frames[MAX_FRAMES];
} Sample;

static gboolean     enabled;     /* main thread */
static int          depth;       /* main thread: nesting of timed steps */
static pthread_t    main_thread;
static _Atomic gint64 armed_since; /* start of the outermost step, 0 = idle */
static Sample       samples[MAX_SAMPLES];
static atomic_int    n_samples;   /* samples taken in this step (may exceed MAX_SAMPLES) */

static void
on_sample (int sig)
{
  (void) sig;
  int i = atomic_fetch_add (&n_samples, 1);
  if (i < MAX_SAMPLES)
    samples[i].n = backtrace (samples[i].frames, MAX_FRAMES);
}

static gpointer
watcher (gpointer data)
{
  (void) data;
  for (;;)
    {
      g_usleep (1000);
      gint64 since = atomic_load (&armed_since);
      if (since != 0 && g_get_monotonic_time () - since > ARM_AFTER_US && atomic_load (&n_samples) < MAX_SAMPLES)
        pthread_kill (main_thread, SAMPLE_SIG);
    }
  return NULL;
}

static void
ensure_started (void)
{
  static gsize once = 0;
  if (!g_once_init_enter (&once))
    return;
  enabled = g_strcmp0 (g_getenv ("DP_STALL_SAMPLER"), "1") == 0;
  if (enabled)
    {
      void *warm[4];
      backtrace (warm, 4); /* loads libgcc now, so the handler doesn't allocate */
      main_thread = pthread_self ();
      struct sigaction sa;
      memset (&sa, 0, sizeof sa);
      sa.sa_handler = on_sample;
      sa.sa_flags = SA_RESTART;
      sigemptyset (&sa.sa_mask);
      sigaction (SAMPLE_SIG, &sa, NULL);
      sigset_t set; /* some libraries block signals on the main thread */
      sigemptyset (&set);
      sigaddset (&set, SAMPLE_SIG);
      pthread_sigmask (SIG_UNBLOCK, &set, NULL);
      g_thread_unref (g_thread_new ("dp-stall-watch", watcher, NULL));
      g_message ("stall sampler on");
    }
  g_once_init_leave (&once, 1);
}

void
dp_stall_step_begin (void)
{
  ensure_started ();
  if (!enabled || depth++ > 0)
    return;
  atomic_store (&n_samples, 0);
  atomic_store (&armed_since, g_get_monotonic_time ());
}

void
dp_stall_step_end (const char *what, gint64 wall_us)
{
  if (!enabled || depth == 0 || --depth > 0)
    return;
  atomic_store (&armed_since, 0);
  int n = MIN (atomic_load (&n_samples), MAX_SAMPLES);
  if (wall_us <= 16000)
    return;
  if (n == 0)
    {
      g_message ("stall: %s took %" G_GINT64_FORMAT " µs, no samples (signal %d not delivered?)", what, wall_us,
                 SAMPLE_SIG);
      return;
    }
  /* Distinct stacks (frames 2… : skip the handler and the signal trampoline), most frequent first. */
  GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (int i = 0; i < n; i++)
    {
      int skip = MIN (2, samples[i].n);
      char **sym = backtrace_symbols (samples[i].frames + skip, samples[i].n - skip);
      if (sym == NULL)
        continue;
      GString *s = g_string_new (NULL);
      for (int f = 0; f < samples[i].n - skip; f++)
        g_string_append_printf (s, "\n    %s", sym[f]);
      free (sym);
      char *key = g_string_free (s, FALSE);
      guint c = GPOINTER_TO_UINT (g_hash_table_lookup (seen, key));
      g_hash_table_replace (seen, key, GUINT_TO_POINTER (c + 1));
    }
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init (&it, seen);
  g_message ("stall: %s took %" G_GINT64_FORMAT " µs, %d samples:", what, wall_us, n);
  while (g_hash_table_iter_next (&it, &key, &val))
    g_message ("stall: %u× in:%s", GPOINTER_TO_UINT (val), (const char *) key);
  g_hash_table_unref (seen);
}
