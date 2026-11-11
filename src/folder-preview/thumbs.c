#define GNOME_DESKTOP_USE_UNSTABLE_API
#include "thumbs.h"

#include <libgnome-desktop/gnome-desktop-thumbnail.h>

#define FP_THUMB_SIZE GNOME_DESKTOP_THUMBNAIL_SIZE_LARGE

/* ------------------------------------------------------------------------------------------------ */
/* Result types                                                                                     */

void
fp_thumb_free (FpThumb *thumb)
{
  if (thumb == NULL)
    return;
  g_clear_object (&thumb->file);
  g_clear_pointer (&thumb->name, g_free);
  g_clear_pointer (&thumb->thumbnail_path, g_free);
  g_free (thumb);
}

void
fp_resolved_free (FpResolved *resolved)
{
  if (resolved == NULL)
    return;
  g_clear_pointer (&resolved->scan, fp_scan_result_unref);
  g_clear_pointer (&resolved->thumbs, g_ptr_array_unref);
  g_free (resolved);
}

/* ------------------------------------------------------------------------------------------------ */
/* Factory pool                                                                                     */

struct _FpThumbs
{
  /* Owned. Holds every factory while idle. A job pops one (exclusive use) and pushes it back, so we never
   * depend on GnomeDesktopThumbnailFactory being safe for concurrent use from several threads. */
  GAsyncQueue *factories;
  guint        n_factories;
};

FpThumbs *
fp_thumbs_new (guint n_factories)
{
  g_return_val_if_fail (n_factories > 0, NULL);
  FpThumbs *thumbs = g_new0 (FpThumbs, 1);
  thumbs->n_factories = n_factories;
  thumbs->factories = g_async_queue_new_full (g_object_unref);
  for (guint i = 0; i < n_factories; i++)
    g_async_queue_push (thumbs->factories, gnome_desktop_thumbnail_factory_new (FP_THUMB_SIZE));
  return thumbs;
}

void
fp_thumbs_free (FpThumbs *thumbs)
{
  if (thumbs == NULL)
    return;
  /* Every factory must be back in the queue, i.e. no job is running (see header). */
  g_warn_if_fail (g_async_queue_length (thumbs->factories) == (gint) thumbs->n_factories);
  g_clear_pointer (&thumbs->factories, g_async_queue_unref); /* unrefs the queued factories */
  g_free (thumbs);
}

/* ------------------------------------------------------------------------------------------------ */
/* Resolution                                                                                       */

typedef enum
{
  RESOLVE_OK,        /* *path_out set. */
  RESOLVE_SKIP,      /* No thumbnail possible (failed now or before, or no thumbnailer). */
  RESOLVE_CANCELLED, /* @error set. */
} ResolveStatus;

/* Finds or makes the thumbnail for @c. On RESOLVE_OK, *path_out receives a new string the caller owns.
 * *generated is set to TRUE if a thumbnail was created by this call. */
static ResolveStatus
resolve_one (GnomeDesktopThumbnailFactory  *factory,
             FpCandidate                   *c,
             char                         **path_out,
             gboolean                      *generated,
             GCancellable                  *cancellable,
             GError                       **error)
{
  *generated = FALSE;

  /* 1. The scanner already found a valid thumbnail (any size). */
  if (c->thumbnail_path != NULL && g_file_test (c->thumbnail_path, G_FILE_TEST_IS_REGULAR))
    {
      *path_out = g_strdup (c->thumbnail_path);
      return RESOLVE_OK;
    }

  g_autofree char *uri = g_file_get_uri (c->file);
  time_t mtime = (time_t) c->mtime;

  /* 2. Someone (e.g. Nautilus) made one since the scan. Returns a new string or NULL. */
  char *existing = gnome_desktop_thumbnail_factory_lookup (factory, uri, mtime);
  if (existing != NULL)
    {
      *path_out = existing;
      return RESOLVE_OK;
    }

  /* 3. Known failure, or no thumbnailer for this type: skip without writing anything. */
  if (gnome_desktop_thumbnail_factory_has_valid_failed_thumbnail (factory, uri, mtime) ||
      !gnome_desktop_thumbnail_factory_can_thumbnail (factory, uri, c->content_type, mtime))
    return RESOLVE_SKIP;

  /* 4. Generate (runs an external, sandboxed thumbnailer; may take a few hundred ms). */
  g_autoptr (GError) gen_error = NULL;
  g_autoptr (GdkPixbuf) pixbuf =
    gnome_desktop_thumbnail_factory_generate_thumbnail (factory, uri, c->content_type, cancellable, &gen_error);
  if (pixbuf == NULL)
    {
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        return RESOLVE_CANCELLED;
      /* Record the failure in the shared cache so nobody retries until the file changes. */
      g_debug ("no thumbnail for %s: %s", uri, gen_error ? gen_error->message : "unknown error");
      gnome_desktop_thumbnail_factory_create_failed_thumbnail (factory, uri, mtime, NULL, NULL);
      return RESOLVE_SKIP;
    }

  g_autoptr (GError) save_error = NULL;
  if (!gnome_desktop_thumbnail_factory_save_thumbnail (factory, pixbuf, uri, mtime, NULL, &save_error))
    {
      /* E.g. the cache dir is not writable. Note: gnome-desktop's save_thumbnail() itself writes a
       * failure record in this case (verified, PLAN.md §6.4), the same as for Nautilus, so the file
       * stays excluded until it changes. We don't fight that. */
      g_debug ("could not save thumbnail for %s: %s", uri, save_error->message);
      return RESOLVE_SKIP;
    }

  *path_out = gnome_desktop_thumbnail_path_for_uri (uri, FP_THUMB_SIZE); /* new string, caller owns */
  *generated = TRUE;
  return RESOLVE_OK;
}

