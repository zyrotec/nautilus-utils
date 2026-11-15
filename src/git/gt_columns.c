/* Git columns. See gt_columns.h. */
#include "gt_columns.h"

#include <string.h>

#include "gt_clone.h"
#include "gt_hook.h"
#include "gt_info.h"
#include "gt_repo.h"
#include "gt_run.h"

#define READ_TIMEOUT_MS  30000
#define LOG_MAX_COMMITS  "5000"      /* the last-commit walk stops here; older items stay empty */
#define REASK_AFTER_US   (2 * G_USEC_PER_SEC)   /* Files asking again after this: a change, or a revisit */
#define ROOT_TTL_US      (30 * G_USEC_PER_SEC)  /* "is this folder in a repository" answers kept this long */
#define DIR_IDLE_US      (120 * G_USEC_PER_SEC) /* folder answers dropped when unused this long */

struct _GtColumns
{
  GObject parent_instance;
};

typedef struct
{
  GtColumns        *provider; /* ref */
  NautilusFileInfo *file;     /* ref */
  GClosure         *done;     /* ref */
  char             *name;
  gboolean          is_dir;
  gboolean          cancelled;
} Pending;

typedef struct
{
  GWeakRef file;
  gboolean is_dir;
  char    *values; /* the four values last given, "\x1f"-joined */
} Seen;

typedef struct
{
  char         *path;     /* the folder */
  char         *root;
  char         *rel;      /* the folder relative to the root ("" = the root) */
  GtInfo       *info;     /* ref: tells us when the repository changes */
  gulong        info_id;
  gboolean      skip_info; /* the info's own first read is not a change */
  GtStatus     *status;   /* NULL until read */
  GHashTable   *last;     /* item name → GtCommit*, NULL until read */
  gint64        read_at;  /* when the answers were read */
  gint64        used_at;
  GCancellable *cancel;   /* the read running, or NULL */
  guint         parts;    /* parts of the read still running (status, log) */
  GtStatus     *new_status;
  GHashTable   *new_last;
  gboolean      again;    /* read again when this one ends */
  GPtrArray    *waiting;  /* Pending* */
  GHashTable   *seen;     /* name → Seen* */
} Dir;

typedef struct
{
  char  *root;     /* NULL: not in a repository */
  char  *git_dir;
  gint64 checked_at;
} RootEntry;

static gboolean    enabled = TRUE;
static GHashTable *live;   /* Pending* handed to Files as handles (borrowed): cancel_update checks against it */
static GHashTable *dirs;   /* folder path → Dir* */
static GHashTable *roots;  /* folder path → RootEntry* */
static guint       sweep_id;

static void column_iface_init (NautilusColumnProviderInterface *iface);
static void info_iface_init (NautilusInfoProviderInterface *iface);
static void menu_iface_init (NautilusMenuProviderInterface *iface);

/* One type for Files' public providers: the Git columns and "Clone Repository…" (PLAN.md §14.2 items 3, 5). */
G_DEFINE_DYNAMIC_TYPE_EXTENDED (GtColumns, gt_columns, G_TYPE_OBJECT, G_TYPE_FLAG_FINAL,
                                G_IMPLEMENT_INTERFACE_DYNAMIC (NAUTILUS_TYPE_COLUMN_PROVIDER, column_iface_init)
                                G_IMPLEMENT_INTERFACE_DYNAMIC (NAUTILUS_TYPE_INFO_PROVIDER, info_iface_init)
                                G_IMPLEMENT_INTERFACE_DYNAMIC (NAUTILUS_TYPE_MENU_PROVIDER, menu_iface_init))

/* ---- columns ---- */

static GList *
get_columns (NautilusColumnProvider *provider)
{
  (void) provider;
  GList *l = NULL;
  l = g_list_append (l, nautilus_column_new ("NuGit::status", GT_ATTR_STATUS, "Git Status",
                                             "The file’s state in Git: modified, added, untracked, ignored…"));
  l = g_list_append (l, nautilus_column_new ("NuGit::commit", GT_ATTR_COMMIT, "Last Commit",
                                             "The message of the last commit that changed it"));
  l = g_list_append (l, nautilus_column_new ("NuGit::hash", GT_ATTR_HASH, "Commit Hash",
                                             "The last commit that changed it"));
  l = g_list_append (l, nautilus_column_new ("NuGit::author", GT_ATTR_AUTHOR, "Commit Author",
                                             "Who made the last commit that changed it"));
  return l;
}

