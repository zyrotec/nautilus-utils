/* Git status bar. See gt_bar.h. */
#include "gt_bar.h"

#include <string.h>

#include "gt_ide.h"
#include "gt_ops.h"
#include "gt_run.h"

#define READ_TIMEOUT_MS 20000

struct _GtBar
{
  GtkWidget     parent_instance;
  GtInfo       *info;          /* owned or NULL */
  gulong        info_id;
  GAppInfo     *ide;           /* owned or NULL */

  GtkWidget    *box;           /* our only child */
  GtkWidget    *branch_button; /* GtkMenuButton */
  GtkWidget    *branch_label;
  GtkWidget    *counts_button; /* GtkMenuButton */
  GtkWidget    *counts_label;
  GtkWidget    *progress_button;
  GtkWidget    *progress_spinner;
  GtkWidget    *progress_icon;
  GtkWidget    *progress_label;
  GtkWidget    *ide_button;
  GtkWidget    *ide_label;
  GtkWidget    *ide_image;

  /* Branch popover */
  GtkWidget    *branch_search;
  GtkWidget    *branch_list;   /* GtkListBox */
  GtkWidget    *branch_empty;  /* "Loading…" / git's error */
  GtkWidget    *new_entry;
  GtkWidget    *create_button;
  GCancellable *branch_cancel;

  /* Counts popover */
  GtkWidget    *commit_hash;
  GtkWidget    *commit_subject;
  GtkWidget    *commit_meta;
  GtkWidget    *upstream_label;
  GtkWidget    *push_button;
  GtkWidget    *pull_button;
  GtkWidget    *sync_button;
  char         *remote;        /* first remote ("origin"), for publishing a branch; NULL: none or not read */
  GCancellable *remote_cancel;

  /* Progress popover */
  GtkFilterListModel *my_ops;  /* owned: this repository's operations */
  GtkWidget    *ops_list;
};

G_DEFINE_FINAL_TYPE (GtBar, gt_bar, GTK_TYPE_WIDGET)

static GtkCssProvider *css;

static const char style_sheet[] =
  ".gt-bar { border-top: 1px solid var(--border-color); padding: 2px 6px; }\n"
  ".gt-bar > button, .gt-bar > menubutton > button { min-height: 26px; padding: 0 8px; }\n"
  ".gt-op-message { font-family: monospace; font-size: 0.9em; }\n"
  ".gt-branch-row { padding: 6px 10px; }\n";

static void
ensure_style (void)
{
  if (css != NULL)
    return;
  css = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (css, style_sheet);
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

void
gt_bar_unload_style (void)
{
  if (css == NULL)
    return;
  gtk_style_context_remove_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (css));
  g_clear_object (&css);
}

static const char *
root (GtBar *self)
{
  return self->info != NULL ? gt_info_get_root (self->info) : NULL;
}

static char *
relative_date (gint64 when)
{
  return gt_relative_date (when, g_get_real_time () / G_USEC_PER_SEC);
}

/* ---- refresh from the info ---- */

static void
update (GtBar *self)
{
  GtInfo *info = self->info;
  gboolean loaded = info != NULL && gt_info_get_loaded (info);
  const char *branch = info != NULL ? gt_info_get_branch (info) : NULL;
  const char *oid = info != NULL ? gt_info_get_oid (info) : NULL;
  const char *error = info != NULL ? gt_info_get_error (info) : NULL;
  g_autofree char *short_oid = oid != NULL ? g_strndup (oid, 7) : NULL;
  g_autofree char *branch_text = !loaded           ? g_strdup ("…")
                                 : error != NULL    ? g_strdup ("Git error")
                                 : branch != NULL   ? g_strdup (branch)
                                 : short_oid != NULL ? g_strdup_printf ("Detached at %s", short_oid)
                                                     : g_strdup ("No commits yet");
  gtk_label_set_text (GTK_LABEL (self->branch_label), branch_text);
  /* At least the first 14 characters stay readable when the column is narrow (the details pane open). */
  gtk_label_set_width_chars (GTK_LABEL (self->branch_label), (int) MIN (g_utf8_strlen (branch_text, -1), 14));
  gtk_widget_set_tooltip_text (self->branch_button, error != NULL ? error : "Branches");

  int ahead = 0, behind = 0;
  gboolean counts = info != NULL && gt_info_get_counts (info, &ahead, &behind);
  g_autofree char *counts_text = counts ? g_strdup_printf ("↑%d ↓%d", ahead, behind)
                                        : g_strdup (loaded && branch != NULL ? "Not published" : "");
  gtk_label_set_text (GTK_LABEL (self->counts_label), counts_text);
  gtk_widget_set_visible (self->counts_button, loaded && error == NULL && *counts_text != '\0');
  gtk_widget_set_tooltip_text (self->counts_button,
                               counts ? "Commits to push (↑) and to pull (↓) — Push, Pull, Sync" : "Push this branch");

  /* Counts popover content. */
  GtCommit *last = info != NULL ? gt_info_get_last (info) : NULL;
  g_autofree char *hash = last != NULL ? g_strndup (last->hash, 7) : NULL;
  g_autofree char *when = last != NULL ? relative_date (last->date) : NULL;
  g_autofree char *meta = last != NULL ? g_strdup_printf ("%s · %s", last->author, when) : NULL;
  gtk_label_set_text (GTK_LABEL (self->commit_hash), hash != NULL ? hash : "");
  gtk_label_set_text (GTK_LABEL (self->commit_subject), last != NULL ? last->subject : "No commits yet");
  gtk_label_set_text (GTK_LABEL (self->commit_meta), meta != NULL ? meta : "");
  const char *upstream = info != NULL ? gt_info_get_upstream (info) : NULL;
  g_autofree char *up = upstream != NULL && counts ? g_strdup_printf ("Tracking %s", upstream)
                        : upstream != NULL         ? g_strdup_printf ("%s no longer exists", upstream)
                        : self->remote != NULL      ? g_strdup_printf ("Not on %s yet: Push publishes it", self->remote)
                                                    : g_strdup ("This repository has no remote");
  gtk_label_set_text (GTK_LABEL (self->upstream_label), up);
  gboolean busy = gt_ops_count_running (root (self)) > 0;
  gtk_widget_set_sensitive (self->push_button, !busy && branch != NULL && (counts || self->remote != NULL));
  gtk_widget_set_sensitive (self->pull_button, !busy && counts);
  gtk_widget_set_sensitive (self->sync_button, !busy && (counts || self->remote != NULL));
  g_autofree char *push_text = counts && ahead > 0 ? g_strdup_printf ("Push %d", ahead) : g_strdup ("Push");
  g_autofree char *pull_text = counts && behind > 0 ? g_strdup_printf ("Pull %d", behind) : g_strdup ("Pull");
  gtk_button_set_label (GTK_BUTTON (self->push_button), push_text);
  gtk_button_set_label (GTK_BUTTON (self->pull_button), pull_text);
}

