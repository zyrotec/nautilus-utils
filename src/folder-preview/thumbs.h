/* Thumbnail resolution (PLAN.md §6.2 rules 3–4, Phase 3).
 *
 * Turns a scan result into a list of usable freedesktop thumbnail paths, in the scan's natural order:
 * reuse existing thumbnails (shared ~/.cache/thumbnails, so Nautilus benefits too), generate missing
 * ones with GnomeDesktopThumbnailFactory in a worker, record failures (e.g. audio without cover art)
 * and fall through to the next candidate. Uses "large" (256 px) thumbnails, like Nautilus.
 * No Nautilus types (PLAN.md §4.2).
 */
#pragma once

#include <gio/gio.h>

#include "scanner.h"
#include "workers.h"

G_BEGIN_DECLS

typedef struct
{
  GFile       *file;            /* Owned. */
  char        *name;            /* Owned; on-disk name. */
  char        *thumbnail_path;  /* Owned; never NULL. */
  FpMediaKind  kind;
} FpThumb;

void fp_thumb_free (FpThumb *thumb);

typedef struct
{
  FpScanResult *scan;          /* Owned reference to the scan this was resolved from. */
  GPtrArray    *thumbs;        /* Owned; FpThumb* in natural order, at most `wanted`. */
  guint         n_generated;   /* Thumbnails created by this call (0 = everything was cached). */
  guint         n_failed;      /* Candidates dropped because no thumbnail could be made. */
  gboolean      needs_rescan;  /* Fewer than `wanted`, every candidate was tried, and the scan says more
                                * eligible files exist: rescanning (which skips the new failure records)
                                * can find more. */
} FpResolved;

void fp_resolved_free (FpResolved *resolved);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FpResolved, fp_resolved_free)

typedef struct _FpThumbs FpThumbs;

/* Creates @n_factories thumbnail factories (one per worker thread that may resolve at once).
 * Caller owns the result; free with fp_thumbs_free() only after the FpWorkers that ran its jobs has
 * been freed (jobs borrow it). Main thread only. */
FpThumbs *fp_thumbs_new (guint n_factories);
void      fp_thumbs_free (FpThumbs *thumbs);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FpThumbs, fp_thumbs_free)

/* Synchronous; blocking I/O and child processes, so worker threads (or tests) only.
 * Resolves up to @wanted thumbnails from @scan (borrowed; a ref is kept in the result).
 * Errors: only G_IO_ERROR_CANCELLED. Caller owns the result (fp_resolved_free). */
FpResolved *fp_thumbs_resolve (FpThumbs      *thumbs,
                               FpScanResult  *scan,
                               guint          wanted,
                               GCancellable  *cancellable,
                               GError       **error);

/* Queues fp_thumbs_resolve on @workers. @scan is ref'd; @thumbs is borrowed (see fp_thumbs_new).
 * @callback runs in the calling thread's default main context. Main thread only. */
void fp_thumbs_resolve_async (FpThumbs            *thumbs,
                              FpWorkers           *workers,
                              FpScanResult        *scan,
                              guint                wanted,
                              int                  priority,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data);

/* Caller owns the result (fp_resolved_free). NULL with @error set on failure. */
FpResolved *fp_thumbs_resolve_finish (GAsyncResult  *result,
                                      GError       **error);

G_END_DECLS