static void
column_iface_init (NautilusColumnProviderInterface *iface)
{
  iface->get_columns = get_columns;
}

/* ---- bookkeeping ---- */

static void
pending_free (gpointer data)
{
  Pending *p = data;
  if (live != NULL)
    g_hash_table_remove (live, p);
  g_object_unref (p->provider);
  g_object_unref (p->file);
  g_closure_unref (p->done);
  g_free (p->name);
  g_free (p);
}

static void
seen_free (gpointer data)
{
  Seen *s = data;
  g_weak_ref_clear (&s->file);
  g_free (s->values);
  g_free (s);
}

static void
dir_free (gpointer data)
{
  Dir *d = data;
  if (d->cancel != NULL)
    g_cancellable_cancel (d->cancel);
  g_clear_object (&d->cancel);
  if (d->info != NULL)
    g_clear_signal_handler (&d->info_id, d->info);
  g_clear_object (&d->info);
  g_clear_pointer (&d->status, gt_status_free);
  g_clear_pointer (&d->last, g_hash_table_unref);
  g_clear_pointer (&d->new_status, gt_status_free);
  g_clear_pointer (&d->new_last, g_hash_table_unref);
  /* Files still waiting are answered empty: Files must hear back. */
  for (guint i = 0; i < d->waiting->len; i++)
    {
      Pending *p = g_ptr_array_index (d->waiting, i);
      if (!p->cancelled)
        nautilus_info_provider_update_complete_invoke (p->done, NAUTILUS_INFO_PROVIDER (p->provider),
                                                       (NautilusOperationHandle *) p, NAUTILUS_OPERATION_COMPLETE);
    }
  g_ptr_array_unref (d->waiting);
  g_hash_table_unref (d->seen);
  g_free (d->path);
  g_free (d->root);
  g_free (d->rel);
  g_free (d);
}

static void
root_entry_free (gpointer data)
{
  RootEntry *e = data;
  g_free (e->root);
  g_free (e->git_dir);
  g_free (e);
}

/* The repository @folder is in (stat only, cached for ROOT_TTL_US); NULL if none. Borrowed. */
static RootEntry *
root_of (const char *folder)
{
  if (roots == NULL)
    roots = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, root_entry_free);
  gint64 now = g_get_monotonic_time ();
  RootEntry *e = g_hash_table_lookup (roots, folder);
  if (e != NULL && now - e->checked_at < ROOT_TTL_US)
    return e->root != NULL ? e : NULL;
  e = g_new0 (RootEntry, 1);
  e->root = gt_repo_find_root (folder, &e->git_dir);
  e->checked_at = now;
  g_hash_table_replace (roots, g_strdup (folder), e);
  return e->root != NULL ? e : NULL;
}

/* ---- answers ---- */

/* The four values for @name in @d (status, commit message, short hash, author), "\x1f"-joined. Caller frees. */
static char *
values_for (Dir *d, const char *name, gboolean is_dir)
{
  g_autofree char *path = *d->rel != '\0' ? g_strconcat (d->rel, "/", name, NULL) : g_strdup (name);
  GtState state = d->status != NULL ? gt_status_state (d->status, path, is_dir) : GT_STATE_NONE;
  GtCommit *c = d->last != NULL ? g_hash_table_lookup (d->last, name) : NULL;
  /* Untracked and ignored items have no commit of their own (a folder's could be from tracked files inside). */
  if (!is_dir && (state == GT_STATE_UNTRACKED || state == GT_STATE_IGNORED))
    c = NULL;
  g_autofree char *hash = c != NULL ? g_strndup (c->hash, 7) : g_strdup ("");
  return g_strjoin ("\x1f", gt_state_label (state), c != NULL ? c->subject : "", hash, c != NULL ? c->author : "",
                    NULL);
}

