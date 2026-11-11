/* setpriority(): POSIX, hidden by strict -std=c11 unless requested. */
#define _XOPEN_SOURCE 700
#include "workers.h"

#include <sys/resource.h>
#ifdef __GLIBC__
#include <malloc.h> /* malloc_trim */
#endif

struct _FpWorkers
{
  GThreadPool *pool;          /* Owned; drained and freed in fp_workers_free. */
  guint64      next_seq;      /* Main thread only. */
  gint         shutting_down; /* Atomic; read by pool threads. */
  gboolean     exclusive;     /* Dedicated threads: only then may run_job lower their priority. */
};

typedef struct
{
  GTask      *task;     /* Owned ref; returned exactly once by run_job (directly or via func). */
  FpWorkFunc  func;
  int         priority;
  guint64     seq;
} Job;

static void
job_free (Job *job)
{
  g_clear_object (&job->task);
  g_free (job);
}

/* GThreadPool sort function: lower priority value first, then FIFO. */
static gint
compare_jobs (gconstpointer a, gconstpointer b, gpointer user_data)
{
  (void) user_data;
  const Job *ja = a;
  const Job *jb = b;
  if (ja->priority != jb->priority)
    return ja->priority < jb->priority ? -1 : 1;
  return ja->seq < jb->seq ? -1 : (ja->seq > jb->seq ? 1 : 0);
}

/* Our pool threads run at the lowest CPU priority so background work never competes with Nautilus' UI
 * thread (PLAN.md §6.4). On Linux, setpriority(PRIO_PROCESS, 0, …) changes only the calling thread
 * (a documented NPTL difference from POSIX). The pool is exclusive, so these threads are ours alone and
 * GLib's shared threads (which Nautilus uses too) are not affected. $FP_WORKERS_NO_NICE disables it
 * (benchmark A/B only). */
static void
lower_thread_priority_once (void)
{
  static GPrivate done;
  if (g_private_get (&done) != NULL)
    return;
  g_private_set (&done, GINT_TO_POINTER (1));
  if (g_getenv ("FP_WORKERS_NO_NICE") == NULL)
    setpriority (PRIO_PROCESS, 0, 19);
}

/* Decoding thumbnails and drawing frames make large short-lived allocations in the worker threads; glibc
 * keeps the freed memory in per-thread arenas, which showed up as ~40 MB of "free but held" heap in the
 * Phase 6 browse benchmark. When the queue runs empty, give it back to the system (at most every 2 s). */
static void
trim_if_idle (FpWorkers *workers)
{
#ifdef __GLIBC__
  static gint64 last_trim_us; /* touched by pool threads only; a racy double trim is harmless */
  /* During fp_workers_free the pool is still valid (it waits for running jobs), but don't bother. */
  if (g_atomic_int_get (&workers->shutting_down) || g_thread_pool_unprocessed (workers->pool) != 0)
    return;
  gint64 now = g_get_monotonic_time ();
  if (now - last_trim_us < 2 * G_USEC_PER_SEC)
    return;
  last_trim_us = now;
  malloc_trim (0);
#else
  (void) workers;
#endif
}

/* Runs in a pool thread; consumes the job. @user_data is the FpWorkers, which outlives every job
 * because fp_workers_free waits for the pool to drain. */
static void
run_job (gpointer data, gpointer user_data)
{
  Job *job = data;
  FpWorkers *workers = user_data;
  if (workers->exclusive)
    lower_thread_priority_once ();
  GCancellable *cancellable = g_task_get_cancellable (job->task); /* Borrowed from the task. */
  g_autoptr (GError) error = NULL;

  if (g_atomic_int_get (&workers->shutting_down))
    {
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Worker pool shut down");
      g_task_return_error (job->task, g_steal_pointer (&error));
    }
  else if (g_cancellable_set_error_if_cancelled (cancellable, &error))
    g_task_return_error (job->task, g_steal_pointer (&error));
  else
    job->func (job->task, g_task_get_task_data (job->task), cancellable);

  job_free (job);
  trim_if_idle (workers);
}

FpWorkers *
fp_workers_new (guint max_threads)
{
  g_return_val_if_fail (max_threads > 0, NULL);

  FpWorkers *workers = g_new0 (FpWorkers, 1);
  /* Exclusive: dedicated threads (we lower their priority, see above). item_free_func only matters for
   * immediate shutdown, which we never use, but keeps ownership complete. */
  g_autoptr (GError) error = NULL;
  workers->pool = g_thread_pool_new_full (run_job, workers, (GDestroyNotify) job_free,
                                          (gint) max_threads, TRUE, &error);
  workers->exclusive = workers->pool != NULL;
  if (workers->pool == NULL)
    {
      /* Could not start dedicated threads: fall back to GLib's shared pool (never fails). */
      g_debug ("exclusive worker pool failed (%s); using shared threads", error->message);
      workers->pool = g_thread_pool_new_full (run_job, workers, (GDestroyNotify) job_free,
                                              (gint) max_threads, FALSE, NULL);
    }
  g_thread_pool_set_sort_function (workers->pool, compare_jobs, NULL);
  return workers;
}

void
fp_workers_free (FpWorkers *workers)
{
  if (workers == NULL)
    return;
  g_atomic_int_set (&workers->shutting_down, 1);
  /* immediate=FALSE: queued jobs still go through run_job (and return CANCELLED at once), so every task
   * is returned; wait=TRUE: no thread can still be using @workers after this. */
  /* Don't clear workers->pool before this returns: jobs still running may read it (trim_if_idle).
   * g_thread_pool_free waits for them, so after it returns nobody can. */
  g_thread_pool_free (workers->pool, FALSE, TRUE);
  workers->pool = NULL;
  g_free (workers);
}

void
fp_workers_push (FpWorkers  *workers,
                 GTask      *task,
                 int         priority,
                 FpWorkFunc  func)
{
  g_return_if_fail (workers != NULL);
  g_return_if_fail (G_IS_TASK (task));
  g_return_if_fail (func != NULL);

  Job *job = g_new0 (Job, 1);
  job->task = g_object_ref (task);
  job->func = func;
  job->priority = priority;
  job->seq = workers->next_seq++;

  g_autoptr (GError) error = NULL;
  if (!g_thread_pool_push (workers->pool, job, &error))
    {
      /* Only possible if no thread could be created; report through the normal callback path. */
      g_task_return_error (job->task, g_steal_pointer (&error));
      job_free (job);
    }
  /* Otherwise the pool owns @job until run_job frees it. */
}
