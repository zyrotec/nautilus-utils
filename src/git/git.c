/* Feature 7 — Git. Phase 0 spike (PLAN.md §14.4): checks Files' column / info / menu providers with Git values,
 * where a status bar can go (widget tree around the slot) and AdwToggleGroup. Inert unless NU_GT_SPIKE=1 and the
 * settings are in memory (GSETTINGS_BACKEND=memory: the isolated smoke session), so the owner's Files is never
 * touched. Replaced by the real feature in Phase 1. */
#define _POSIX_C_SOURCE 200809L /* strnlen */
#include "git.h"

#include <adwaita.h>
#include <glib-unix.h>
#include <nautilus-extension.h>
#include <signal.h>
#include <string.h>

static gboolean spike;
static GType    types[1];
static guint    signal_id;

/* ---- Provider: columns, async file info, background menu ---------------------------------------------- */

#define GT_TYPE_SPIKE (gt_spike_get_type ())
G_DECLARE_FINAL_TYPE (GtSpike, gt_spike, GT, SPIKE, GObject)

struct _GtSpike
{
  GObject parent_instance;
};

static void column_iface_init (NautilusColumnProviderInterface *iface);
static void info_iface_init (NautilusInfoProviderInterface *iface);
static void menu_iface_init (NautilusMenuProviderInterface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED (GtSpike, gt_spike, G_TYPE_OBJECT, G_TYPE_FLAG_FINAL,
                                G_IMPLEMENT_INTERFACE_DYNAMIC (NAUTILUS_TYPE_COLUMN_PROVIDER, column_iface_init)
                                G_IMPLEMENT_INTERFACE_DYNAMIC (NAUTILUS_TYPE_INFO_PROVIDER, info_iface_init)
                                G_IMPLEMENT_INTERFACE_DYNAMIC (NAUTILUS_TYPE_MENU_PROVIDER, menu_iface_init))

static GList *
get_columns (NautilusColumnProvider *provider)
{
  (void) provider;
  g_message ("gt spike: get_columns called");
  GList *l = NULL;
  l = g_list_append (l, nautilus_column_new ("NuGit::status", "nu_git_status", "Git Status", "Git status of the file"));
  l = g_list_append (l, nautilus_column_new ("NuGit::commit", "nu_git_commit", "Last Commit", "Last commit message"));
  l = g_list_append (l, nautilus_column_new ("NuGit::hash", "nu_git_hash", "Commit Hash", "Last commit hash"));
  l = g_list_append (l, nautilus_column_new ("NuGit::author", "nu_git_author", "Commit Author", "Last commit author"));
  return l;
}

static void
column_iface_init (NautilusColumnProviderInterface *iface)
{
  iface->get_columns = get_columns;
}

/* Per folder: one `git status` for all its files; files asked for meanwhile wait for it. */
typedef struct
{
  NautilusInfoProvider *provider;
  NautilusFileInfo *file;
  GClosure         *done;
  gboolean          cancelled;
} Pending;

typedef struct
{
  char       *dir;      /* folder path */
  GPtrArray  *waiting;  /* Pending* (owned) */
  GHashTable *status;   /* name → code, NULL until read */
  char       *head;     /* "hash\0author\0subject" of HEAD */
} Dir;

static GHashTable *dirs; /* path → Dir* */
static guint       n_async, n_complete;

static void
pending_free (gpointer data)
{
  Pending *p = data;
  g_object_unref (p->file);
  g_object_unref (p->provider);
  g_closure_unref (p->done);
  g_free (p);
}

static const char *
status_label (const char *xy)
{
  if (xy == NULL)
    return "";
  if (xy[0] == '?')
    return "Untracked";
  if (xy[0] == '!')
    return "Ignored";
  if (strchr (xy, 'U'))
    return "Conflict";
  if (strchr (xy, 'A'))
    return "Added";
  if (strchr (xy, 'D'))
    return "Deleted";
  if (strchr (xy, 'R'))
    return "Renamed";
  if (strchr (xy, 'M'))
    return "Modified";
  return "";
}

static void
fill (Dir *d, Pending *p)
{
  g_autofree char *name = nautilus_file_info_get_name (p->file);
  const char *code = d->status != NULL ? g_hash_table_lookup (d->status, name) : NULL;
  nautilus_file_info_add_string_attribute (p->file, "nu_git_status", code != NULL ? status_label (code) : "Unchanged");
  static const char none[3] = { 0, 0, 0 }; /* empty hash, author, subject */
  const char *h = d->head != NULL ? d->head : none;
  const char *author = h + strlen (h) + 1;
  const char *subject = author + strlen (author) + 1;
  g_autofree char *short_hash = g_strndup (h, 7);
  nautilus_file_info_add_string_attribute (p->file, "nu_git_hash", d->head != NULL ? short_hash : "");
  nautilus_file_info_add_string_attribute (p->file, "nu_git_author", d->head != NULL ? author : "");
  nautilus_file_info_add_string_attribute (p->file, "nu_git_commit", d->head != NULL ? subject : "");
}

static void
on_head (GObject *src, GAsyncResult *res, gpointer data)
{
  Dir *d = data;
  g_autoptr (GBytes) out = NULL;
  g_subprocess_communicate_finish (G_SUBPROCESS (src), res, &out, NULL, NULL);
  gsize len = 0;
  const char *bytes = out != NULL ? g_bytes_get_data (out, &len) : NULL;
  if (bytes != NULL && len > 0)
    {
      d->head = g_malloc0 (len + 3);
      memcpy (d->head, bytes, len);
      for (gsize i = 0; i < len; i++)
        if (d->head[i] == '\n')
          d->head[i] = '\0';
    }
  for (guint i = 0; i < d->waiting->len; i++)
    {
      Pending *p = g_ptr_array_index (d->waiting, i);
      if (p->cancelled)
        continue;
      fill (d, p);
      n_complete++;
      nautilus_info_provider_update_complete_invoke (p->done, p->provider,
                                                     (NautilusOperationHandle *) p, NAUTILUS_OPERATION_COMPLETE);
    }
  g_ptr_array_set_size (d->waiting, 0);
  g_message ("gt spike: %s filled (async %u, complete %u)", d->dir, n_async, n_complete);
}

static void
on_status (GObject *src, GAsyncResult *res, gpointer data)
{
  Dir *d = data;
  g_autoptr (GBytes) out = NULL;
  g_autoptr (GError) error = NULL;
  g_subprocess_communicate_finish (G_SUBPROCESS (src), res, &out, NULL, &error);
  d->status = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  gsize len = 0;
  const char *s = out != NULL ? g_bytes_get_data (out, &len) : NULL;
  /* porcelain v2 -z: "1 XY ... path", "? path", "! path"; paths relative to the folder (-C + relative pathspec). */
  for (gsize i = 0; s != NULL && i < len;)
    {
      const char *rec = s + i;
      gsize rl = strnlen (rec, len - i);
      const char *path = NULL, *xy = NULL;
      if (rec[0] == '1' && rl > 4)
        xy = rec + 2, path = rec;
      else if (rec[0] == '?' || rec[0] == '!')
        xy = rec, path = rec + 2;
      if (rec[0] == '1')
        {
          int sp = 0;
          for (const char *c = rec; *c && sp < 8; c++)
            if (*c == ' ' && ++sp == 8)
              path = c + 1;
        }
      if (path != NULL && xy != NULL)
        {
          const char *slash = strchr (path, '/');
          g_autofree char *top = slash != NULL ? g_strndup (path, slash - path) : g_strdup (path);
          g_hash_table_replace (d->status, g_steal_pointer (&top), g_strndup (xy, 2));
        }
      i += rl + 1;
    }
  const char *argv[] = { "git", "-C", d->dir, "log", "-1", "--format=%H%n%an%n%s", NULL };
  GSubprocess *p = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
  if (p != NULL)
    {
      g_subprocess_communicate_async (p, NULL, NULL, on_head, d);
      g_object_unref (p);
    }
}

static NautilusOperationResult
update_file_info (NautilusInfoProvider *provider, NautilusFileInfo *file, GClosure *done, NautilusOperationHandle **handle)
{
  g_autoptr (GFile) loc = nautilus_file_info_get_location (file);
  g_autoptr (GFile) parent = loc != NULL ? g_file_get_parent (loc) : NULL;
  g_autofree char *dir = parent != NULL ? g_file_get_path (parent) : NULL;
  if (dir == NULL || !g_file_is_native (loc) || strstr (dir, "gt-") == NULL) /* spike: only the test repos */
    return NAUTILUS_OPERATION_COMPLETE;
  Dir *d = g_hash_table_lookup (dirs, dir);
  if (d == NULL)
    {
      d = g_new0 (Dir, 1);
      d->dir = g_strdup (dir);
      d->waiting = g_ptr_array_new_with_free_func (pending_free);
      g_hash_table_insert (dirs, g_strdup (dir), d);
      const char *argv[] = { "git", "-C", dir, "status", "--porcelain=v2", "-z", "--untracked-files=normal",
                             "--ignored=matching", "--", ".", NULL };
      g_autoptr (GSubprocessLauncher) l = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                                      G_SUBPROCESS_FLAGS_STDERR_SILENCE);
      g_subprocess_launcher_setenv (l, "GIT_OPTIONAL_LOCKS", "0", TRUE);
      g_autoptr (GSubprocess) p = g_subprocess_launcher_spawnv (l, argv, NULL);
      if (p != NULL)
        g_subprocess_communicate_async (p, NULL, NULL, on_status, d);
    }
  if (d->head != NULL || (d->status != NULL && d->waiting->len == 0 && d->head != NULL))
    {
      Pending tmp = { provider, file, done, FALSE };
      fill (d, &tmp);
      return NAUTILUS_OPERATION_COMPLETE;
    }
  Pending *p = g_new0 (Pending, 1);
  p->provider = g_object_ref (provider);
  p->file = g_object_ref (file);
  p->done = g_closure_ref (done);
  g_ptr_array_add (d->waiting, p);
  *handle = (NautilusOperationHandle *) p;
  n_async++;
  return NAUTILUS_OPERATION_IN_PROGRESS;
}