static void
fill (Dir *d, NautilusFileInfo *file, const char *name, gboolean is_dir)
{
  g_autofree char *joined = values_for (d, name, is_dir);
  g_auto (GStrv) v = g_strsplit (joined, "\x1f", 4);
  static const char *const attrs[] = { GT_ATTR_STATUS, GT_ATTR_COMMIT, GT_ATTR_HASH, GT_ATTR_AUTHOR };
  gboolean ended = FALSE;
  for (int i = 0; i < 4; i++)
    {
      ended = ended || v[i] == NULL;
      nautilus_file_info_add_string_attribute (file, attrs[i], ended ? "" : v[i]);
    }
  Seen *s = g_hash_table_lookup (d->seen, name);
  if (s == NULL)
    {
      s = g_new0 (Seen, 1);
      g_weak_ref_init (&s->file, file);
      g_hash_table_insert (d->seen, g_strdup (name), s);
    }
  else
    g_weak_ref_set (&s->file, file);
  s->is_dir = is_dir;
  g_free (s->values);
  s->values = g_steal_pointer (&joined);
}

static void start_read (Dir *d);

/* Both parts of a read are in: switch to the new answers, answer the waiting files, and ask Files to ask again
 * about files whose values changed. */
static void
read_finished (Dir *d)
{
  g_clear_object (&d->cancel);
  g_clear_pointer (&d->status, gt_status_free);
  g_clear_pointer (&d->last, g_hash_table_unref);
  d->status = g_steal_pointer (&d->new_status);
  d->last = g_steal_pointer (&d->new_last);
  if (d->status == NULL)
    d->status = gt_status_parse (NULL, 0);
  if (d->last == NULL)
    d->last = g_hash_table_new (g_str_hash, g_str_equal);
  d->read_at = g_get_monotonic_time ();
  g_autoptr (GPtrArray) waiting = g_steal_pointer (&d->waiting);
  d->waiting = g_ptr_array_new_with_free_func (pending_free);
  for (guint i = 0; i < waiting->len; i++)
    {
      Pending *p = g_ptr_array_index (waiting, i);
      if (p->cancelled)
        continue;
      fill (d, p->file, p->name, p->is_dir);
      nautilus_info_provider_update_complete_invoke (p->done, NAUTILUS_INFO_PROVIDER (p->provider),
                                                     (NautilusOperationHandle *) p, NAUTILUS_OPERATION_COMPLETE);
    }
  /* Files answered before this read whose values are different now. */
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init (&it, d->seen);
  while (g_hash_table_iter_next (&it, &key, &val))
    {
      Seen *s = val;
      g_autofree char *now = values_for (d, key, s->is_dir);
      if (g_strcmp0 (now, s->values) == 0)
        continue;
      g_autoptr (NautilusFileInfo) file = g_weak_ref_get (&s->file);
      if (file != NULL)
        nautilus_file_info_invalidate_extension_info (file); /* Files asks again; we answer from the new read */
      else
        g_hash_table_iter_remove (&it);
    }
  if (d->again)
    {
      d->again = FALSE;
      start_read (d);
    }
}

