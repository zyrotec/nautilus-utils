/* Shared, prioritised worker pool for all blocking work (scans, thumbnail generation, later decoding).
 *
 * One pool caps concurrency across every job type (PLAN.md §6.3 WORKER_THREADS). Jobs are GTasks: the job
 * function runs in a pool thread and must return the task exactly once (g_task_return_*); the task's
 * callback then runs in the submitter's main context. No Nautilus types (PLAN.md §4.2).
 */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _FpWorkers FpWorkers;

/* Runs in a pool thread. Must return @task exactly once. @task_data is g_task_get_task_data(@task)
 * (borrowed), @cancellable is the task's cancellable (borrowed, may be NULL). */
typedef void (*FpWorkFunc) (GTask        *task,
                            gpointer      task_data,
                            GCancellable *cancellable);

/* Caller owns the pool; free with fp_workers_free(). */
FpWorkers *fp_workers_new (guint max_threads);

/* Marks the pool as shutting down, then waits until every queued and running job has finished.
 * Jobs that hadn't started return G_IO_ERROR_CANCELLED without running their function, so every
 * submitted task is still returned (its callback runs when the submitter's main context iterates).
 * Running jobs are not interrupted. Main thread only. Anything jobs borrow (e.g. FpThumbs) may be
 * freed only after this returns. */
void fp_workers_free (FpWorkers *workers);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FpWorkers, fp_workers_free)

/* Queues @task (a ref is taken) to run @func. Lower @priority runs first; FIFO within a priority.
 * If the task's cancellable is already cancelled when the job starts, the task returns
 * G_IO_ERROR_CANCELLED and @func is not called. Main thread only. */
void fp_workers_push (FpWorkers  *workers,
                      GTask      *task,
                      int         priority,
                      FpWorkFunc  func);

G_END_DECLS
