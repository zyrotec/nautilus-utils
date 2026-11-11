#include "cache.h"

#include <string.h>
#include <glib/gstdio.h>

#include "composite.h"
#include "config.h"
#include "scanner.h"

/* ================================================================================================ */
/* FpFrame                                                                                          */

void
fp_frame_free (FpFrame *frame)
{
  if (frame == NULL)
    return;
  g_clear_object (&frame->texture);
  g_free (frame);
}

/* ================================================================================================ */
/* Cache object and in-memory LRU (main thread only, except the immutable config fields)            */

typedef struct
{
  char         *uri;           /* Owned; also the key in FpCache.entries (the table borrows it). */
  guint64       mtime_sec;
  guint32       mtime_usec;
  FpScanResult *scan;          /* Owned ref, or NULL if only disk-cached frames are known. */
  GHashTable   *frames;        /* Owned. Key: gint64* (frame << 32 | px), freed by the table.
                                * Value: GdkTexture* owned by the table, or NULL = "no such frame". */
  guint         n_frames_hint;
  gsize         bytes;         /* Sum of the texture sizes in @frames. */
  GList        *lru_link;      /* Link in FpCache.lru; the queue owns the link, data borrows us. */
} Entry;

struct _FpCache
{
  /* Immutable after fp_cache_new; also read by worker threads. */
  FpWorkers  *workers;     /* Borrowed (see header for shutdown order). */
  FpThumbs   *thumbs;      /* Borrowed. */
  char       *disk_dir;    /* Owned. */
  guint       max_folders;
  gsize       max_bytes;

  /* Main thread only. */
  GHashTable *entries;     /* Owned. uri (borrowed from Entry) → Entry* (owned; entry_free). */
  GQueue      lru;         /* Head = most recently used. Data borrowed from @entries. */
  gsize       n_bytes;
};

static void
clear_texture (gpointer texture)
{
  if (texture != NULL)
    g_object_unref (texture);
}

static gsize
texture_bytes (GdkTexture *texture)
{
  if (texture == NULL)
    return 0;
  return (gsize) gdk_texture_get_width (texture) * (gsize) gdk_texture_get_height (texture) * 4;
}

static gint64
frame_key (guint frame, guint px)
{
  return (gint64) (((guint64) frame << 32) | (guint64) px);
}

static Entry *
entry_new (const char *uri, guint64 mtime_sec, guint32 mtime_usec)
{
  Entry *e = g_new0 (Entry, 1);
  e->uri = g_strdup (uri);
  e->mtime_sec = mtime_sec;
  e->mtime_usec = mtime_usec;
  e->frames = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, clear_texture);
  return e;
}

/* Value-destroy function of FpCache.entries. The caller has already unlinked @e from the LRU. */
static void
entry_free (gpointer data)
{
  Entry *e = data;
  g_clear_pointer (&e->uri, g_free);
  g_clear_pointer (&e->scan, fp_scan_result_unref);
  g_clear_pointer (&e->frames, g_hash_table_unref);
  g_free (e);
}

/* Called by g_atomic_rc_box_release_full when the last reference goes (any thread: by then nobody
 * else can be using the cache). */
static void
cache_clear (gpointer data)
{
  FpCache *cache = data;
  g_queue_clear (&cache->lru);                       /* frees the links; entries are freed next */
  g_clear_pointer (&cache->entries, g_hash_table_unref);
  g_clear_pointer (&cache->disk_dir, g_free);
}

FpCache *
fp_cache_new (FpWorkers  *workers,
              FpThumbs   *thumbs,
              const char *disk_dir,
              guint       max_folders,
              gsize       max_bytes)
{
  g_return_val_if_fail (workers != NULL, NULL);
  g_return_val_if_fail (thumbs != NULL, NULL);
  g_return_val_if_fail (max_folders > 0, NULL);

  FpCache *cache = g_atomic_rc_box_new0 (FpCache);
  cache->workers = workers;
  cache->thumbs = thumbs;
  cache->disk_dir = disk_dir != NULL ? g_strdup (disk_dir)
                                     : g_build_filename (g_get_user_cache_dir (), "nautilus-utils", "folder-preview", NULL);
  cache->max_folders = max_folders;
  cache->max_bytes = max_bytes;
  cache->entries = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, entry_free);
  g_queue_init (&cache->lru);
  return cache;
}