/* Progress button: spinner while something runs here, else the last result's icon; hidden with no operations. */
static void
update_progress (GtBar *self)
{
  guint n = self->my_ops != NULL ? g_list_model_get_n_items (G_LIST_MODEL (self->my_ops)) : 0;
  guint running = gt_ops_count_running (root (self));
  gtk_widget_set_visible (self->progress_button, n > 0);
  gtk_widget_set_visible (self->progress_spinner, running > 0);
  g_autoptr (GtOp) newest = n > 0 ? g_list_model_get_item (G_LIST_MODEL (self->my_ops), 0) : NULL;
  GtOpState st = newest != NULL ? gt_op_get_state (newest) : GT_OP_DONE;
  gtk_widget_set_visible (self->progress_icon, running == 0 && newest != NULL);
  gtk_image_set_from_icon_name (GTK_IMAGE (self->progress_icon),
                                st == GT_OP_FAILED ? "dialog-warning-symbolic" : "object-select-symbolic");
  if (st == GT_OP_FAILED && running == 0)
    gtk_widget_add_css_class (self->progress_icon, "error");
  else
    gtk_widget_remove_css_class (self->progress_icon, "error");
  g_autofree char *text = NULL;
  if (running > 0 && newest != NULL)
    {
      const char *phase = gt_op_get_phase (newest);
      int pc = gt_op_get_percent (newest);
      text = phase != NULL && pc >= 0 ? g_strdup_printf ("%s… %d%%", gt_op_get_title (newest), pc)
                                      : g_strdup_printf ("%s…", gt_op_get_title (newest));
    }
  else if (newest != NULL)
    text = g_strdup_printf ("%s: %s", gt_op_get_title (newest), st == GT_OP_FAILED ? "failed" : "done");
  gtk_label_set_text (GTK_LABEL (self->progress_label), text != NULL ? text : "");
  /* Words only while something runs; afterwards the icon says it (the tooltip and the popover say more). */
  gtk_widget_set_visible (self->progress_label, running > 0);
  gtk_widget_set_tooltip_text (self->progress_button, text != NULL ? text : "Git operations");
  update (self); /* buttons follow "busy" */
}

static void
on_info_changed (GtInfo *info, gpointer data)
{
  (void) info;
  update (GT_BAR (data));
}

/* ---- operations ---- */

static void
start (GtBar *self, const char *title, const char *const *args, GtRunFlags flags, const char *success)
{
  if (root (self) == NULL)
    return;
  gt_ops_start (root (self), title, args, flags, success, NULL, NULL);
  gtk_menu_button_popdown (GTK_MENU_BUTTON (self->counts_button));
  gtk_menu_button_popdown (GTK_MENU_BUTTON (self->branch_button));
}

void
gt_bar_switch_branch (GtBar *self, const char *name, gboolean remote)
{
  g_return_if_fail (GT_IS_BAR (self) && name != NULL);
  g_autofree char *title = g_strdup_printf ("Switch to %s", name);
  if (!remote)
    {
      const char *args[] = { "switch", "--", name, NULL };
      start (self, title, args, 0, "Switched");
      return;
    }
  /* A remote branch "origin/feature": `git switch feature` switches to the local branch of that name, or (git's
   * --guess) creates one tracking the remote branch; with several remotes having it git explains. */
  const char *slash = strchr (name, '/');
  const char *local = slash != NULL ? slash + 1 : name;
  const char *args[] = { "switch", "--guess", "--", local, NULL };
  start (self, title, args, 0, "Switched");
}