static void
cancel_update (NautilusInfoProvider *provider, NautilusOperationHandle *handle)
{
  (void) provider;
  ((Pending *) handle)->cancelled = TRUE;
}

static void
info_iface_init (NautilusInfoProviderInterface *iface)
{
  iface->update_file_info = update_file_info;
  iface->cancel_update = cancel_update;
}

static void
on_clone (NautilusMenuItem *item, gpointer data)
{
  (void) item, (void) data;
  g_message ("gt spike: Clone Repository… activated");
}

static GList *
get_background_items (NautilusMenuProvider *provider, NautilusFileInfo *folder)
{
  (void) provider;
  g_autofree char *uri = nautilus_file_info_get_uri (folder);
  g_message ("gt spike: get_background_items called for %s", uri);
  NautilusMenuItem *item = nautilus_menu_item_new ("NuGit::clone", "Clone Repository…", "Clone a Git repository here", NULL);
  g_signal_connect (item, "activate", G_CALLBACK (on_clone), NULL);
  return g_list_append (NULL, item);
}

static void
menu_iface_init (NautilusMenuProviderInterface *iface)
{
  iface->get_background_items = get_background_items;
}

static void gt_spike_class_init (GtSpikeClass *klass) { (void) klass; }
static void gt_spike_class_finalize (GtSpikeClass *klass) { (void) klass; }
static void gt_spike_init (GtSpike *self) { (void) self; }