FpCache *
fp_cache_ref (FpCache *cache)
{
  g_return_val_if_fail (cache != NULL, NULL);
  return g_atomic_rc_box_acquire (cache);
}

void
fp_cache_unref (FpCache *cache)
{
  if (cache == NULL)
    return;
  g_atomic_rc_box_release_full (cache, cache_clear);
}

static void
remove_entry (FpCache *cache, Entry *e)
{
  g_queue_delete_link (&cache->lru, e->lru_link);
  e->lru_link = NULL;
  cache->n_bytes -= e->bytes;
  g_hash_table_remove (cache->entries, e->uri); /* frees @e */
}

static void
touch (FpCache *cache, Entry *e)
{
  g_queue_unlink (&cache->lru, e->lru_link);
  g_queue_push_head_link (&cache->lru, e->lru_link);
}

/* Evicts least-recently-used folders until within budget, never evicting @keep. */
static void
evict (FpCache *cache, Entry *keep)
{
  while ((g_hash_table_size (cache->entries) > cache->max_folders || cache->n_bytes > cache->max_bytes) &&
         cache->lru.length > 1)
    {
      Entry *victim = g_queue_peek_tail (&cache->lru);
      if (victim == keep)
        break;
      remove_entry (cache, victim);
    }
}

/* < 0 if a is older than b. */
static int
mtime_cmp (guint64 a_sec, guint32 a_usec, guint64 b_sec, guint32 b_usec)
{
  if (a_sec != b_sec)
    return a_sec < b_sec ? -1 : 1;
  if (a_usec != b_usec)
    return a_usec < b_usec ? -1 : 1;
  return 0;
}

/* Stores @texture (ref taken; may be NULL) for (@frame, @px) in @e, keeping byte counts right. */
static void
entry_store_frame (FpCache *cache, Entry *e, guint frame, guint px, GdkTexture *texture)
{
  gint64 key = frame_key (frame, px);
  gpointer old = NULL;
  if (g_hash_table_lookup_extended (e->frames, &key, NULL, &old))
    {
      e->bytes -= texture_bytes (old);
      cache->n_bytes -= texture_bytes (old);
    }
  gint64 *owned_key = g_new (gint64, 1);
  *owned_key = key;
  g_hash_table_replace (e->frames, owned_key, texture != NULL ? g_object_ref (texture) : NULL);
  e->bytes += texture_bytes (texture);
  cache->n_bytes += texture_bytes (texture);
}

gboolean
fp_cache_peek_frame (FpCache     *cache,
                     const char  *uri,
                     guint        frame,
                     guint        frame_px,
                     GdkTexture **texture,
                     guint       *n_frames_hint)
{
  g_return_val_if_fail (cache != NULL, FALSE);
  g_return_val_if_fail (uri != NULL, FALSE);
  g_return_val_if_fail (texture != NULL && *texture == NULL, FALSE);

  Entry *e = g_hash_table_lookup (cache->entries, uri);
  if (e == NULL)
    return FALSE;
  gint64 key = frame_key (frame, frame_px);
  gpointer value = NULL;
  if (!g_hash_table_lookup_extended (e->frames, &key, NULL, &value))
    return FALSE;

  *texture = value != NULL ? g_object_ref (value) : NULL;
  if (n_frames_hint != NULL)
    *n_frames_hint = e->n_frames_hint;
  touch (cache, e);
  return TRUE;
}

GdkTexture *
fp_cache_peek_frame_any_size (FpCache    *cache,
                              const char *uri,
                              guint       frame)
{
  g_return_val_if_fail (cache != NULL, NULL);
  g_return_val_if_fail (uri != NULL, NULL);
  Entry *e = g_hash_table_lookup (cache->entries, uri);
  if (e == NULL)
    return NULL;
  GdkTexture *best = NULL; /* borrowed from e->frames */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, e->frames);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      guint64 k = (guint64) *(gint64 *) key;
      if ((guint) (k >> 32) != frame || value == NULL)
        continue;
      if (best == NULL || gdk_texture_get_width (value) > gdk_texture_get_width (best))
        best = value;
    }
  return best != NULL ? g_object_ref (best) : NULL;
}

gboolean
fp_cache_invalidate (FpCache    *cache,
                     const char *uri)
{
  g_return_val_if_fail (cache != NULL, FALSE);
  g_return_val_if_fail (uri != NULL, FALSE);
  Entry *e = g_hash_table_lookup (cache->entries, uri);
  if (e == NULL)
    return FALSE;
  remove_entry (cache, e);
  return TRUE;
}