FpResolved *
fp_thumbs_resolve (FpThumbs      *thumbs,
                   FpScanResult  *scan,
                   guint          wanted,
                   GCancellable  *cancellable,
                   GError       **error)
{
  g_return_val_if_fail (thumbs != NULL, NULL);
  g_return_val_if_fail (scan != NULL, NULL);
  g_return_val_if_fail (wanted > 0, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return NULL;

  g_autoptr (FpResolved) resolved = g_new0 (FpResolved, 1);
  resolved->scan = fp_scan_result_ref (scan);
  resolved->thumbs = g_ptr_array_new_with_free_func ((GDestroyNotify) fp_thumb_free);

  /* Exclusive use of one factory for this whole call; blocks only if more resolves run at once than
   * there are factories. Ownership of the ref moves to us until we push it back below. */
  GnomeDesktopThumbnailFactory *factory = g_async_queue_pop (thumbs->factories);

  guint i = 0;
  gboolean cancelled = FALSE;
  for (; i < scan->candidates->len && resolved->thumbs->len < wanted; i++)
    {
      FpCandidate *c = g_ptr_array_index (scan->candidates, i);
      g_autofree char *path = NULL;
      gboolean generated = FALSE;
      ResolveStatus status = resolve_one (factory, c, &path, &generated, cancellable, error);
      if (status == RESOLVE_CANCELLED)
        {
          cancelled = TRUE;
          break;
        }
      if (status == RESOLVE_SKIP)
        {
          resolved->n_failed++;
          continue;
        }
      FpThumb *t = g_new0 (FpThumb, 1);
      t->file = g_object_ref (c->file);
      t->name = g_strdup (c->name);
      t->thumbnail_path = g_steal_pointer (&path);
      t->kind = c->kind;
      g_ptr_array_add (resolved->thumbs, t);
      resolved->n_generated += generated ? 1 : 0;
    }

  g_async_queue_push (thumbs->factories, factory); /* ownership back to the pool */

  if (cancelled)
    return NULL; /* @error was set by resolve_one; @resolved is freed by g_autoptr. */

  resolved->needs_rescan = resolved->thumbs->len < wanted && i == scan->candidates->len && scan->more_available;
  return g_steal_pointer (&resolved);
}

/* ------------------------------------------------------------------------------------------------ */
/* Async                                                                                            */

typedef struct
{
  FpThumbs     *thumbs;  /* Borrowed; outlives the job (see fp_thumbs_new). */
  FpScanResult *scan;    /* Owned ref. */
  guint         wanted;
} ResolveArgs;

static void
resolve_args_free (ResolveArgs *args)
{
  g_clear_pointer (&args->scan, fp_scan_result_unref);
  g_free (args);
}

/* FpWorkFunc: runs in a pool thread, returns @task exactly once. */
static void
resolve_work (GTask *task, gpointer task_data, GCancellable *cancellable)
{
  ResolveArgs *args = task_data;
  g_autoptr (GError) error = NULL;
  FpResolved *resolved = fp_thumbs_resolve (args->thumbs, args->scan, args->wanted, cancellable, &error);
  if (resolved != NULL)
    g_task_return_pointer (task, resolved, (GDestroyNotify) fp_resolved_free); /* task owns it */
  else
    g_task_return_error (task, g_steal_pointer (&error));
}

void
fp_thumbs_resolve_async (FpThumbs            *thumbs,
                         FpWorkers           *workers,
                         FpScanResult        *scan,
                         guint                wanted,
                         int                  priority,
                         GCancellable        *cancellable,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data)
{
  g_return_if_fail (thumbs != NULL);
  g_return_if_fail (workers != NULL);
  g_return_if_fail (scan != NULL);
  g_return_if_fail (wanted > 0);

  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, fp_thumbs_resolve_async);
  ResolveArgs *args = g_new0 (ResolveArgs, 1);
  args->thumbs = thumbs;
  args->scan = fp_scan_result_ref (scan);
  args->wanted = wanted;
  g_task_set_task_data (task, args, (GDestroyNotify) resolve_args_free); /* task owns @args */
  fp_workers_push (workers, task, priority, resolve_work);
}

FpResolved *
fp_thumbs_resolve_finish (GAsyncResult  *result,
                          GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
  g_return_val_if_fail (g_task_get_source_tag (G_TASK (result)) == fp_thumbs_resolve_async, NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}
