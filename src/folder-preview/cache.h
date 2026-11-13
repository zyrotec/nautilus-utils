/* Frame cache and request pipeline (PLAN.md §6.1.4, §6.2 rules 8–9, §6.5 Phase 4).
 *
 * fp_cache_get_frame_async() is the one entry point the view hook needs. In a worker it:
 *   1. reads the folder's mtime (cache key part, D5);
 *   2. frame 0 only: looks in the disk cache  <dir>/v4/<sha1(style key + uri)>/<sec>.<usec>-<px>-<frames>.png  (or a
 *      "<sec>.<usec>-none" marker for folders with nothing to preview) and returns at once on a hit —
 *      no scan, no thumbnail decoding;
 *   3. otherwise scans (reusing the scan it already has for this mtime), resolves the needed thumbnails
 *      (frame k needs 4(k+1)), composites the frame, and for frame 0 writes the disk cache.
 * On the main thread the result goes into an in-memory LRU (max folders AND max bytes) that
 * fp_cache_peek_frame() reads instantly.
 *
 * Threading: create, request, peek, invalidate and unref on the main thread. The cache is atomically
 * ref-counted because in-flight requests hold a reference.
 * No Nautilus types (PLAN.md §4.2).
 */
#pragma once

#include <gtk/gtk.h>

#include "composite.h"
#include "thumbs.h"
#include "workers.h"

G_BEGIN_DECLS

typedef struct
{
  GdkTexture *texture;       /* Owned; NULL = no such frame (folder has no previewable files, or fewer
                              * than 4·frame+1 of them). */
  guint       frame;
  guint       frame_px;      /* Frame width; height = fp_composite_frame_height (style, frame_px). */
  guint       n_frames_hint; /* Upper bound on frames available (0 = nothing to preview). Some may turn
                              * out empty if thumbnails fail; treat a NULL frame as "wrap around". */
  gboolean    from_disk;     /* Served from the disk cache (no scan, no thumbnail decoding). */
} FpFrame;

void fp_frame_free (FpFrame *frame);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FpFrame, fp_frame_free)

typedef struct _FpCache FpCache;

/* @workers and @thumbs are borrowed: free the workers first (it drains every job), then drop the cache,
 * then free @thumbs. @disk_dir NULL = $XDG_CACHE_HOME/nautilus-utils/folder-preview. @max_folders and
 * @max_bytes bound the in-memory LRU (textures counted as width × height × 4).
 * Caller owns one reference (fp_cache_unref). */
FpCache *fp_cache_new (FpWorkers  *workers,
                       FpThumbs   *thumbs,
                       const char *disk_dir,
                       guint       max_folders,
                       gsize       max_bytes);

FpCache *fp_cache_ref   (FpCache *cache);
void     fp_cache_unref (FpCache *cache);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FpCache, fp_cache_unref)

/* Requests frame @frame of @folder at @frame_px pixels. @folder and @cancellable are ref'd.
 * @callback runs on the main thread after the in-memory cache was updated. */
void fp_cache_get_frame_async (FpCache             *cache,
                               GFile               *folder,
                               guint                frame,
                               guint                frame_px,
                               int                  priority,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data);

/* Caller owns the result (fp_frame_free). Errors: G_IO_ERROR_CANCELLED, or the folder's GIO error. */
FpFrame *fp_cache_get_frame_finish (GAsyncResult  *result,
                                    GError       **error);

/* Instant memory-only lookup. Returns TRUE if this (folder, frame, size) is cached; then *texture gets a
 * new reference (may be NULL = known to have no such frame) and *n_frames_hint is set. Moves the folder
 * to the front of the LRU. Doesn't check whether the folder changed: see fp_cache_invalidate(). */
gboolean fp_cache_peek_frame (FpCache     *cache,
                              const char  *uri,
                              guint        frame,
                              guint        frame_px,
                              GdkTexture **texture,
                              guint       *n_frames_hint);

/* Like fp_cache_peek_frame, but for any cached size: returns a new reference to the largest cached
 * texture of @frame for @uri, or NULL if none (or only "no such frame" is cached). Used to show something
 * sensible right after a zoom, until the exact size is built. Doesn't touch the LRU order. */
GdkTexture *fp_cache_peek_frame_any_size (FpCache    *cache,
                                          const char *uri,
                                          guint       frame);

/* Sets the look of new frames (starts as fp_style_init_default). If it differs from the current one,
 * everything in memory is forgotten and results of requests started before are dropped (they finish with
 * G_IO_ERROR_CANCELLED), so a frame of the old look is never shown. The disk cache keeps one set of frames
 * per look. Returns TRUE if the style changed. */
gboolean fp_cache_set_style (FpCache       *cache,
                             const FpStyle *style);

/* Borrowed; valid until the next fp_cache_set_style. */
const FpStyle *fp_cache_get_style (FpCache *cache);

/* Forgets everything in memory about @uri (e.g. Nautilus reported the folder changed). The disk cache is
 * keyed by mtime, so it needs no invalidation. Returns TRUE if something was cached. */
gboolean fp_cache_invalidate (FpCache    *cache,
                              const char *uri);

/* Queues a low-priority worker job that deletes disk cache directories of other CACHE_DISK_VERSIONs (frames
 * an older version of the extension wrote; they can never be read again). Only touches the disk cache. */
void fp_cache_purge_old_versions (FpCache *cache);

/* Number of folders / texture bytes currently held in memory (for tests and logs). */
guint fp_cache_get_n_folders (FpCache *cache);
gsize fp_cache_get_n_bytes   (FpCache *cache);

G_END_DECLS
