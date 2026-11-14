/* See fi_apply.h. */
#define _POSIX_C_SOURCE 200809L /* chmod, stat */
#include "fi_apply.h"

#include <sys/stat.h>

/* "Already checked" memory per rule set; cleared when full (folders are then just checked once more). */
#define DONE_MAX 16384

#define READ_ATTRS FI_ATTR_ICON "," FI_ATTR_ICON_NAME "," FI_ATTR_MARKER

static gint inflight_total; /* Atomic: jobs started and not finished, over all appliers. */

/* What a job needs from its applier after the applier may be gone. Atomically ref-counted: the applier
 * and each job hold one; @applier is only read and cleared on the main thread. */
typedef struct
{
  FiApplier *applier; /* Borrowed; NULL once the applier was freed. */
} Shared;

struct _FiApplier
{
  const FiBackend *backend;    /* Borrowed (see header). */
  gpointer         data;       /* Passed to the backend. */
  guint            max_inflight;
  FiRules         *rules;      /* Owned or NULL. */
  guint64          generation; /* Bumped by every rule change. */
  GQueue           queue;      /* Of Job* (owned): waiting to run. */
  GHashTable      *queued;     /* Owned: uri (owned) → generation (guint64*, owned): queued or running. */
  GHashTable      *done;       /* Owned: set of uris (owned) checked for the current generation. */
  guint            running;
  guint            n_set, n_cleared, n_errors;
  FiDoneFunc       done_func;
  gpointer         done_data;
  Shared          *shared;     /* Owned ref. */
};

typedef struct
{
  GFile           *folder;    /* Owned. */
  char            *uri;       /* Owned. */
  char            *rule_icon; /* Owned or NULL: the icon its name has under the rules of @generation. */
  guint64          generation;
  const FiBackend *backend;   /* Borrowed. */
  gpointer         data;
  Shared          *shared;    /* Owned ref. */
  /* Results, written by the worker before the task returns, read on the main thread after. */
  FiActionKind     action;
  char            *error;     /* Owned or NULL. */
} Job;

static void
job_free (Job *job)
{
  g_clear_object (&job->folder);
  g_clear_pointer (&job->uri, g_free);
  g_clear_pointer (&job->rule_icon, g_free);
  g_clear_pointer (&job->error, g_free);
  if (job->shared != NULL)
    g_atomic_rc_box_release (g_steal_pointer (&job->shared));
  g_free (job);
}

/* ---- GIO backend ------------------------------------------------------------------------------------- */

static gboolean
gio_read (GFile *folder, char **icon, char **icon_name, char **marker, gpointer data, GError **error)
{
  (void) data;
  g_autoptr (GFileInfo) info = g_file_query_info (folder, READ_ATTRS, G_FILE_QUERY_INFO_NONE, NULL, error);
  if (info == NULL)
    return FALSE;
  *icon = g_strdup (g_file_info_get_attribute_string (info, FI_ATTR_ICON));
  *icon_name = g_strdup (g_file_info_get_attribute_string (info, FI_ATTR_ICON_NAME));
  *marker = g_strdup (g_file_info_get_attribute_string (info, FI_ATTR_MARKER));
  return TRUE;
}

/* Removes metadata @key (gvfs: an INVALID-typed attribute unsets it). */
static void
unset (GFileInfo *info, const char *key)
{
  g_file_info_set_attribute (info, key, G_FILE_ATTRIBUTE_TYPE_INVALID, NULL);
}

static gboolean
gio_write (GFile *folder, FiActionKind action, const char *icon_uri, gpointer data, GError **error)
{
  (void) data;
  g_autoptr (GFileInfo) info = g_file_info_new ();
  switch (action)
    {
    case FI_ACTION_SET:
      {
        g_autofree char *marker = fi_marker_new (icon_uri);
        g_file_info_set_attribute_string (info, FI_ATTR_ICON, icon_uri);
        g_file_info_set_attribute_string (info, FI_ATTR_MARKER, marker);
        break;
      }
    case FI_ACTION_CLEAR:
      unset (info, FI_ATTR_ICON);
      unset (info, FI_ATTR_MARKER);
      break;
    case FI_ACTION_CLEAR_MARKER:
      unset (info, FI_ATTR_MARKER);
      break;
    case FI_ACTION_NONE:
    default:
      return TRUE;
    }
  return g_file_set_attributes_from_info (folder, info, G_FILE_QUERY_INFO_NONE, NULL, error);
}