void
gt_bar_create_branch (GtBar *self, const char *name)
{
  g_return_if_fail (GT_IS_BAR (self));
  g_autofree char *clean = g_strstrip (g_strdup (name != NULL ? name : ""));
  if (*clean == '\0' || clean[0] == '-')
    return;
  g_autofree char *title = g_strdup_printf ("Create %s", clean);
  const char *args[] = { "switch", "-c", clean, NULL };
  start (self, title, args, 0, "Created and switched");
}

void
gt_bar_push (GtBar *self)
{
  g_return_if_fail (GT_IS_BAR (self));
  if (self->info == NULL)
    return;
  if (gt_info_get_counts (self->info, NULL, NULL) || gt_info_get_upstream (self->info) != NULL || self->remote == NULL)
    {
      const char *args[] = { "push", "--progress", NULL };
      start (self, "Push", args, GT_RUN_NETWORK, "Pushed");
      return;
    }
  /* First push of this branch: publish it on the first remote and track it. */
  const char *args[] = { "push", "--progress", "--set-upstream", self->remote, "HEAD", NULL };
  start (self, "Push", args, GT_RUN_NETWORK, "Published");
}

void
gt_bar_pull (GtBar *self)
{
  g_return_if_fail (GT_IS_BAR (self));
  /* Fast-forward only: never a merge commit or a conflict nobody asked for (git explains when it can't). */
  const char *args[] = { "pull", "--progress", "--ff-only", NULL };
  start (self, "Pull", args, GT_RUN_NETWORK, "Pulled");
}

void
gt_bar_sync (GtBar *self)
{
  g_return_if_fail (GT_IS_BAR (self));
  /* Fetch only: updates the counts, changes nothing else. */
  const char *args[] = { "fetch", "--progress", NULL };
  start (self, "Sync", args, GT_RUN_NETWORK, "Up to date with the remote");
}

gboolean
gt_bar_open_ide (GtBar *self)
{
  g_return_val_if_fail (GT_IS_BAR (self), FALSE);
  if (self->ide == NULL || root (self) == NULL)
    return FALSE;
  g_autoptr (GFile) folder = g_file_new_for_path (root (self));
  g_autoptr (GError) error = NULL;
  if (!gt_ide_open (self->ide, folder, &error))
    {
      g_warning ("could not open %s in %s: %s", root (self), g_app_info_get_name (self->ide), error->message);
      return FALSE;
    }
  return TRUE;
}

static void
on_push (GtkButton *b, gpointer d)
{
  (void) b;
  gt_bar_push (GT_BAR (d));
}

static void
on_pull (GtkButton *b, gpointer d)
{
  (void) b;
  gt_bar_pull (GT_BAR (d));
}

static void
on_sync (GtkButton *b, gpointer d)
{
  (void) b;
  gt_bar_sync (GT_BAR (d));
}

static void
on_ide (GtkButton *b, gpointer d)
{
  (void) b;
  gt_bar_open_ide (GT_BAR (d));
}

/* ---- branch popover ---- */

static void
clear_list (GtkWidget *list)
{
  GtkWidget *c;
  while ((c = gtk_widget_get_first_child (list)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (list), c);
}

static GtkWidget *
section_row (const char *title)
{
  GtkWidget *l = gtk_label_new (title);
  gtk_widget_add_css_class (l, "heading");
  gtk_widget_add_css_class (l, "dim-label");
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  gtk_widget_set_margin_top (l, 8);
  gtk_widget_set_margin_start (l, 10);
  gtk_widget_set_margin_bottom (l, 2);
  GtkWidget *row = gtk_list_box_row_new ();
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), l);
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
  gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);
  g_object_set_data (G_OBJECT (row), "gt-section", GINT_TO_POINTER (1));
  return row;
}

static GtkWidget *
branch_row (const GtBranch *b)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class (box, "gt-branch-row");
  GtkWidget *check = gtk_image_new_from_icon_name ("object-select-symbolic");
  gtk_widget_set_opacity (check, b->current ? 1.0 : 0.0);
  gtk_box_append (GTK_BOX (box), check);
  GtkWidget *names = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *name = gtk_label_new (b->name);
  gtk_label_set_xalign (GTK_LABEL (name), 0);
  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_MIDDLE);
  g_autofree char *when = relative_date (b->date);
  g_autofree char *sub = g_strdup_printf ("%s · %s", b->subject, when);
  GtkWidget *detail = gtk_label_new (sub);
  gtk_label_set_xalign (GTK_LABEL (detail), 0);
  gtk_label_set_ellipsize (GTK_LABEL (detail), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (detail, "dim-label");
  gtk_widget_add_css_class (detail, "caption");
  gtk_box_append (GTK_BOX (names), name);
  gtk_box_append (GTK_BOX (names), detail);
  gtk_widget_set_hexpand (names, TRUE);
  gtk_box_append (GTK_BOX (box), names);
  GtkWidget *row = gtk_list_box_row_new ();
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  gtk_widget_set_tooltip_text (row, b->current ? "The current branch" : b->remote ? "Switch to a new local branch tracking it" : "Switch to it");
  g_object_set_data_full (G_OBJECT (row), "gt-name", g_strdup (b->name), g_free);
  g_object_set_data (G_OBJECT (row), "gt-remote", GINT_TO_POINTER (b->remote));
  g_object_set_data (G_OBJECT (row), "gt-current", GINT_TO_POINTER (b->current));
  return row;
}

