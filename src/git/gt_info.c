/* Git repository info. See gt_info.h. */
#include "gt_info.h"

#include "gt_run.h"

#define READ_TIMEOUT_MS 20000
#define CHANGE_DELAY_MS 250 /* coalesce bursts of git folder changes (a commit touches many files) */

struct _GtInfo
{
  GObject       parent_instance;
  char         *root;
  char         *name;
  char         *git_dir;
  char         *common_dir;   /* where refs live (worktrees: the main repository's git folder) */
  GPtrArray    *monitors;     /* GFileMonitor* (owned) */
  guint         change_id;    /* coalescing timeout, or 0 */
  GCancellable *cancel;       /* the read running, or NULL */
  gboolean      again;        /* another read was asked for while one ran */
  gboolean      loaded;
  guint         n_reads;
  GtStatus     *status;
  GtCommit     *last;
  char         *error;
};

enum { CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];
static GHashTable *infos; /* root → GtInfo* (borrowed; removed in finalize) */

G_DEFINE_FINAL_TYPE (GtInfo, gt_info, G_TYPE_OBJECT)

static void read_now (GtInfo *self);

/* ---- reading ---- */

typedef struct
{
  GtInfo *self;     /* ref */
  GtStatus *status; /* owned until handed over */
} Read;

static void
read_free (Read *r)
{
  g_clear_object (&r->self);
  g_clear_pointer (&r->status, gt_status_free);
  g_free (r);
}

static void
read_done (GtInfo *self, GtStatus *status, GtCommit *last, const char *error)
{
  g_clear_object (&self->cancel);
  g_clear_pointer (&self->error, g_free);
  self->error = g_strdup (error);
  if (status != NULL)
    {
      g_clear_pointer (&self->status, gt_status_free);
      self->status = status;
      g_clear_pointer (&self->last, gt_commit_unref);
      self->last = last;
    }
  else if (last != NULL)
    gt_commit_unref (last);
  self->loaded = TRUE;
  self->n_reads++;
  g_signal_emit (self, signals[CHANGED], 0);
  if (self->again)
    {
      self->again = FALSE;
      read_now (self);
    }
}

static void
on_log (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  Read *r = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GtRunResult) result = gt_run_finish (res, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      read_free (r);
      return;
    }
  GtCommit *last = NULL;
  if (result != NULL && result->exit_status == 0)
    {
      gsize len = 0;
      const char *d = g_bytes_get_data (result->out, &len);
      last = gt_log_first (d, len);
    }
  /* `git log` fails before the first commit: not an error. */
  read_done (r->self, g_steal_pointer (&r->status), last, NULL);
  read_free (r);
}

static void
on_status (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  Read *r = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GtRunResult) result = gt_run_finish (res, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      read_free (r);
      return;
    }
  if (result == NULL || result->exit_status != 0)
    {
      const char *why = result != NULL && result->error_text[0] != '\0' ? result->error_text
                        : error != NULL ? error->message : "git status failed";
      read_done (r->self, NULL, NULL, why);
      read_free (r);
      return;
    }
  gsize len = 0;
  const char *d = g_bytes_get_data (result->out, &len);
  r->status = gt_status_parse (d, len);
  if (r->status->oid == NULL)
    {
      read_done (r->self, g_steal_pointer (&r->status), NULL, NULL);
      read_free (r);
      return;
    }
  const char *args[] = { "log", "-1", "--format=" GT_LOG_FORMAT, NULL };
  gt_run_async (r->self->root, args, GT_RUN_READ, NULL, READ_TIMEOUT_MS, r->self->cancel, NULL, NULL, on_log, r);
}

static void
read_now (GtInfo *self)
{
  if (self->cancel != NULL)
    {
      self->again = TRUE;
      return;
    }
  self->cancel = g_cancellable_new ();
  Read *r = g_new0 (Read, 1);
  r->self = g_object_ref (self);
  /* No untracked scan: the bar needs the branch and the counts, not every file. */
  const char *args[] = { "status", "--porcelain=v2", "-z", "--branch", "--untracked-files=no",
                         "--ignore-submodules=all", NULL };
  gt_run_async (self->root, args, GT_RUN_READ, NULL, READ_TIMEOUT_MS, self->cancel, NULL, NULL, on_status, r);
}

void
gt_info_refresh (GtInfo *self)
{
  g_return_if_fail (GT_IS_INFO (self));
  read_now (self);
}

/* ---- watching the git folder ---- */

static gboolean
on_change_timeout (gpointer data)
{
  GtInfo *self = data;
  self->change_id = 0;
  read_now (self);
  return G_SOURCE_REMOVE;
}

static void
on_changed (GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, gpointer data)
{
  (void) monitor, (void) other;
  GtInfo *self = data;
  if (event == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED)
    return;
  g_autofree char *name = g_file_get_basename (file);
  /* Lock files come and go around every change; the change itself follows. */
  if (name != NULL && g_str_has_suffix (name, ".lock"))
    return;
  if (self->change_id == 0)
    self->change_id = g_timeout_add (CHANGE_DELAY_MS, on_change_timeout, self);
}

