/* Folder scanner: finds the first eligible media files of a folder, off the main thread.
 *
 * No Nautilus types here (PLAN.md §4.2), only GLib/GIO, so it can be tested standalone.
 *
 * Rules (PLAN.md §6.1.1): direct children only; skip hidden/backup files and directories; classify by
 * standard::fast-content-type (image/…, video/…, audio/…); natural filename order
 * (g_utf8_collate_key_for_filename on the display name); native (file://) folders only.
 * Files that the freedesktop thumbnail cache already marks as failed (e.g. audio without cover art)
 * are excluded. Thumbnail info is looked up only for the files we keep (PLAN.md D6: requesting
 * it for every child made 10k-file scans 3–4× slower).
 */
#pragma once

#include <gio/gio.h>

#include "workers.h"

G_BEGIN_DECLS

typedef enum
{
  FP_MEDIA_NONE = 0,
  FP_MEDIA_IMAGE,
  FP_MEDIA_VIDEO,
  FP_MEDIA_AUDIO,
} FpMediaKind;

typedef struct
{
  GFile   *file;            /* Owned; freed by fp_candidate_free(). */
  char    *name;            /* Owned; on-disk file name (bytes). */
  char    *display_name;    /* Owned; UTF-8 display name. */
  char    *content_type;    /* Owned; fast content type, e.g. "image/png". */
  char    *collate_key;     /* Owned; natural-order sort key. */
  char    *thumbnail_path;  /* Owned; existing valid freedesktop thumbnail, or NULL if none yet. */
  guint64  mtime;           /* File modification time (seconds); thumbnail generation needs it. */
  FpMediaKind kind;
} FpCandidate;

void fp_candidate_free (FpCandidate *candidate);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FpCandidate, fp_candidate_free)

/* Immutable after creation and atomically ref-counted, so it can be shared with worker threads.
 * Allocated with g_atomic_rc_box; take refs with fp_scan_result_ref(), drop with fp_scan_result_unref(). */
typedef struct
{
  GFile     *folder;        /* Owned. */
  guint64    mtime_sec;     /* Folder time::modified — cache key part (PLAN.md §6.1.4). */
  guint32    mtime_usec;    /* Folder time::modified-usec. */
  GPtrArray *candidates;    /* Owned; FpCandidate* (freed with the array), in natural order,
                             * at most `window` entries. */
  guint      n_eligible;    /* Eligible children seen before excluding cached failures. */
  gboolean   more_available;/* TRUE if eligible files exist beyond the returned candidates. */
} FpScanResult;

/* Returns @result with one more reference. Thread-safe. */
FpScanResult *fp_scan_result_ref (FpScanResult *result);
/* Drops one reference; frees everything when it was the last. NULL is allowed. Thread-safe. */
void fp_scan_result_unref (FpScanResult *result);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FpScanResult, fp_scan_result_unref)

/* Attributes the scanner enumerates with. Only ones GLib can fill without stat()ing every entry (the type
 * comes from the directory entry; symlinks are followed). is-hidden, is-backup and fast-content-type each
 * made GLib stat every entry (2 000-file folder: 2.6 ms → 9.4 ms), although all three depend only on the
 * name, so the scanner computes them itself (PLAN.md D18). The mtime is read only for kept files. */
#define FP_SCAN_ENUM_ATTRIBUTES "standard::name,standard::display-name,standard::type"

/* Classifies one enumerated child. Pure function, no I/O. @info is borrowed. */
FpMediaKind fp_scanner_classify (GFileInfo *info);

/* Synchronous scan; does blocking I/O, so call it only from a worker thread (or tests).
 * Returns up to @window candidates. Errors: G_IO_ERROR_NOT_SUPPORTED (not native),
 * G_IO_ERROR_NOT_DIRECTORY, G_IO_ERROR_CANCELLED, or any GIO error.
 * Caller owns the returned reference (fp_scan_result_unref). @folder is borrowed. */
FpScanResult *fp_scan_folder (GFile         *folder,
                              guint          window,
                              GCancellable  *cancellable,
                              GError       **error);

/* ---- Asynchronous front end on the shared worker pool ---- */

/* Queues a scan of @folder on @workers. Lower @priority runs first. @callback runs in the calling
 * thread's default main context. @folder and @cancellable are ref'd. Main thread only. */
void fp_scan_async (FpWorkers           *workers,
                    GFile               *folder,
                    guint                window,
                    int                  priority,
                    GCancellable        *cancellable,
                    GAsyncReadyCallback  callback,
                    gpointer             user_data);

/* Caller owns the returned reference (fp_scan_result_unref). NULL with @error set on failure. */
FpScanResult *fp_scan_finish (GAsyncResult  *result,
                              GError       **error);

G_END_DECLS