static void
on_branches (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  g_autoptr (GError) error = NULL;
  g_autoptr (GtRunResult) r = gt_run_finish (res, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  GtBar *self = data;
  g_clear_object (&self->branch_cancel);
  clear_list (self->branch_list);
  if (r == NULL || r->exit_status != 0)
    {
      gtk_label_set_text (GTK_LABEL (self->branch_empty), r != NULL ? r->error_text : error->message);
      gtk_widget_set_visible (self->branch_empty, TRUE);
      return;
    }
  gsize len = 0;
  const char *d = g_bytes_get_data (r->out, &len);
  g_autoptr (GPtrArray) branches = gt_branches_parse (d, len);
  gboolean remote_header = FALSE;
  for (guint i = 0; i < branches->len; i++)
    {
      GtBranch *b = g_ptr_array_index (branches, i);
      if (i == 0 && !b->remote)
        gtk_list_box_append (GTK_LIST_BOX (self->branch_list), section_row ("Branches"));
      if (b->remote && !remote_header)
        {
          gtk_list_box_append (GTK_LIST_BOX (self->branch_list), section_row ("Remote Branches"));
          remote_header = TRUE;
        }
      gtk_list_box_append (GTK_LIST_BOX (self->branch_list), branch_row (b));
    }
  gtk_widget_set_visible (self->branch_empty, branches->len == 0);
  gtk_label_set_text (GTK_LABEL (self->branch_empty), "No branches yet");
}

static void
load_branches (GtBar *self)
{
  if (root (self) == NULL)
    return;
  if (self->branch_cancel != NULL)
    g_cancellable_cancel (self->branch_cancel);
  g_clear_object (&self->branch_cancel);
  self->branch_cancel = g_cancellable_new ();
  gtk_label_set_text (GTK_LABEL (self->branch_empty), "Loading…");
  gtk_widget_set_visible (self->branch_empty, TRUE);
  const char *args[] = { "for-each-ref", "--format=" GT_BRANCH_FORMAT, "refs/heads", "refs/remotes", NULL };
  gt_run_async (root (self), args, GT_RUN_READ, NULL, READ_TIMEOUT_MS, self->branch_cancel, NULL, NULL, on_branches,
                self);
}

static gboolean
branch_filter (GtkListBoxRow *row, gpointer data)
{
  GtBar *self = data;
  const char *q = gtk_editable_get_text (GTK_EDITABLE (self->branch_search));
  if (q == NULL || *q == '\0')
    return TRUE;
  if (g_object_get_data (G_OBJECT (row), "gt-section") != NULL)
    return FALSE;
  const char *name = g_object_get_data (G_OBJECT (row), "gt-name");
  g_autofree char *n = name != NULL ? g_utf8_casefold (name, -1) : NULL;
  g_autofree char *qq = g_utf8_casefold (q, -1);
  return n != NULL && strstr (n, qq) != NULL;
}

static void
on_search (GtkSearchEntry *e, gpointer data)
{
  (void) e;
  gtk_list_box_invalidate_filter (GTK_LIST_BOX (GT_BAR (data)->branch_list));
}

static void
on_branch_activated (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  (void) list;
  GtBar *self = data;
  const char *name = g_object_get_data (G_OBJECT (row), "gt-name");
  if (name == NULL || g_object_get_data (G_OBJECT (row), "gt-current") != NULL)
    return;
  gt_bar_switch_branch (self, name, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "gt-remote")));
}

static void
on_new_changed (GtkEditable *e, gpointer data)
{
  GtBar *self = data;
  g_autofree char *t = g_strstrip (g_strdup (gtk_editable_get_text (e)));
  gtk_widget_set_sensitive (self->create_button, *t != '\0' && t[0] != '-' && strpbrk (t, " ~^:?*[\\") == NULL);
}

static void
on_create (GtkWidget *w, gpointer data)
{
  (void) w;
  GtBar *self = data;
  if (!gtk_widget_get_sensitive (self->create_button))
    return;
  gt_bar_create_branch (self, gtk_editable_get_text (GTK_EDITABLE (self->new_entry)));
  gtk_editable_set_text (GTK_EDITABLE (self->new_entry), "");
}

static void
on_branch_popover (GtkPopover *popover, gpointer data)
{
  (void) popover;
  GtBar *self = data;
  gtk_editable_set_text (GTK_EDITABLE (self->branch_search), "");
  load_branches (self);
  gtk_widget_grab_focus (self->branch_search);
}

/* ---- counts popover: the first remote, for publishing ---- */

