/* Drive information: OS name, classification, size queries, usage text (PLAN.md §8, Phase 1). */
#include "drive_info.h"

#include "config.h"

void
sd_usage_free (SdUsage *usage)
{
  if (usage == NULL)
    return;
  g_free (usage->fs_type);
  g_free (usage);
}

char *
sd_os_name (void)
{
  g_autofree char *name = g_get_os_info (G_OS_INFO_KEY_PRETTY_NAME);
  if (name == NULL || *g_strstrip (name) == '\0')
    return g_strdup (SD_OS_FALLBACK_NAME);
  return g_steal_pointer (&name);
}

SdDriveKind
sd_classify (gboolean is_native,
             gboolean is_remote)
{
  return (!is_native || is_remote) ? SD_DRIVE_NETWORK : SD_DRIVE_LOCAL;
}

double
sd_fill_fraction (guint64 size,
                  guint64 free)
{
  if (size == 0)
    return -1.0;
  if (free >= size)
    return 0.0;
  return (double) (size - free) / (double) size;
}

SdFillState
sd_fill_state (double fraction)
{
  if (fraction < 0.0)
    return SD_FILL_UNKNOWN;
  if (fraction >= SD_FULL_FRACTION)
    return SD_FILL_FULL;
  if (fraction >= SD_WARN_FRACTION)
    return SD_FILL_WARNING;
  return SD_FILL_NORMAL;
}

char *
sd_usage_text (guint64 size,
               guint64 free)
{
  g_autofree char *free_s = g_format_size (MIN (free, size));
  g_autofree char *size_s = g_format_size (size);
  /* Caller owns the result. */
  return g_strdup_printf ("%s free of %s", free_s, size_s);
}

/* ------------------------------------------------------------------------------------------------------ */
/* Async size query with timeout                                                                         */
/* ------------------------------------------------------------------------------------------------------ */

#define USAGE_ATTRIBUTES G_FILE_ATTRIBUTE_FILESYSTEM_SIZE "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE "," \
                         G_FILE_ATTRIBUTE_FILESYSTEM_TYPE "," G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE

/* Task data of one query. Lives as long as the GTask; the GTask itself is owned by the pending GIO call
 * (on_query_done drops that reference), so a timed-out task stays valid until GIO gives up on its call. */
typedef struct
{
  GCancellable *inner;          /* Owned. Passed to GIO; cancelled on timeout or when @outer fires. */
  GCancellable *outer;          /* Owned ref of the caller's cancellable, or NULL. */
  gulong        outer_handler;  /* Connection on @outer; 0 if none. */
  GSource      *timeout;        /* Owned ref while the timeout is pending, else NULL. */
  gboolean      returned;       /* The task has already been completed (by the timeout). */
} QueryData;

static void
query_data_free (QueryData *d)
{
  /* Disconnect before dropping @inner: the handler uses it, and disconnect waits for a running handler. */
  if (d->outer != NULL && d->outer_handler != 0)
    g_cancellable_disconnect (d->outer, d->outer_handler);
  g_clear_object (&d->outer);
  if (d->timeout != NULL)
    {
      g_source_destroy (d->timeout);
      g_clear_pointer (&d->timeout, g_source_unref);
    }
  g_clear_object (&d->inner);
  g_free (d);
}

/* May run in any thread (whoever cancels @outer); g_cancellable_cancel is thread-safe. */
static void
on_outer_cancelled (GCancellable *outer,
                    gpointer      inner)
{
  (void) outer;
  g_cancellable_cancel (G_CANCELLABLE (inner));
}

/* Main context of the task. @task is borrowed: the pending GIO call keeps it alive. */
static gboolean
on_query_timeout (gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  QueryData *d = g_task_get_task_data (task);

  g_clear_pointer (&d->timeout, g_source_unref);
  d->returned = TRUE;
  g_cancellable_cancel (d->inner);
  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Filesystem size query timed out");
  return G_SOURCE_REMOVE;
}

static SdUsage *
usage_from_info (GFileInfo *info)
{
  /* Caller owns the result. */
  SdUsage *u = g_new0 (SdUsage, 1);
  if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE) &&
      g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE))
    {
      u->size = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
      u->free = MIN (g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE), u->size);
      u->known = u->size > 0;
    }
  u->remote = g_file_info_get_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE);
  if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE))
    u->fs_type = g_strdup (g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE));
  return u;
}

/* @user_data is the GTask reference handed to GIO in sd_query_usage_async; dropped here. */
static void
on_query_done (GObject      *source,
               GAsyncResult *result,
               gpointer      user_data)
{
  g_autoptr(GTask) task = G_TASK (user_data);
  QueryData *d = g_task_get_task_data (task);
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileInfo) info = g_file_query_filesystem_info_finish (G_FILE (source), result, &error);

  if (d->timeout != NULL)
    {
      g_source_destroy (d->timeout);
      g_clear_pointer (&d->timeout, g_source_unref);
    }
  if (d->returned)
    return; /* Already completed with G_IO_ERROR_TIMED_OUT; drop the late result. */
  d->returned = TRUE;

  if (info == NULL)
    g_task_return_error (task, g_steal_pointer (&error));
  else
    g_task_return_pointer (task, usage_from_info (info), (GDestroyNotify) sd_usage_free);
}

void
sd_query_usage_async (GFile               *root,
                      guint                timeout_ms,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
  g_return_if_fail (G_IS_FILE (root));
  g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

  /* This reference is handed to GIO and dropped in on_query_done. */
  GTask *task = g_task_new (root, cancellable, callback, user_data);
  g_task_set_source_tag (task, sd_query_usage_async);

  QueryData *d = g_new0 (QueryData, 1);
  d->inner = g_cancellable_new ();
  g_task_set_task_data (task, d, (GDestroyNotify) query_data_free);

  if (cancellable != NULL)
    {
      d->outer = g_object_ref (cancellable);
      /* Fires immediately (and returns 0) if already cancelled. @inner outlives the connection. */
      d->outer_handler = g_cancellable_connect (cancellable, G_CALLBACK (on_outer_cancelled), d->inner, NULL);
    }

  if (timeout_ms > 0)
    {
      d->timeout = g_timeout_source_new (timeout_ms);
      g_source_set_static_name (d->timeout, "[sidebar-drives] size query timeout");
      /* The source borrows @task: on_query_done destroys the source before dropping the task. */
      g_source_set_callback (d->timeout, on_query_timeout, task, NULL);
      g_source_attach (d->timeout, g_task_get_context (task));
    }

  g_file_query_filesystem_info_async (root, USAGE_ATTRIBUTES, G_PRIORITY_DEFAULT, d->inner,
                                      on_query_done, task);
}

SdUsage *
sd_query_usage_finish (GFile         *root,
                       GAsyncResult  *result,
                       GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, root), NULL);
  g_return_val_if_fail (g_task_get_source_tag (G_TASK (result)) == sd_query_usage_async, NULL);
  /* Caller owns the result. */
  return g_task_propagate_pointer (G_TASK (result), error);
}
