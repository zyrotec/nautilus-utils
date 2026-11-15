/* Main-thread step timing for the "slow main-thread step" debug log (PLAN.md §4.4, §13.14 stalls).
 * Besides wall and CPU time it records why a step waited: voluntary context switches (blocked on a lock, I/O,
 * poll), involuntary ones (preempted: the CPU was busy with other threads) and page faults. Linux only
 * (RUSAGE_THREAD, needs _GNU_SOURCE before the first system header); elsewhere the extra fields are 0. */
#pragma once

#include <glib.h>

#include "dp_stall.h"
#include <sys/resource.h>
#include <time.h>

typedef struct
{
  gint64 wall_us, cpu_us;
  long   vcsw, ivcsw, minflt, majflt;
} DpStepSample;

static inline void
dp_step_read (DpStepSample *s)
{
  s->wall_us = g_get_monotonic_time ();
  s->cpu_us = s->vcsw = s->ivcsw = s->minflt = s->majflt = 0;
#ifdef RUSAGE_THREAD
  struct rusage ru;
  if (getrusage (RUSAGE_THREAD, &ru) == 0)
    {
      s->cpu_us = (gint64) (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000 + ru.ru_utime.tv_usec + ru.ru_stime.tv_usec;
      s->vcsw = ru.ru_nvcsw, s->ivcsw = ru.ru_nivcsw, s->minflt = ru.ru_minflt, s->majflt = ru.ru_majflt;
    }
#endif
}

/* Starts a step: reads the counters (and arms the stall sampler). */
static inline void
dp_step_sample (DpStepSample *s)
{
  dp_stall_step_begin ();
  dp_step_read (s);
}

/* Wall µs since @start; logs the step if it took more than a frame (16 ms). */
static inline gint64
dp_step_end (const DpStepSample *start, const char *domain_what)
{
  DpStepSample e;
  dp_step_read (&e);
  gint64 dt = e.wall_us - start->wall_us;
  dp_stall_step_end (domain_what, dt);
  if (dt > 16000)
    g_debug ("slow main-thread step: %s took %" G_GINT64_FORMAT " µs (cpu %" G_GINT64_FORMAT
             " µs, blocked %ld×, preempted %ld×, faults %ld minor %ld major)",
             domain_what, dt, e.cpu_us - start->cpu_us, e.vcsw - start->vcsw, e.ivcsw - start->ivcsw,
             e.minflt - start->minflt, e.majflt - start->majflt);
  return dt;
}