static void
on_remotes (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  g_autoptr (GError) error = NULL;
  g_autoptr (GtRunResult) r = gt_run_finish (res, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  GtBar *self = data;
  g_clear_object (&self->remote_cancel);
  g_clear_pointer (&self->remote, g_free);
  if (r != NULL && r->exit_status == 0)
    {
      /* No remotes: git prints nothing, and the bytes may have no data at all (NULL). */
      gsize len = 0;
      const char *d = g_bytes_get_data (r->out, &len);
      g_autofree char *text = d != NULL && len > 0 ? g_strndup (d, len) : g_strdup ("");
      g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
      /* "origin" if there is one, else the first. */
      for (int i = 0; lines[i] != NULL; i++)
        if (g_str_equal (lines[i], "origin"))
          self->remote = g_strdup ("origin");
      if (self->remote == NULL && lines[0] != NULL && *lines[0] != '\0')
        self->remote = g_strdup (lines[0]);
    }
  update (self);
}

static void
load_remotes (GtBar *self)
{
  if (root (self) == NULL)
    return;
  if (self->remote_cancel != NULL)
    g_cancellable_cancel (self->remote_cancel);
  g_clear_object (&self->remote_cancel);
  self->remote_cancel = g_cancellable_new ();
  const char *args[] = { "remote", NULL };
  gt_run_async (root (self), args, GT_RUN_READ, NULL, READ_TIMEOUT_MS, self->remote_cancel, NULL, NULL, on_remotes, self);
}

/* ---- progress popover ---- */

static void
copy_message (GtkButton *button, gpointer data)
{
  (void) data;
  const char *text = g_object_get_data (G_OBJECT (button), "gt-text");
  gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (button)), text != NULL ? text : "");
}

static void
update_op_row (GtOp *op, GParamSpec *pspec, GtkWidget *row)
{
  (void) pspec;
  GtkWidget *state = g_object_get_data (G_OBJECT (row), "gt-state");
  GtkWidget *bar = g_object_get_data (G_OBJECT (row), "gt-bar");
  GtkWidget *message = g_object_get_data (G_OBJECT (row), "gt-message");
  GtkWidget *copy = g_object_get_data (G_OBJECT (row), "gt-copy");
  GtOpState st = gt_op_get_state (op);
  int pc = gt_op_get_percent (op);
  const char *phase = gt_op_get_phase (op);
  g_autofree char *state_text = st == GT_OP_RUNNING ? (phase != NULL ? (pc >= 0 ? g_strdup_printf ("%s %d%%", phase, pc)
                                                                                : g_strdup (phase))
                                                                     : g_strdup ("Running…"))
                                : g_strdup (st == GT_OP_DONE ? gt_op_get_message (op) : "Failed");
  gtk_label_set_text (GTK_LABEL (state), state_text);
  gtk_widget_set_visible (bar, st == GT_OP_RUNNING);
  if (st == GT_OP_RUNNING && pc >= 0)
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (bar), pc / 100.0);
  else if (st == GT_OP_RUNNING)
    gtk_progress_bar_pulse (GTK_PROGRESS_BAR (bar));
  gboolean failed = st == GT_OP_FAILED;
  gtk_label_set_text (GTK_LABEL (message), failed ? gt_op_get_message (op) : "");
  gtk_widget_set_visible (message, failed);
  gtk_widget_set_visible (copy, failed);
  g_object_set_data_full (G_OBJECT (copy), "gt-text", g_strdup (failed ? gt_op_get_message (op) : ""), g_free);
  if (failed)
    gtk_widget_add_css_class (state, "error");
  else
    gtk_widget_remove_css_class (state, "error");
}