guint
fp_cache_get_n_folders (FpCache *cache)
{
  g_return_val_if_fail (cache != NULL, 0);
  return g_hash_table_size (cache->entries);
}

gsize
fp_cache_get_n_bytes (FpCache *cache)
{
  g_return_val_if_fail (cache != NULL, 0);
  return cache->n_bytes;
}

/* ================================================================================================ */
/* Disk cache (worker threads only; touches only files under disk_dir)                              */

/* New string: the per-folder cache directory. */
static char *
disk_folder_dir (const char *disk_dir, const char *uri)
{
  g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA1, uri, -1);
  return g_build_filename (disk_dir, CACHE_DISK_VERSION, hash, NULL);
}

/* New string: "<sec>.<usec>" — the mtime part every file name starts with. */
static char *
mtime_prefix (guint64 sec, guint32 usec)
{
  return g_strdup_printf ("%" G_GUINT64_FORMAT ".%06u", sec, usec);
}

/* Parses a decimal number covering [s, *end) strictly. */
static gboolean
parse_uint (const char *s, const char **end, guint *out)
{
  if (!g_ascii_isdigit (*s))
    return FALSE;
  char *e = NULL;
  guint64 v = g_ascii_strtoull (s, &e, 10);
  if (e == s || v > G_MAXUINT)
    return FALSE;
  *out = (guint) v;
  *end = e;
  return TRUE;
}

typedef enum
{
  DISK_MISS,
  DISK_NONE,  /* Marker: the folder has nothing to preview at this mtime. */
  DISK_FRAME, /* *path_out set. */
} DiskHit;

/* Looks for frame 0 of @uri at this mtime and size. On DISK_FRAME, *path_out is a new string. */
static DiskHit
disk_lookup (const char *disk_dir, const char *uri, guint64 sec, guint32 usec, guint px,
             char **path_out, guint *hint_out)
{
  g_autofree char *dir = disk_folder_dir (disk_dir, uri);
  g_autoptr (GDir) gdir = g_dir_open (dir, 0, NULL);
  if (gdir == NULL)
    return DISK_MISS;

  g_autofree char *prefix = mtime_prefix (sec, usec);
  gsize prefix_len = strlen (prefix);
  const char *name;
  while ((name = g_dir_read_name (gdir)) != NULL) /* borrowed from @gdir */
    {
      if (strncmp (name, prefix, prefix_len) != 0 || name[prefix_len] != '-')
        continue;
      const char *rest = name + prefix_len + 1;
      if (strcmp (rest, "none") == 0)
        {
          *hint_out = 0;
          return DISK_NONE;
        }
      guint file_px = 0, hint = 0;
      const char *p = rest;
      if (!parse_uint (p, &p, &file_px) || *p != '-')
        continue;
      if (!parse_uint (p + 1, &p, &hint) || strcmp (p, ".png") != 0)
        continue;
      if (file_px != px)
        continue;
      *path_out = g_build_filename (dir, name, NULL);
      *hint_out = hint;
      return DISK_FRAME;
    }
  return DISK_MISS;
}

/* Writes frame 0 (or the "none" marker if @texture is NULL) and deletes files for other mtimes.
 * Failures only cost a future cache miss, so they're logged and ignored. */