/* ---- Widget tree, test bar, toggle group ------------------------------------------------------------- */

static GtkWidget *
find_type (GtkWidget *w, GType t, int depth)
{
  if (t != 0 && G_TYPE_CHECK_INSTANCE_TYPE (w, t))
    return w;
  if (depth <= 0 || GTK_IS_LIST_VIEW (w) || GTK_IS_GRID_VIEW (w) || GTK_IS_COLUMN_VIEW (w))
    return NULL;
  for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL; c = gtk_widget_get_next_sibling (c))
    {
      GtkWidget *r = find_type (c, t, depth - 1);
      if (r != NULL)
        return r;
    }
  return NULL;
}

static GtkWidget *
active_window (void)
{
  GListModel *tops = gtk_window_get_toplevels (); /* borrowed (transfer none) */
  GType wt = g_type_from_name ("NautilusWindow");
  for (guint i = 0; i < g_list_model_get_n_items (tops); i++)
    {
      g_autoptr (GtkWidget) w = g_list_model_get_item (tops, i);
      if (wt != 0 && G_TYPE_CHECK_INSTANCE_TYPE (w, wt))
        return w; /* borrowed: the toplevel list keeps it */
    }
  return NULL;
}

static void
describe (GtkWidget *w, GtkWidget *root, const char *prefix)
{
  graphene_rect_t r = GRAPHENE_RECT_INIT (0, 0, 0, 0);
  gboolean ok = gtk_widget_compute_bounds (w, root, &r);
  const char *o = GTK_IS_ORIENTABLE (w) ? (gtk_orientable_get_orientation (GTK_ORIENTABLE (w)) == GTK_ORIENTATION_VERTICAL ? " vertical" : " horizontal") : "";
  g_message ("gt tree: %s%s.%s%s vis=%d y=%.0f..%.0f x=%.0f..%.0f%s", prefix, G_OBJECT_TYPE_NAME (w), gtk_widget_get_css_name (w), o,
             gtk_widget_get_visible (w), r.origin.y, r.origin.y + r.size.height, r.origin.x, r.origin.x + r.size.width, ok ? "" : " (no bounds)");
}