static void
on_status (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  g_autoptr (GError) error = NULL;
  g_autoptr (GtRunResult) r = gt_run_finish (res, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  Dir *d = data;
  if (r != NULL && r->exit_status == 0)
    {
      gsize len = 0;
      const char *out = g_bytes_get_data (r->out, &len);
      d->new_status = gt_status_parse (out, len);
    }
  if (--d->parts == 0)
    read_finished (d);
}

static void
on_log (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  g_autoptr (GError) error = NULL;
  g_autoptr (GtRunResult) r = gt_run_finish (res, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  Dir *d = data;
  if (r != NULL && r->exit_status == 0)
    {
      gsize len = 0;
      const char *out = g_bytes_get_data (r->out, &len);
      d->new_last = gt_log_last_commits (out, len, d->rel);
    }
  if (--d->parts == 0)
    read_finished (d);
}

static void
start_read (Dir *d)
{
  if (d->cancel != NULL)
    {
      d->again = TRUE;
      return;
    }
  d->cancel = g_cancellable_new ();
  d->parts = 2;
  const char *spec = *d->rel != '\0' ? d->rel : ".";
  /* Literal pathspecs: a folder named "*" or ":x" is just a name. Run at the root, so paths are root-relative. */
  const char *status[] = { "--literal-pathspecs", "status", "--porcelain=v2", "-z", "--untracked-files=normal",
                           "--ignored=matching", "--ignore-submodules=dirty", "--", spec, NULL };
  const char *log[] = { "--literal-pathspecs", "log", "--format=" GT_LOG_FORMAT, "--name-only", "-z",
                        "-n", LOG_MAX_COMMITS, "--", spec, NULL };
  gt_run_async (d->root, status, GT_RUN_READ, NULL, READ_TIMEOUT_MS, d->cancel, NULL, NULL, on_status, d);
  gt_run_async (d->root, log, GT_RUN_READ, NULL, READ_TIMEOUT_MS, d->cancel, NULL, NULL, on_log, d);
}

/* The repository changed (commit, checkout, stage…): read the folders shown again. */
static void
on_repo_changed (GtInfo *info, gpointer data)
{
  (void) info;
  Dir *d = data;
  if (d->skip_info)
    {
      d->skip_info = FALSE;
      return;
    }
  if (g_hash_table_size (d->seen) > 0 || d->waiting->len > 0)
    start_read (d);
}

static Dir *
get_dir (const char *folder, const RootEntry *e)
{
  if (dirs == NULL)
    dirs = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, dir_free);
  Dir *d = g_hash_table_lookup (dirs, folder);
  if (d != NULL && g_strcmp0 (d->root, e->root) != 0)
    {
      g_hash_table_remove (dirs, folder); /* the folder is in another repository now */
      d = NULL;
    }
  if (d != NULL)
    return d;
  d = g_new0 (Dir, 1);
  d->path = g_strdup (folder);
  d->root = g_strdup (e->root);
  d->rel = gt_repo_relative (e->root, folder);
  if (d->rel == NULL)
    d->rel = g_strdup ("");
  d->waiting = g_ptr_array_new_with_free_func (pending_free);
  d->seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, seen_free);
  d->info = gt_info_get (e->root, e->git_dir);
  d->skip_info = !gt_info_get_loaded (d->info);
  d->info_id = g_signal_connect (d->info, "changed", G_CALLBACK (on_repo_changed), d);
  g_hash_table_insert (dirs, d->path, d);
  return d;
}

/* Drops folders nobody asked about for a while. */
static gboolean
sweep (gpointer data)
{
  (void) data;
  gint64 now = g_get_monotonic_time ();
  GHashTableIter it;
  gpointer val;
  if (dirs != NULL)
    {
      g_hash_table_iter_init (&it, dirs);
      while (g_hash_table_iter_next (&it, NULL, &val))
        {
          Dir *d = val;
          if (d->cancel == NULL && d->waiting->len == 0 && now - d->used_at > DIR_IDLE_US)
            g_hash_table_iter_remove (&it);
        }
    }
  if (roots != NULL)
    {
      g_hash_table_iter_init (&it, roots);
      while (g_hash_table_iter_next (&it, NULL, &val))
        if (now - ((RootEntry *) val)->checked_at > ROOT_TTL_US)
          g_hash_table_iter_remove (&it);
    }
  return G_SOURCE_CONTINUE;
}

static NautilusOperationResult
update_file_info (NautilusInfoProvider *provider, NautilusFileInfo *file, GClosure *done, NautilusOperationHandle **handle)
{
  if (!enabled)
    return NAUTILUS_OPERATION_COMPLETE;
  g_autoptr (GFile) location = nautilus_file_info_get_location (file);
  if (location == NULL || !g_file_is_native (location))
    return NAUTILUS_OPERATION_COMPLETE; /* never a network share (PLAN.md §14.2 item 1) */
  g_autoptr (GFile) parent = g_file_get_parent (location);
  g_autofree char *folder = parent != NULL ? g_file_get_path (parent) : NULL;
  g_autofree char *name = g_file_get_basename (location);
  if (folder == NULL || name == NULL || g_str_equal (name, ".git"))
    return NAUTILUS_OPERATION_COMPLETE;
  RootEntry *e = root_of (folder);
  if (e == NULL)
    return NAUTILUS_OPERATION_COMPLETE;
  if (sweep_id == 0)
    sweep_id = g_timeout_add_seconds (60, sweep, NULL);
  Dir *d = get_dir (folder, e);
  gint64 now = g_get_monotonic_time ();
  d->used_at = now;
  gboolean is_dir = nautilus_file_info_is_directory (file);
  gboolean fresh = d->status != NULL && now - d->read_at < REASK_AFTER_US;
  if (fresh && d->cancel == NULL)
    {
      fill (d, file, name, is_dir);
      return NAUTILUS_OPERATION_COMPLETE;
    }
  /* Not read yet, or asked again later (a file changed, or the folder is shown again): read, then answer. */
  Pending *p = g_new0 (Pending, 1);
  p->provider = g_object_ref (GT_COLUMNS (provider));
  p->file = g_object_ref (file);
  p->done = g_closure_ref (done);
  p->name = g_strdup (name);
  p->is_dir = is_dir;
  g_ptr_array_add (d->waiting, p);
  if (live == NULL)
    live = g_hash_table_new (NULL, NULL);
  g_hash_table_add (live, p);
  *handle = (NautilusOperationHandle *) p;
  if (d->cancel == NULL)
    start_read (d);
  return NAUTILUS_OPERATION_IN_PROGRESS;
}