static GtkWidget *
create_op_row (gpointer item, gpointer data)
{
  (void) data;
  GtOp *op = item;
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_start (box, 10);
  gtk_widget_set_margin_end (box, 10);
  gtk_widget_set_margin_top (box, 8);
  gtk_widget_set_margin_bottom (box, 8);
  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *title = gtk_label_new (gt_op_get_title (op));
  gtk_widget_add_css_class (title, "heading");
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  gtk_widget_set_hexpand (title, TRUE);
  GtkWidget *state = gtk_label_new (NULL);
  gtk_widget_add_css_class (state, "dim-label");
  gtk_label_set_ellipsize (GTK_LABEL (state), PANGO_ELLIPSIZE_END);
  GtkWidget *copy = gtk_button_new_from_icon_name ("edit-copy-symbolic");
  gtk_widget_add_css_class (copy, "flat");
  gtk_widget_set_tooltip_text (copy, "Copy git’s message");
  g_signal_connect (copy, "clicked", G_CALLBACK (copy_message), NULL);
  gtk_box_append (GTK_BOX (head), title);
  gtk_box_append (GTK_BOX (head), state);
  gtk_box_append (GTK_BOX (head), copy);
  GtkWidget *bar = gtk_progress_bar_new ();
  GtkWidget *message = gtk_label_new (NULL);
  gtk_widget_add_css_class (message, "gt-op-message");
  gtk_label_set_xalign (GTK_LABEL (message), 0);
  gtk_label_set_wrap (GTK_LABEL (message), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (message), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_selectable (GTK_LABEL (message), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (message), 60);
  gtk_box_append (GTK_BOX (box), head);
  gtk_box_append (GTK_BOX (box), bar);
  gtk_box_append (GTK_BOX (box), message);
  g_object_set_data (G_OBJECT (box), "gt-state", state);
  g_object_set_data (G_OBJECT (box), "gt-bar", bar);
  g_object_set_data (G_OBJECT (box), "gt-message", message);
  g_object_set_data (G_OBJECT (box), "gt-copy", copy);
  g_signal_connect_object (op, "notify", G_CALLBACK (update_op_row), box, 0);
  update_op_row (op, NULL, box);
  return box;
}

static gboolean
mine (gpointer item, gpointer data)
{
  GtBar *self = data;
  return root (self) != NULL && g_strcmp0 (gt_op_get_root (item), root (self)) == 0;
}

static void
on_ops_changed (GListModel *model, guint pos, guint removed, guint added, gpointer data)
{
  (void) model, (void) pos, (void) removed, (void) added;
  update_progress (GT_BAR (data));
}

/* Every operation's state changes reach the button (the filter model only reports additions / removals). */
static void
watch_op (GtBar *self, GtOp *op)
{
  g_signal_connect_object (op, "notify::state", G_CALLBACK (update_progress), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (op, "notify::percent", G_CALLBACK (update_progress), self, G_CONNECT_SWAPPED);
}

static void
on_any_op_notify (GListModel *all, guint pos, guint removed, guint added, gpointer data)
{
  (void) removed;
  GtBar *self = data;
  for (guint i = pos; i < pos + added; i++)
    {
      g_autoptr (GtOp) op = g_list_model_get_item (all, i);
      watch_op (self, op);
    }
}

/* ---- construction ---- */

static GtkWidget *
popover_box (void)
{
  GtkWidget *b = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start (b, 6);
  gtk_widget_set_margin_end (b, 6);
  gtk_widget_set_margin_top (b, 6);
  gtk_widget_set_margin_bottom (b, 6);
  return b;
}

static void
build_branch_popover (GtBar *self)
{
  GtkWidget *pop = gtk_popover_new ();
  GtkWidget *box = popover_box ();
  gtk_widget_set_size_request (box, 320, -1);
  self->branch_search = gtk_search_entry_new ();
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (self->branch_search), "Search branches");
  g_signal_connect (self->branch_search, "search-changed", G_CALLBACK (on_search), self);
  gtk_box_append (GTK_BOX (box), self->branch_search);
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroll), 340);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroll), TRUE);
  self->branch_list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (self->branch_list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (self->branch_list, "navigation-sidebar");
  gtk_list_box_set_filter_func (GTK_LIST_BOX (self->branch_list), branch_filter, self, NULL);
  g_signal_connect (self->branch_list, "row-activated", G_CALLBACK (on_branch_activated), self);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), self->branch_list);
  gtk_box_append (GTK_BOX (box), scroll);
  self->branch_empty = gtk_label_new ("Loading…");
  gtk_widget_add_css_class (self->branch_empty, "dim-label");
  gtk_label_set_wrap (GTK_LABEL (self->branch_empty), TRUE);
  gtk_box_append (GTK_BOX (box), self->branch_empty);
  gtk_box_append (GTK_BOX (box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  GtkWidget *create = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  self->new_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (self->new_entry), "New branch name");
  gtk_widget_set_hexpand (self->new_entry, TRUE);
  g_signal_connect (self->new_entry, "changed", G_CALLBACK (on_new_changed), self);
  g_signal_connect (self->new_entry, "activate", G_CALLBACK (on_create), self);
  self->create_button = gtk_button_new_with_label ("Create");
  gtk_widget_add_css_class (self->create_button, "suggested-action");
  gtk_widget_set_sensitive (self->create_button, FALSE);
  gtk_widget_set_tooltip_text (self->create_button, "Create a branch at the current commit and switch to it");
  g_signal_connect (self->create_button, "clicked", G_CALLBACK (on_create), self);
  gtk_box_append (GTK_BOX (create), self->new_entry);
  gtk_box_append (GTK_BOX (create), self->create_button);
  gtk_box_append (GTK_BOX (box), create);
  gtk_popover_set_child (GTK_POPOVER (pop), box);
  g_signal_connect (pop, "show", G_CALLBACK (on_branch_popover), self);
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (self->branch_button), pop);
}

static GtkWidget *
caption (const char *text)
{
  GtkWidget *l = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  gtk_label_set_wrap (GTK_LABEL (l), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (l), 40);
  return l;
}

static void
on_counts_popover (GtkPopover *popover, gpointer data)
{
  (void) popover;
  load_remotes (GT_BAR (data));
}