static void
dump_down (GtkWidget *w, GtkWidget *root, int depth, GString *indent)
{
  describe (w, root, indent->str);
  if (depth <= 0 || GTK_IS_LIST_VIEW (w) || GTK_IS_GRID_VIEW (w) || GTK_IS_COLUMN_VIEW (w))
    return;
  g_string_append (indent, "  ");
  for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL; c = gtk_widget_get_next_sibling (c))
    dump_down (c, root, depth - 1, indent);
  g_string_truncate (indent, indent->len - 2);
}

static void
command (const char *cmd)
{
  GtkWidget *win = active_window ();
  GType st = g_type_from_name ("NautilusWindowSlot");
  GtkWidget *slot = win != NULL ? find_type (win, st, 20) : NULL;
  if (g_str_equal (cmd, "gt-tree"))
    {
      if (slot == NULL)
        {
          g_message ("gt tree: no slot");
          return;
        }
      for (GtkWidget *a = gtk_widget_get_parent (slot); a != NULL && a != win; a = gtk_widget_get_parent (a))
        describe (a, win, "ancestor ");
      g_autoptr (GString) indent = g_string_new ("");
      dump_down (slot, win, 4, indent);
      GtkWidget *tg = adw_toggle_group_new ();
      adw_toggle_group_add (ADW_TOGGLE_GROUP (tg), g_object_new (ADW_TYPE_TOGGLE, "label", "Details", "name", "details", NULL));
      adw_toggle_group_add (ADW_TOGGLE_GROUP (tg), g_object_new (ADW_TYPE_TOGGLE, "label", "Git", "name", "git", NULL));
      g_message ("gt tree: toggle group ok, %u toggles", adw_toggle_group_get_n_toggles (ADW_TOGGLE_GROUP (tg)));
      g_object_ref_sink (tg);
      g_object_unref (tg);
    }
  else if (g_str_equal (cmd, "gt-bar") && slot != NULL)
    {
      GtkWidget *bar = gtk_center_box_new ();
      gtk_widget_add_css_class (bar, "toolbar");
      gtk_center_box_set_start_widget (GTK_CENTER_BOX (bar), gtk_label_new ("  main   ↑1 ↓0  (spike bar)"));
      gtk_center_box_set_end_widget (GTK_CENTER_BOX (bar), gtk_button_new_with_label ("Open in VS Code"));
      if (GTK_IS_BOX (slot) && gtk_orientable_get_orientation (GTK_ORIENTABLE (slot)) == GTK_ORIENTATION_VERTICAL)
        {
          gtk_box_append (GTK_BOX (slot), bar);
          g_message ("gt bar: appended to the slot (vertical GtkBox)");
        }
      else
        {
          g_message ("gt bar: slot is %s, not a vertical box", G_OBJECT_TYPE_NAME (slot));
          g_object_ref_sink (bar);
          g_object_unref (bar);
        }
    }
  else if (g_str_equal (cmd, "gt-bar2") && slot != NULL)
    {
      /* Under the tab view: wrap AdwTabView in a vertical box [tab view][bar] where it is (the details pane's
       * paned start child, or the toolbar view's content). */
      GtkWidget *tv = gtk_widget_get_ancestor (slot, ADW_TYPE_TAB_VIEW);
      GtkWidget *parent = tv != NULL ? gtk_widget_get_parent (tv) : NULL;
      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      GtkWidget *bar = gtk_center_box_new ();
      gtk_widget_add_css_class (bar, "toolbar");
      gtk_center_box_set_start_widget (GTK_CENTER_BOX (bar), gtk_label_new ("  main   ↑1 ↓0  (spike bar)"));
      gtk_center_box_set_end_widget (GTK_CENTER_BOX (bar), gtk_button_new_with_label ("Open in VS Code"));
      g_object_ref (tv);
      if (GTK_IS_PANED (parent) && gtk_paned_get_start_child (GTK_PANED (parent)) == tv)
        {
          gtk_paned_set_start_child (GTK_PANED (parent), NULL);
          gtk_box_append (GTK_BOX (box), tv);
          gtk_widget_set_vexpand (tv, TRUE);
          gtk_box_append (GTK_BOX (box), bar);
          gtk_paned_set_start_child (GTK_PANED (parent), box);
          g_message ("gt bar2: under the tab view in the paned");
        }
      else
        {
          g_message ("gt bar2: tab view parent is %s", parent != NULL ? G_OBJECT_TYPE_NAME (parent) : "-");
          g_object_ref_sink (box);
          g_object_unref (box);
        }
      g_object_unref (tv);
    }
  else if (g_str_equal (cmd, "gt-cols"))
    {
      /* In-memory settings only (checked at start): Files' default visible columns plus ours. */
      g_autoptr (GSettings) s = g_settings_new ("org.gnome.nautilus.list-view");
      const char *cols[] = { "name", "size", "NuGit::status", "NuGit::commit", "NuGit::hash", "NuGit::author", NULL };
      g_settings_set_strv (s, "default-visible-columns", cols);
      g_message ("gt cols: default-visible-columns set");
    }
}