static void
cancel_update (NautilusInfoProvider *provider, NautilusOperationHandle *handle)
{
  (void) provider;
  /* Files won't hear back about it (it asked us not to); the read goes on for the others. A handle we didn't
   * hand out (or already answered) is ignored. */
  if (live != NULL && g_hash_table_contains (live, handle))
    ((Pending *) handle)->cancelled = TRUE;
}

static void
info_iface_init (NautilusInfoProviderInterface *iface)
{
  iface->update_file_info = update_file_info;
  iface->cancel_update = cancel_update;
}

/* ---- "Clone Repository…" in the folder's background menu ---- */

static void
on_cloned (const char *folder, const char *name, gpointer data)
{
  (void) data;
  gt_hook_reveal (folder, name); /* select the new folder where it was cloned */
}

static void
on_clone_activate (NautilusMenuItem *item, gpointer data)
{
  (void) data;
  const char *folder = g_object_get_data (G_OBJECT (item), "gt-folder");
  GApplication *app = g_application_get_default ();
  GtkWindow *window = GTK_IS_APPLICATION (app) ? gtk_application_get_active_window (GTK_APPLICATION (app)) : NULL;
  gt_clone_dialog_present (window != NULL ? GTK_WIDGET (window) : NULL, folder, on_cloned, NULL);
}

static GList *
get_background_items (NautilusMenuProvider *provider, NautilusFileInfo *folder)
{
  (void) provider;
  if (!enabled || folder == NULL || !nautilus_file_info_can_write (folder))
    return NULL;
  g_autoptr (GFile) location = nautilus_file_info_get_location (folder);
  g_autofree char *path = location != NULL && g_file_is_native (location) ? g_file_get_path (location) : NULL;
  if (path == NULL || !gt_run_have_git ())
    return NULL;
  NautilusMenuItem *item = nautilus_menu_item_new ("NuGit::clone", "Clone Repository…",
                                                   "Clone a Git repository into this folder", NULL);
  g_object_set_data_full (G_OBJECT (item), "gt-folder", g_steal_pointer (&path), g_free);
  g_signal_connect (item, "activate", G_CALLBACK (on_clone_activate), NULL);
  return g_list_append (NULL, item);
}

static void
menu_iface_init (NautilusMenuProviderInterface *iface)
{
  iface->get_background_items = get_background_items;
}

static void gt_columns_class_init (GtColumnsClass *klass) { (void) klass; }
static void gt_columns_class_finalize (GtColumnsClass *klass) { (void) klass; }
static void gt_columns_init (GtColumns *self) { (void) self; }

void
gt_columns_register (GTypeModule *module)
{
  gt_columns_register_type (module);
}

static void
drop_all (void)
{
  g_clear_handle_id (&sweep_id, g_source_remove);
  g_clear_pointer (&dirs, g_hash_table_unref);
  g_clear_pointer (&roots, g_hash_table_unref);
  if (live != NULL && g_hash_table_size (live) == 0)
    g_clear_pointer (&live, g_hash_table_unref);
}

void
gt_columns_set_enabled (gboolean on)
{
  enabled = on;
  if (!on)
    drop_all ();
}

void
gt_columns_shutdown (void)
{
  drop_all ();
}