static void
disk_store (const char *disk_dir, const char *uri, guint64 sec, guint32 usec, guint px, guint hint,
            GdkTexture *texture)
{
  g_autofree char *dir = disk_folder_dir (disk_dir, uri);
  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      g_debug ("disk cache: cannot create %s", dir);
      return;
    }

  g_autofree char *prefix = mtime_prefix (sec, usec);
  g_autofree char *name = texture != NULL ? g_strdup_printf ("%s-%u-%u.png", prefix, px, hint)
                                          : g_strdup_printf ("%s-none", prefix);
  g_autofree char *path = g_build_filename (dir, name, NULL);
  g_autoptr (GBytes) png = texture != NULL ? gdk_texture_save_to_png_bytes (texture) : g_bytes_new (NULL, 0);
  gsize len = 0;
  const char *data = g_bytes_get_data (png, &len); /* borrowed from @png */
  /* Write a temp file, then rename it into place: readers never see a half-written PNG. Deliberately
   * no fsync (GLib's CONSISTENT flag always fsyncs, which took > 30 s on an NTFS drive here): after a
   * crash a damaged file just fails to load, gets deleted and is rebuilt (job_work). The temp name
   * keeps the mtime prefix, so cleanup below leaves in-flight temp files of other workers alone, and
   * disk_lookup never matches it (it doesn't end in ".png"). */
  g_autofree char *tmp = g_strdup_printf ("%s.tmp-%08x", path, g_random_int ());
  g_autoptr (GError) error = NULL;
  if (!g_file_set_contents_full (tmp, data != NULL ? data : "", (gssize) len,
                                 G_FILE_SET_CONTENTS_NONE, 0600, &error))
    {
      g_debug ("disk cache: cannot write %s: %s", tmp, error->message);
      g_unlink (tmp);
    }
  else if (g_rename (tmp, path) != 0)
    {
      g_debug ("disk cache: cannot rename %s", tmp);
      g_unlink (tmp);
    }

  /* Remove files of older mtimes for this folder (they can never be hit again). */
  g_autoptr (GDir) gdir = g_dir_open (dir, 0, NULL);
  if (gdir == NULL)
    return;
  gsize prefix_len = strlen (prefix);
  const char *entry;
  while ((entry = g_dir_read_name (gdir)) != NULL)
    {
      if (strncmp (entry, prefix, prefix_len) == 0 && entry[prefix_len] == '-')
        continue;
      g_autofree char *stale = g_build_filename (dir, entry, NULL);
      g_unlink (stale);
    }
}

/* ================================================================================================ */
/* Request pipeline                                                                                 */

typedef struct
{
  FpCache      *cache;       /* Owned ref (reads only the immutable config fields in the worker). */
  GFile        *folder;      /* Owned. */
  char         *uri;         /* Owned. */
  guint         frame;
  guint         px;
  FpScanResult *known_scan;  /* Owned ref or NULL: the scan the memory cache already had. */
} JobArgs;

static void
job_args_free (JobArgs *a)
{
  g_clear_pointer (&a->cache, fp_cache_unref);
  g_clear_object (&a->folder);
  g_clear_pointer (&a->uri, g_free);
  g_clear_pointer (&a->known_scan, fp_scan_result_unref);
  g_free (a);
}

typedef struct
{
  FpFrame      *frame;       /* Owned. */
  FpScanResult *scan;        /* Owned ref or NULL (disk hit). */
  guint64       mtime_sec;
  guint32       mtime_usec;
} JobResult;

static void
job_result_free (JobResult *r)
{
  g_clear_pointer (&r->frame, fp_frame_free);
  g_clear_pointer (&r->scan, fp_scan_result_unref);
  g_free (r);
}

static guint
div_ceil (guint a, guint b)
{
  return (a + b - 1) / b;
}

/* Resolves enough thumbnails for @frame from @scan (rescanning once if failures exhausted the window)
 * and composites it. Returns a JobResult (caller owns) or NULL with @error set (cancelled). */