static void
watch (GtInfo *self, const char *dir)
{
  g_autoptr (GFile) f = g_file_new_for_path (dir);
  g_autoptr (GFileMonitor) m = g_file_monitor_directory (f, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
  if (m == NULL)
    return;
  g_signal_connect_object (m, "changed", G_CALLBACK (on_changed), self, 0);
  g_ptr_array_add (self->monitors, g_steal_pointer (&m));
}

/* refs/heads/a/b, refs/remotes/origin/…: every folder below (branch names may contain '/'), at most @depth deep. */
static void
watch_tree (GtInfo *self, const char *dir, int depth)
{
  if (!g_file_test (dir, G_FILE_TEST_IS_DIR))
    return;
  watch (self, dir);
  if (depth <= 0)
    return;
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  for (const char *n; d != NULL && (n = g_dir_read_name (d)) != NULL;)
    {
      g_autofree char *sub = g_build_filename (dir, n, NULL);
      if (g_file_test (sub, G_FILE_TEST_IS_DIR))
        watch_tree (self, sub, depth - 1);
    }
}

static char *
common_dir (const char *git_dir)
{
  g_autofree char *file = g_build_filename (git_dir, "commondir", NULL);
  g_autofree char *text = NULL;
  if (!g_file_get_contents (file, &text, NULL, NULL))
    return g_strdup (git_dir);
  g_strstrip (text);
  g_autofree char *abs = g_path_is_absolute (text) ? g_strdup (text) : g_build_filename (git_dir, text, NULL);
  return g_canonicalize_filename (abs, NULL);
}

/* ---- object ---- */

static void
gt_info_finalize (GObject *object)
{
  GtInfo *self = GT_INFO (object);
  if (infos != NULL)
    g_hash_table_remove (infos, self->root);
  g_clear_handle_id (&self->change_id, g_source_remove);
  if (self->cancel != NULL)
    g_cancellable_cancel (self->cancel);
  g_clear_object (&self->cancel);
  for (guint i = 0; i < self->monitors->len; i++)
    g_file_monitor_cancel (g_ptr_array_index (self->monitors, i));
  g_ptr_array_unref (self->monitors);
  g_clear_pointer (&self->status, gt_status_free);
  g_clear_pointer (&self->last, gt_commit_unref);
  g_free (self->error);
  g_free (self->root);
  g_free (self->name);
  g_free (self->git_dir);
  g_free (self->common_dir);
  G_OBJECT_CLASS (gt_info_parent_class)->finalize (object);
}

static void
gt_info_class_init (GtInfoClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = gt_info_finalize;
  signals[CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
}

static void
gt_info_init (GtInfo *self)
{
  self->monitors = g_ptr_array_new_with_free_func (g_object_unref);
}

GtInfo *
gt_info_lookup (const char *root)
{
  GtInfo *self = infos != NULL && root != NULL ? g_hash_table_lookup (infos, root) : NULL;
  return self != NULL ? g_object_ref (self) : NULL;
}

GtInfo *
gt_info_get (const char *root, const char *git_dir)
{
  g_return_val_if_fail (root != NULL && git_dir != NULL, NULL);
  GtInfo *self = gt_info_lookup (root);
  if (self != NULL)
    return self;
  if (infos == NULL)
    infos = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self = g_object_new (GT_TYPE_INFO, NULL);
  self->root = g_strdup (root);
  self->name = g_path_get_basename (root);
  self->git_dir = g_strdup (git_dir);
  self->common_dir = common_dir (git_dir);
  g_hash_table_insert (infos, g_strdup (root), self);
  watch (self, self->git_dir); /* HEAD, index, FETCH_HEAD, packed-refs (worktrees: their own HEAD and index) */
  if (g_strcmp0 (self->common_dir, self->git_dir) != 0)
    watch (self, self->common_dir);
  g_autofree char *heads = g_build_filename (self->common_dir, "refs", "heads", NULL);
  g_autofree char *remotes = g_build_filename (self->common_dir, "refs", "remotes", NULL);
  watch_tree (self, heads, 3);
  watch_tree (self, remotes, 3);
  read_now (self);
  return self;
}

const char *gt_info_get_root (GtInfo *self) { g_return_val_if_fail (GT_IS_INFO (self), NULL); return self->root; }
const char *gt_info_get_name (GtInfo *self) { g_return_val_if_fail (GT_IS_INFO (self), NULL); return self->name; }
gboolean gt_info_get_loaded (GtInfo *self) { g_return_val_if_fail (GT_IS_INFO (self), FALSE); return self->loaded; }
guint gt_info_get_n_reads (GtInfo *self) { g_return_val_if_fail (GT_IS_INFO (self), 0); return self->n_reads; }
const char *gt_info_get_error (GtInfo *self) { g_return_val_if_fail (GT_IS_INFO (self), NULL); return self->error; }
GtCommit *gt_info_get_last (GtInfo *self) { g_return_val_if_fail (GT_IS_INFO (self), NULL); return self->last; }

const char *
gt_info_get_branch (GtInfo *self)
{
  g_return_val_if_fail (GT_IS_INFO (self), NULL);
  return self->status != NULL ? self->status->head : NULL;
}

const char *
gt_info_get_oid (GtInfo *self)
{
  g_return_val_if_fail (GT_IS_INFO (self), NULL);
  return self->status != NULL ? self->status->oid : NULL;
}

const char *
gt_info_get_upstream (GtInfo *self)
{
  g_return_val_if_fail (GT_IS_INFO (self), NULL);
  return self->status != NULL ? self->status->upstream : NULL;
}

gboolean
gt_info_get_counts (GtInfo *self, int *ahead, int *behind)
{
  g_return_val_if_fail (GT_IS_INFO (self), FALSE);
  gboolean ok = self->status != NULL && self->status->has_ab;
  if (ahead != NULL)
    *ahead = ok ? self->status->ahead : 0;
  if (behind != NULL)
    *behind = ok ? self->status->behind : 0;
  return ok;
}