static void
build_counts_popover (GtBar *self)
{
  GtkWidget *pop = gtk_popover_new ();
  GtkWidget *box = popover_box ();
  gtk_widget_set_size_request (box, 300, -1);
  GtkWidget *head = gtk_label_new ("Last Commit");
  gtk_widget_add_css_class (head, "heading");
  gtk_label_set_xalign (GTK_LABEL (head), 0);
  gtk_box_append (GTK_BOX (box), head);
  self->commit_subject = caption ("");
  gtk_label_set_selectable (GTK_LABEL (self->commit_subject), TRUE);
  self->commit_hash = caption ("");
  gtk_widget_add_css_class (self->commit_hash, "monospace");
  gtk_widget_add_css_class (self->commit_hash, "dim-label");
  gtk_label_set_selectable (GTK_LABEL (self->commit_hash), TRUE);
  self->commit_meta = caption ("");
  gtk_widget_add_css_class (self->commit_meta, "dim-label");
  gtk_box_append (GTK_BOX (box), self->commit_subject);
  gtk_box_append (GTK_BOX (box), self->commit_hash);
  gtk_box_append (GTK_BOX (box), self->commit_meta);
  gtk_box_append (GTK_BOX (box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  self->upstream_label = caption ("");
  gtk_widget_add_css_class (self->upstream_label, "dim-label");
  gtk_box_append (GTK_BOX (box), self->upstream_label);
  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_set_homogeneous (GTK_BOX (buttons), TRUE);
  self->push_button = gtk_button_new_with_label ("Push");
  gtk_widget_set_tooltip_text (self->push_button, "Send your commits to the remote");
  self->pull_button = gtk_button_new_with_label ("Pull");
  gtk_widget_set_tooltip_text (self->pull_button, "Bring in the remote's commits (fast-forward only)");
  self->sync_button = gtk_button_new_with_label ("Sync");
  gtk_widget_set_tooltip_text (self->sync_button, "Check the remote for new commits (fetch); changes nothing else");
  g_signal_connect (self->push_button, "clicked", G_CALLBACK (on_push), self);
  g_signal_connect (self->pull_button, "clicked", G_CALLBACK (on_pull), self);
  g_signal_connect (self->sync_button, "clicked", G_CALLBACK (on_sync), self);
  gtk_box_append (GTK_BOX (buttons), self->push_button);
  gtk_box_append (GTK_BOX (buttons), self->pull_button);
  gtk_box_append (GTK_BOX (buttons), self->sync_button);
  gtk_box_append (GTK_BOX (box), buttons);
  gtk_popover_set_child (GTK_POPOVER (pop), box);
  g_signal_connect (pop, "show", G_CALLBACK (on_counts_popover), self);
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (self->counts_button), pop);
}

static void
build_progress_popover (GtBar *self)
{
  GtkWidget *pop = gtk_popover_new ();
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroll), 420);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroll), TRUE);
  gtk_widget_set_size_request (scroll, 380, -1);
  self->ops_list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (self->ops_list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (self->ops_list, "boxed-list-separate");
  gtk_list_box_bind_model (GTK_LIST_BOX (self->ops_list), G_LIST_MODEL (self->my_ops), (GtkListBoxCreateWidgetFunc) create_op_row,
                           NULL, NULL);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), self->ops_list);
  gtk_popover_set_child (GTK_POPOVER (pop), scroll);
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (self->progress_button), pop);
}

static GtkWidget *
menu_button (GtkWidget *child)
{
  GtkWidget *b = gtk_menu_button_new ();
  gtk_menu_button_set_child (GTK_MENU_BUTTON (b), child);
  gtk_menu_button_set_always_show_arrow (GTK_MENU_BUTTON (b), TRUE);
  gtk_widget_add_css_class (b, "flat");
  return b;
}