/* Sets the folder's permissions to their current value: nothing changes (times and mode stay), but the
 * kernel reports an attribute change (inotify IN_ATTRIB) and Files re-reads the folder's info (PLAN.md
 * §12.7 item 3). Local folders we own only; otherwise the icon shows the next time Files lists the folder. */
static void
gio_nudge (GFile *folder, gpointer data)
{
  (void) data;
  g_autofree char *path = g_file_get_path (folder);
  struct stat st;
  if (path == NULL || stat (path, &st) != 0)
    return;
  if (chmod (path, st.st_mode & 07777) != 0)
    g_debug ("cannot refresh %s", path);
}

static const FiBackend gio_backend = { gio_read, gio_write, gio_nudge };

const FiBackend *
fi_applier_gio_backend (void)
{
  return &gio_backend;
}

/* ---- Jobs ------------------------------------------------------------------------------------------- */

/* Worker thread: read, decide, write, nudge. Results go into the job. */
static void
job_thread (GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
  (void) source_object;
  (void) cancellable;
  Job *job = task_data;
  g_autofree char *icon = NULL, *icon_name = NULL, *marker = NULL;
  g_autoptr (GError) error = NULL;
  job->action = FI_ACTION_NONE;
  if (!job->backend->read (job->folder, &icon, &icon_name, &marker, job->data, &error))
    job->error = g_strdup (error->message);
  else
    {
      FiActionKind action = fi_decide (job->rule_icon, icon, icon_name, marker);
      if (action != FI_ACTION_NONE)
        {
          if (!job->backend->write (job->folder, action, job->rule_icon, job->data, &error))
            job->error = g_strdup (error->message);
          else
            {
              job->action = action;
              if (job->backend->nudge != NULL)
                job->backend->nudge (job->folder, job->data);
            }
        }
    }
  g_task_return_boolean (task, TRUE);
}

static void pump (FiApplier *a);

/* Main thread: a job finished. */
static void
on_job_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) source;
  (void) user_data;
  Job *job = g_task_get_task_data (G_TASK (res)); /* borrowed: freed with the task */
  g_atomic_int_add (&inflight_total, -1);
  FiApplier *a = job->shared->applier;
  if (a == NULL)
    return; /* the applier is gone */
  a->running--;
  const guint64 *gen = g_hash_table_lookup (a->queued, job->uri);
  if (gen != NULL && *gen == job->generation)
    g_hash_table_remove (a->queued, job->uri);
  if (job->generation == a->generation)
    {
      if (g_hash_table_size (a->done) >= DONE_MAX)
        g_hash_table_remove_all (a->done);
      g_hash_table_add (a->done, g_strdup (job->uri));
      if (job->error != NULL)
        {
          a->n_errors++;
          g_debug ("cannot apply folder icon to %s: %s", job->uri, job->error);
        }
      else if (job->action == FI_ACTION_SET)
        a->n_set++;
      else if (job->action == FI_ACTION_CLEAR || job->action == FI_ACTION_CLEAR_MARKER)
        a->n_cleared++;
      if (job->action != FI_ACTION_NONE)
        g_debug ("folder icon %s: %s", job->action == FI_ACTION_SET ? "set" : "restored", job->uri);
      if (a->done_func != NULL)
        a->done_func (job->uri, job->action, job->error, a->done_data);
    }
  pump (a);
}

/* Starts queued jobs up to the concurrency limit. */
static void
pump (FiApplier *a)
{
  while (a->running < a->max_inflight && a->queue.length > 0)
    {
      Job *job = g_queue_pop_head (&a->queue); /* ownership moves to the task */
      a->running++;
      g_atomic_int_inc (&inflight_total);
      g_autoptr (GTask) task = g_task_new (NULL, NULL, on_job_done, NULL);
      g_task_set_source_tag (task, pump);
      g_task_set_task_data (task, job, (GDestroyNotify) job_free);
      g_task_run_in_thread (task, job_thread);
    }
}