static gboolean
on_signal (gpointer data)
{
  (void) data;
  const char *dir = g_getenv ("FP_DEBUG_DIR");
  g_autofree char *path = dir != NULL ? g_build_filename (dir, "cmd", NULL) : NULL;
  g_autofree char *cmd = NULL;
  if (path != NULL && g_file_get_contents (path, &cmd, NULL, NULL) && g_str_has_prefix (g_strstrip (cmd), "gt-"))
    command (cmd);
  return G_SOURCE_CONTINUE;
}

void
gt_feature_initialize (GTypeModule *module)
{
  spike = g_strcmp0 (g_getenv ("NU_GT_SPIKE"), "1") == 0 && g_strcmp0 (g_getenv ("GSETTINGS_BACKEND"), "memory") == 0;
  if (!spike)
    return;
  gt_spike_register_type (module);
  types[0] = GT_TYPE_SPIKE;
  dirs = g_hash_table_new (g_str_hash, g_str_equal);
  signal_id = g_unix_signal_add (SIGUSR2, on_signal, NULL);
  g_message ("gt spike on");
}

void
gt_feature_shutdown (void)
{
  g_clear_handle_id (&signal_id, g_source_remove);
}

const GType *
gt_feature_list_types (int *num_types)
{
  *num_types = spike ? 1 : 0;
  return types;
}