static void
gt_bar_init (GtBar *self)
{
  ensure_style ();
  gtk_widget_add_css_class (GTK_WIDGET (self), "gt-bar");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self), GTK_ACCESSIBLE_PROPERTY_LABEL, "Git", -1);
  self->box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_parent (self->box, GTK_WIDGET (self));

  GtkWidget *bb = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (bb), gtk_image_new_from_icon_name ("nu-git-branch-symbolic"));
  self->branch_label = gtk_label_new ("…");
  gtk_label_set_ellipsize (GTK_LABEL (self->branch_label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars (GTK_LABEL (self->branch_label), 32);
  gtk_box_append (GTK_BOX (bb), self->branch_label);
  self->branch_button = menu_button (bb);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->branch_button), GTK_ACCESSIBLE_PROPERTY_LABEL, "Branches", -1);
  gtk_box_append (GTK_BOX (self->box), self->branch_button);

  self->counts_label = gtk_label_new (NULL);
  self->counts_button = menu_button (self->counts_label);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->counts_button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "Commits to push and pull", -1);
  gtk_box_append (GTK_BOX (self->box), self->counts_button);

  GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (spacer, TRUE);
  gtk_box_append (GTK_BOX (self->box), spacer);

  GtkWidget *pb = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  self->progress_spinner = adw_spinner_new ();
  self->progress_icon = gtk_image_new ();
  self->progress_label = gtk_label_new (NULL);
  gtk_label_set_ellipsize (GTK_LABEL (self->progress_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (self->progress_label), 28);
  gtk_box_append (GTK_BOX (pb), self->progress_spinner);
  gtk_box_append (GTK_BOX (pb), self->progress_icon);
  gtk_box_append (GTK_BOX (pb), self->progress_label);
  self->progress_button = menu_button (pb);
  gtk_menu_button_set_always_show_arrow (GTK_MENU_BUTTON (self->progress_button), FALSE);
  gtk_widget_set_tooltip_text (self->progress_button, "Git operations");
  gtk_widget_set_visible (self->progress_button, FALSE);
  gtk_box_append (GTK_BOX (self->box), self->progress_button);

  GtkWidget *ib = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  self->ide_image = gtk_image_new ();
  self->ide_label = gtk_label_new (NULL);
  gtk_label_set_ellipsize (GTK_LABEL (self->ide_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_width_chars (GTK_LABEL (self->ide_label), 4);
  gtk_box_append (GTK_BOX (ib), self->ide_image);
  gtk_box_append (GTK_BOX (ib), self->ide_label);
  self->ide_button = gtk_button_new ();
  gtk_button_set_child (GTK_BUTTON (self->ide_button), ib);
  gtk_widget_add_css_class (self->ide_button, "flat");
  g_signal_connect (self->ide_button, "clicked", G_CALLBACK (on_ide), self);
  gtk_widget_set_visible (self->ide_button, FALSE);
  gtk_box_append (GTK_BOX (self->box), self->ide_button);

  GtkCustomFilter *filter = gtk_custom_filter_new (mine, self, NULL);
  GListModel *all = gt_ops_get_model ();
  self->my_ops = gtk_filter_list_model_new (g_object_ref (all), GTK_FILTER (filter));
  g_signal_connect_object (self->my_ops, "items-changed", G_CALLBACK (on_ops_changed), self, 0);
  g_signal_connect_object (all, "items-changed", G_CALLBACK (on_any_op_notify), self, 0);
  for (guint i = 0; i < g_list_model_get_n_items (all); i++)
    {
      g_autoptr (GtOp) op = g_list_model_get_item (all, i);
      watch_op (self, op);
    }

  build_branch_popover (self);
  build_counts_popover (self);
  build_progress_popover (self);
  update (self);
}

static void
gt_bar_dispose (GObject *object)
{
  GtBar *self = GT_BAR (object);
  gt_bar_set_info (self, NULL);
  if (self->branch_cancel != NULL)
    g_cancellable_cancel (self->branch_cancel);
  if (self->remote_cancel != NULL)
    g_cancellable_cancel (self->remote_cancel);
  g_clear_object (&self->branch_cancel);
  g_clear_object (&self->remote_cancel);
  g_clear_object (&self->ide);
  g_clear_pointer (&self->remote, g_free);
  g_clear_pointer (&self->box, gtk_widget_unparent);
  g_clear_object (&self->my_ops);
  G_OBJECT_CLASS (gt_bar_parent_class)->dispose (object);
}

static void
gt_bar_class_init (GtBarClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = gt_bar_dispose;
  GtkWidgetClass *wc = GTK_WIDGET_CLASS (klass);
  gtk_widget_class_set_layout_manager_type (wc, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (wc, "gt-bar");
  gtk_widget_class_set_accessible_role (wc, GTK_ACCESSIBLE_ROLE_TOOLBAR);
}

GtkWidget *
gt_bar_new (void)
{
  return g_object_new (GT_TYPE_BAR, NULL);
}

void
gt_bar_set_info (GtBar *self, GtInfo *info)
{
  g_return_if_fail (GT_IS_BAR (self));
  if (self->info == info)
    return;
  if (self->info != NULL)
    g_clear_signal_handler (&self->info_id, self->info);
  g_clear_object (&self->info);
  g_clear_pointer (&self->remote, g_free);
  if (info != NULL)
    {
      self->info = g_object_ref (info);
      self->info_id = g_signal_connect (info, "changed", G_CALLBACK (on_info_changed), self);
      load_remotes (self);
    }
  if (self->my_ops != NULL)
    gtk_filter_changed (gtk_filter_list_model_get_filter (self->my_ops), GTK_FILTER_CHANGE_DIFFERENT);
  update (self);
  update_progress (self);
}

GtInfo *
gt_bar_get_info (GtBar *self)
{
  g_return_val_if_fail (GT_IS_BAR (self), NULL);
  return self->info;
}

void
gt_bar_set_ide (GtBar *self, GAppInfo *ide)
{
  g_return_if_fail (GT_IS_BAR (self));
  g_set_object (&self->ide, ide);
  gtk_widget_set_visible (self->ide_button, ide != NULL);
  if (ide == NULL)
    return;
  g_autofree char *label = g_strdup_printf ("Open in %s", g_app_info_get_name (ide));
  gtk_label_set_text (GTK_LABEL (self->ide_label), label);
  GIcon *icon = g_app_info_get_icon (ide);
  gtk_image_set_from_gicon (GTK_IMAGE (self->ide_image), icon);
  gtk_widget_set_visible (self->ide_image, icon != NULL);
  gtk_widget_set_tooltip_text (self->ide_button, "Open the repository in your IDE");
}

char *
gt_bar_describe (GtBar *self)
{
  g_return_val_if_fail (GT_IS_BAR (self), NULL);
  return g_strdup_printf ("root=%s branch=\"%s\" counts=\"%s\" counts-shown=%d progress=\"%s\" progress-shown=%d "
                          "running=%u ide=\"%s\" remote=%s",
                          root (self) != NULL ? root (self) : "-", gtk_label_get_text (GTK_LABEL (self->branch_label)),
                          gtk_label_get_text (GTK_LABEL (self->counts_label)), gtk_widget_get_visible (self->counts_button),
                          gtk_label_get_text (GTK_LABEL (self->progress_label)),
                          gtk_widget_get_visible (self->progress_button), gt_ops_count_running (root (self)),
                          self->ide != NULL ? g_app_info_get_name (self->ide) : "",
                          self->remote != NULL ? self->remote : "-");
}
