/* Stall sampler (diagnostics for PLAN.md §13.14 S1): with DP_STALL_SAMPLER=1 in the environment, a watcher
 * thread samples the main thread's stack every millisecond while a timed main-thread step (dp_timing.h) has
 * been running for more than 3 ms; a step that ends up slower than a frame logs where it was (the distinct
 * stacks, with how often each was seen). Off (no thread, no signal handler) without the variable. Linux only. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* A timed step starts / ends on the main thread (nested steps count once, the outermost). */
void dp_stall_step_begin (void);
void dp_stall_step_end   (const char *what,
                          gint64      wall_us);

G_END_DECLS