static JobResult *
build_frame (FpCache       *cache,
             GFile         *folder,
             FpScanResult  *scan_in,
             guint          frame,
             guint          px,
             GCancellable  *cancellable,
             GError       **error)
{
  g_autoptr (FpScanResult) scan = fp_scan_result_ref (scan_in);
  guint first = frame * PREVIEW_SLOTS;
  guint wanted = MIN (first + PREVIEW_SLOTS, MAX_PREVIEW_FILES);

  g_autoptr (FpResolved) resolved = NULL;
  if (first < MAX_PREVIEW_FILES)
    {
      resolved = fp_thumbs_resolve (cache->thumbs, scan, wanted, cancellable, error);
      if (resolved == NULL)
        return NULL;
      if (resolved->needs_rescan)
        {
          /* Failures are now recorded, so the scanner skips them and finds the next files. */
          FpScanResult *rescan = fp_scan_folder (folder, SCAN_WINDOW, cancellable, error);
          if (rescan == NULL)
            return NULL;
          g_clear_pointer (&scan, fp_scan_result_unref);
          scan = rescan;
          g_clear_pointer (&resolved, fp_resolved_free);
          resolved = fp_thumbs_resolve (cache->thumbs, scan, wanted, cancellable, error);
          if (resolved == NULL)
            return NULL;
        }
    }

  guint n_thumbs = resolved != NULL ? resolved->thumbs->len : 0;
  JobResult *r = g_new0 (JobResult, 1);
  r->scan = g_steal_pointer (&scan);
  r->mtime_sec = r->scan->mtime_sec;
  r->mtime_usec = r->scan->mtime_usec;
  r->frame = g_new0 (FpFrame, 1);
  r->frame->frame = frame;
  r->frame->frame_px = px;

  /* Fewer than asked for means every candidate was tried: the count is exact. Otherwise the candidate
   * count is an upper bound. */
  guint known = n_thumbs < wanted ? n_thumbs : MIN (r->scan->candidates->len, (guint) MAX_PREVIEW_FILES);
  r->frame->n_frames_hint = div_ceil (known, PREVIEW_SLOTS);

  if (n_thumbs > first)
    {
      FpCompositeItem items[PREVIEW_SLOTS] = { { NULL, 0 } }; /* paths borrowed from @resolved */
      guint n = MIN (n_thumbs - first, (guint) PREVIEW_SLOTS);
      for (guint i = 0; i < n; i++)
        {
          FpThumb *t = g_ptr_array_index (resolved->thumbs, first + i);
          g_autofree char *file_uri = g_file_get_uri (t->file);
          items[i].path = t->thumbnail_path;
          items[i].seed = g_str_hash (file_uri); /* stable tilt per file (djb2, same every session) */
        }
      r->frame->texture = fp_composite_frame (items, n, px, NULL);
    }
  return r;
}

/* FpWorkFunc: the whole pipeline for one request, in a pool thread. Returns @task exactly once. */
static void
job_work (GTask *task, gpointer task_data, GCancellable *cancellable)
{
  JobArgs *a = task_data;
  g_autoptr (GError) error = NULL;

  if (!g_file_is_native (a->folder))
    {
      g_task_return_new_error_literal (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Only local folders are previewed");
      return;
    }

  g_autoptr (GFileInfo) info = g_file_query_info (a->folder, "standard::type,time::modified,time::modified-usec",
                                                  G_FILE_QUERY_INFO_NONE, cancellable, &error);
  if (info == NULL)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }
  if (g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_STANDARD_TYPE) != G_FILE_TYPE_DIRECTORY)
    {
      g_task_return_new_error_literal (task, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY, "Not a folder");
      return;
    }
  guint64 sec = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
  guint32 usec = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);

  /* Frame 0 from disk: no scan, no thumbnail decoding (PLAN.md §6.5 Phase 4 acceptance). */
  if (a->frame == 0)
    {
      g_autofree char *path = NULL;
      guint hint = 0;
      DiskHit hit = disk_lookup (a->cache->disk_dir, a->uri, sec, usec, a->px, &path, &hint);
      g_autoptr (GdkTexture) texture = hit == DISK_FRAME ? gdk_texture_new_from_filename (path, NULL) : NULL;
      if (hit == DISK_FRAME && (texture == NULL || gdk_texture_get_width (texture) != (int) a->px ||
                                gdk_texture_get_height (texture) != (int) fp_composite_frame_height (a->px)))
        {
          g_debug ("disk cache: dropping unreadable %s", path);
          g_unlink (path);
          hit = DISK_MISS;
        }
      if (hit != DISK_MISS)
        {
          JobResult *r = g_new0 (JobResult, 1);
          r->mtime_sec = sec;
          r->mtime_usec = usec;
          r->frame = g_new0 (FpFrame, 1);
          r->frame->texture = g_steal_pointer (&texture);
          r->frame->frame_px = a->px;
          r->frame->n_frames_hint = hint;
          r->frame->from_disk = TRUE;
          g_task_return_pointer (task, r, (GDestroyNotify) job_result_free);
          return;
        }
    }

  /* Reuse the scan we already have if the folder hasn't changed since. */
  g_autoptr (FpScanResult) scan = NULL;
  if (a->known_scan != NULL && mtime_cmp (a->known_scan->mtime_sec, a->known_scan->mtime_usec, sec, usec) == 0)
    scan = fp_scan_result_ref (a->known_scan);
  else
    scan = fp_scan_folder (a->folder, SCAN_WINDOW, cancellable, &error);
  if (scan == NULL)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  JobResult *r = build_frame (a->cache, a->folder, scan, a->frame, a->px, cancellable, &error);
  if (r == NULL)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }
  if (a->frame == 0)
    disk_store (a->cache->disk_dir, a->uri, r->mtime_sec, r->mtime_usec, a->px, r->frame->n_frames_hint,
                r->frame->texture);
  g_task_return_pointer (task, r, (GDestroyNotify) job_result_free);
}