/* ---- Public API ------------------------------------------------------------------------------------- */

/* Returns a new applier (caller frees). */
FiApplier *
fi_applier_new (const FiBackend *backend, gpointer backend_data, guint max_inflight)
{
  g_return_val_if_fail (backend != NULL && backend->read != NULL && backend->write != NULL, NULL);
  FiApplier *a = g_new0 (FiApplier, 1);
  a->backend = backend;
  a->data = backend_data;
  a->max_inflight = MAX (max_inflight, 1u);
  g_queue_init (&a->queue);
  a->queued = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  a->done = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  a->shared = g_atomic_rc_box_new0 (Shared);
  a->shared->applier = a;
  return a;
}

void
fi_applier_free (FiApplier *a)
{
  if (a == NULL)
    return;
  a->shared->applier = NULL; /* running jobs finish without reporting */
  g_atomic_rc_box_release (g_steal_pointer (&a->shared));
  g_queue_clear_full (&a->queue, (GDestroyNotify) job_free);
  g_clear_pointer (&a->queued, g_hash_table_unref);
  g_clear_pointer (&a->done, g_hash_table_unref);
  g_clear_pointer (&a->rules, fi_rules_free);
  g_free (a);
}

void
fi_applier_set_done_func (FiApplier *a, FiDoneFunc func, gpointer user_data)
{
  g_return_if_fail (a != NULL);
  a->done_func = func;
  a->done_data = user_data;
}

void
fi_applier_set_rules (FiApplier *a, FiRules *rules)
{
  g_return_if_fail (a != NULL);
  g_clear_pointer (&a->rules, fi_rules_free);
  a->rules = rules;
  a->generation++;
  g_queue_clear_full (&a->queue, (GDestroyNotify) job_free);
  g_hash_table_remove_all (a->queued); /* running jobs of the old rules no longer count */
  g_hash_table_remove_all (a->done);
}

FiRules *
fi_applier_get_rules (FiApplier *a)
{
  g_return_val_if_fail (a != NULL, NULL);
  return a->rules;
}

gboolean
fi_applier_check (FiApplier *a, GFile *folder, const char *name)
{
  g_return_val_if_fail (a != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (folder), FALSE);
  if (a->rules == NULL || !fi_rules_is_candidate (a->rules, name))
    return FALSE;
  g_autofree char *uri = g_file_get_uri (folder);
  if (g_hash_table_contains (a->done, uri) || g_hash_table_contains (a->queued, uri))
    return FALSE;
  guint64 *gen = g_new (guint64, 1);
  *gen = a->generation;
  g_hash_table_insert (a->queued, g_strdup (uri), gen);

  Job *job = g_new0 (Job, 1); /* owned by the queue, then by its task */
  job->folder = g_object_ref (folder);
  job->uri = g_steal_pointer (&uri);
  job->rule_icon = g_strdup (fi_rules_icon_for (a->rules, name));
  job->generation = a->generation;
  job->backend = a->backend;
  job->data = a->data;
  job->shared = g_atomic_rc_box_acquire (a->shared);
  g_queue_push_tail (&a->queue, job);
  pump (a);
  return TRUE;
}

guint
fi_applier_get_n_pending (FiApplier *a)
{
  g_return_val_if_fail (a != NULL, 0);
  return a->queue.length + a->running;
}

guint
fi_applier_get_n_inflight (void)
{
  return (guint) g_atomic_int_get (&inflight_total);
}

guint
fi_applier_get_n_set (FiApplier *a)
{
  g_return_val_if_fail (a != NULL, 0);
  return a->n_set;
}

guint
fi_applier_get_n_cleared (FiApplier *a)
{
  g_return_val_if_fail (a != NULL, 0);
  return a->n_cleared;
}

guint
fi_applier_get_n_errors (FiApplier *a)
{
  g_return_val_if_fail (a != NULL, 0);
  return a->n_errors;
}