/* Data of the caller-facing task. */
typedef struct
{
  FpCache *cache; /* Owned ref. */
  char    *uri;   /* Owned. */
  guint    frame;
  guint    px;
} OuterData;

static void
outer_data_free (OuterData *d)
{
  g_clear_pointer (&d->cache, fp_cache_unref);
  g_clear_pointer (&d->uri, g_free);
  g_free (d);
}

/* Main thread: the worker finished. Update memory, then complete the caller's task.
 * @user_data is a ref on the outer task (consumed here). */
static void
on_job_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) source;
  g_autoptr (GTask) outer = user_data;
  OuterData *d = g_task_get_task_data (outer);
  g_autoptr (GError) error = NULL;

  JobResult *r = g_task_propagate_pointer (G_TASK (res), &error);
  if (r == NULL)
    {
      g_task_return_error (outer, g_steal_pointer (&error));
      return;
    }

  FpCache *cache = d->cache;
  Entry *e = g_hash_table_lookup (cache->entries, d->uri);
  int cmp = e != NULL ? mtime_cmp (r->mtime_sec, r->mtime_usec, e->mtime_sec, e->mtime_usec) : 1;
  if (cmp < 0)
    {
      /* A newer result for this folder is already cached; don't overwrite it with older data. */
    }
  else
    {
      if (e != NULL && cmp > 0)
        {
          remove_entry (cache, e); /* the folder changed */
          e = NULL;
        }
      if (e == NULL)
        {
          e = entry_new (d->uri, r->mtime_sec, r->mtime_usec);
          g_hash_table_insert (cache->entries, e->uri, e);
          g_queue_push_head (&cache->lru, e);
          e->lru_link = cache->lru.head;
        }
      if (r->scan != NULL && e->scan == NULL)
        e->scan = fp_scan_result_ref (r->scan);
      e->n_frames_hint = r->frame->n_frames_hint;
      entry_store_frame (cache, e, d->frame, d->px, r->frame->texture);
      touch (cache, e);
      evict (cache, e);
    }

  FpFrame *frame = g_steal_pointer (&r->frame);
  job_result_free (r);
  /* The outer task checks its cancellable: a cancelled caller gets CANCELLED, but the work above was
   * still cached. */
  g_task_return_pointer (outer, frame, (GDestroyNotify) fp_frame_free);
}

void
fp_cache_get_frame_async (FpCache             *cache,
                          GFile               *folder,
                          guint                frame,
                          guint                frame_px,
                          int                  priority,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data)
{
  g_return_if_fail (cache != NULL);
  g_return_if_fail (G_IS_FILE (folder));
  g_return_if_fail (frame_px > 0);

  g_autofree char *uri = g_file_get_uri (folder);

  g_autoptr (GTask) outer = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (outer, fp_cache_get_frame_async);
  OuterData *d = g_new0 (OuterData, 1);
  d->cache = fp_cache_ref (cache);
  d->uri = g_strdup (uri);
  d->frame = frame;
  d->px = frame_px;
  g_task_set_task_data (outer, d, (GDestroyNotify) outer_data_free);

  JobArgs *a = g_new0 (JobArgs, 1);
  a->cache = fp_cache_ref (cache);
  a->folder = g_object_ref (folder);
  a->uri = g_steal_pointer (&uri);
  a->frame = frame;
  a->px = frame_px;
  Entry *e = g_hash_table_lookup (cache->entries, a->uri);
  if (e != NULL && e->scan != NULL)
    a->known_scan = fp_scan_result_ref (e->scan);

  /* The inner task delivers its result even if cancelled, so finished work is still cached. */
  g_autoptr (GTask) inner = g_task_new (NULL, cancellable, on_job_done, g_object_ref (outer));
  g_task_set_check_cancellable (inner, FALSE);
  g_task_set_task_data (inner, a, (GDestroyNotify) job_args_free);
  fp_workers_push (cache->workers, inner, priority, job_work);
}

FpFrame *
fp_cache_get_frame_finish (GAsyncResult  *result,
                           GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
  g_return_val_if_fail (g_task_get_source_tag (G_TASK (result)) == fp_cache_get_frame_async, NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}
