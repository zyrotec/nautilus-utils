/* Git clone dialog. See gt_clone.h. */
#include "gt_clone.h"

#include <adwaita.h>
#include <string.h>

#include "gt_auth.h"
#include "gt_ops.h"
#include "gt_parse.h"

typedef struct
{
  AdwDialog   *dialog;    /* borrowed: we are its data */
  char        *folder;
  GtCloneDone  done;
  gpointer     user_data;
  GtkWidget   *url_row;
  GtkWidget   *name_row;
  GtkWidget   *problem;   /* why Clone is disabled */
  GtkWidget   *clone_button;
  gboolean     name_edited; /* the user typed a name: stop following the URL */
  gboolean     setting_name;
} Clone;

typedef struct
{
  char       *folder;
  char       *name;
  GtCloneDone done;
  gpointer    user_data;
  GWeakRef    parent;
} Running;

static AdwDialog *open_dialog; /* debug driver */

static void
clone_free (gpointer data)
{
  Clone *c = data;
  if (open_dialog == c->dialog)
    open_dialog = NULL;
  g_free (c->folder);
  g_free (c);
}

static void
running_free (Running *r)
{
  g_free (r->folder);
  g_free (r->name);
  g_weak_ref_clear (&r->parent);
  g_free (r);
}

/* Validates; Clone is enabled only for a cloneable URL and a free, plain folder name. */
static void
validate (Clone *c)
{
  g_autofree char *url = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (c->url_row))));
  g_autofree char *name = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (c->name_row))));
  const char *problem = NULL;
  if (*url == '\0')
    problem = "";
  else if (!gt_url_is_valid (url))
    problem = "This doesn’t look like a Git URL (https://…, ssh://…, git@host:path)";
  else if (*name == '\0')
    problem = "The new folder needs a name";
  else if (strchr (name, '/') != NULL || g_str_equal (name, ".") || g_str_equal (name, "..") || name[0] == '-')
    problem = "Not a usable folder name";
  else
    {
      g_autofree char *target = g_build_filename (c->folder, name, NULL);
      if (g_file_test (target, G_FILE_TEST_EXISTS))
        problem = "A file or folder with this name is already here";
    }
  gtk_label_set_text (GTK_LABEL (c->problem), problem != NULL ? problem : "");
  gtk_widget_set_visible (c->problem, problem != NULL && *problem != '\0');
  gtk_widget_set_sensitive (c->clone_button, problem == NULL);
}

static void
on_url_changed (GtkEditable *e, gpointer data)
{
  (void) e;
  Clone *c = data;
  if (!c->name_edited)
    {
      g_autofree char *name = gt_url_dir_name (gtk_editable_get_text (GTK_EDITABLE (c->url_row)));
      c->setting_name = TRUE;
      gtk_editable_set_text (GTK_EDITABLE (c->name_row), name != NULL ? name : "");
      c->setting_name = FALSE;
    }
  validate (c);
}

static void
on_name_changed (GtkEditable *e, gpointer data)
{
  (void) e;
  Clone *c = data;
  if (!c->setting_name)
    c->name_edited = TRUE;
  validate (c);
}

static void
on_clone_finished (GtOp *op, gpointer data)
{
  Running *r = data;
  if (gt_op_get_state (op) == GT_OP_DONE && r->done != NULL)
    r->done (r->folder, r->name, r->user_data);
  running_free (r);
}

/* The first attempt ended: a sign-in (and retries) if the remote asked for one. */
static void
on_first_done (GtOp *op, gpointer data)
{
  Running *r = data;
  g_autoptr (GtkWidget) parent = g_weak_ref_get (&r->parent);
  gt_auth_handle (parent, op, on_clone_finished, r);
}

static void
on_clone (GtkButton *button, gpointer data)
{
  (void) button;
  Clone *c = data;
  if (!gtk_widget_get_sensitive (c->clone_button))
    return;
  g_autofree char *url = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (c->url_row))));
  Running *r = g_new0 (Running, 1);
  r->folder = g_strdup (c->folder);
  r->name = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (c->name_row))));
  r->done = c->done;
  r->user_data = c->user_data;
  GtkWidget *parent = GTK_WIDGET (gtk_widget_get_root (GTK_WIDGET (c->dialog)));
  g_weak_ref_init (&r->parent, parent);
  g_autofree char *title = g_strdup_printf ("Clone %s", r->name);
  /* "--": a URL can never be taken for an option (gt_url_is_valid also refuses a leading '-'). */
  const char *args[] = { "clone", "--progress", "--", url, r->name, NULL };
  gt_ops_start (c->folder, title, args, GT_RUN_NETWORK, NULL, "Cloned", on_first_done, r);
  adw_dialog_close (c->dialog);
}

static void
on_clipboard (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autofree char *text = gdk_clipboard_read_text_finish (GDK_CLIPBOARD (src), res, NULL);
  GWeakRef *ref = data;
  g_autoptr (GtkWidget) row = g_weak_ref_get (ref);
  g_weak_ref_clear (ref);
  g_free (ref);
  if (row == NULL || text == NULL || *gtk_editable_get_text (GTK_EDITABLE (row)) != '\0')
    return;
  g_strstrip (text);
  if (gt_url_is_valid (text) && strlen (text) < 2048)
    gtk_editable_set_text (GTK_EDITABLE (row), text);
}

void
gt_clone_dialog_present (GtkWidget *parent, const char *folder, GtCloneDone done, gpointer user_data)
{
  g_return_if_fail (folder != NULL);
  Clone *c = g_new0 (Clone, 1);
  c->folder = g_strdup (folder);
  c->done = done;
  c->user_data = user_data;
  c->dialog = adw_dialog_new ();
  adw_dialog_set_title (c->dialog, "Clone Repository");
  adw_dialog_set_content_width (c->dialog, 460);
  g_object_set_data_full (G_OBJECT (c->dialog), "gt-clone", c, clone_free);

  GtkWidget *view = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);
  GtkWidget *cancel = gtk_button_new_with_mnemonic ("_Cancel");
  g_signal_connect_swapped (cancel, "clicked", G_CALLBACK (adw_dialog_close), c->dialog);
  c->clone_button = gtk_button_new_with_mnemonic ("_Clone");
  gtk_widget_add_css_class (c->clone_button, "suggested-action");
  gtk_widget_set_sensitive (c->clone_button, FALSE);
  g_signal_connect (c->clone_button, "clicked", G_CALLBACK (on_clone), c);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), c->clone_button);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (view), header);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 12);
  GtkWidget *list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (list, "boxed-list");
  c->url_row = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (c->url_row), "Repository URL");
  adw_entry_row_set_activates_default (ADW_ENTRY_ROW (c->url_row), TRUE);
  c->name_row = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (c->name_row), "Folder Name");
  adw_entry_row_set_activates_default (ADW_ENTRY_ROW (c->name_row), TRUE);
  g_signal_connect (c->url_row, "changed", G_CALLBACK (on_url_changed), c);
  g_signal_connect (c->name_row, "changed", G_CALLBACK (on_name_changed), c);
  gtk_list_box_append (GTK_LIST_BOX (list), c->url_row);
  gtk_list_box_append (GTK_LIST_BOX (list), c->name_row);
  gtk_box_append (GTK_BOX (box), list);
  g_autofree char *where_text = g_strdup_printf ("Into %s", folder);
  GtkWidget *where = gtk_label_new (where_text);
  gtk_widget_add_css_class (where, "dim-label");
  gtk_label_set_xalign (GTK_LABEL (where), 0);
  gtk_label_set_wrap (GTK_LABEL (where), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (where), PANGO_WRAP_WORD_CHAR);
  gtk_box_append (GTK_BOX (box), where);
  c->problem = gtk_label_new (NULL);
  gtk_widget_add_css_class (c->problem, "error");
  gtk_label_set_xalign (GTK_LABEL (c->problem), 0);
  gtk_label_set_wrap (GTK_LABEL (c->problem), TRUE);
  gtk_widget_set_visible (c->problem, FALSE);
  gtk_box_append (GTK_BOX (box), c->problem);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (view), box);
  adw_dialog_set_child (c->dialog, view);
  adw_dialog_set_default_widget (c->dialog, c->clone_button);
  adw_dialog_set_focus (c->dialog, c->url_row);

  /* A Git URL on the clipboard is filled in (only into an empty field). */
  GWeakRef *ref = g_new0 (GWeakRef, 1);
  g_weak_ref_init (ref, c->url_row);
  gdk_clipboard_read_text_async (gtk_widget_get_clipboard (parent != NULL ? parent : c->url_row), NULL, on_clipboard, ref);
  open_dialog = c->dialog;
  adw_dialog_present (c->dialog, parent);
}

gboolean
gt_clone_debug_submit (const char *url, const char *name)
{
  Clone *c = open_dialog != NULL ? g_object_get_data (G_OBJECT (open_dialog), "gt-clone") : NULL;
  if (c == NULL)
    return FALSE;
  gtk_editable_set_text (GTK_EDITABLE (c->url_row), url);
  if (name != NULL)
    gtk_editable_set_text (GTK_EDITABLE (c->name_row), name);
  if (!gtk_widget_get_sensitive (c->clone_button))
    {
      g_message ("gt clone: Clone disabled: %s", gtk_label_get_text (GTK_LABEL (c->problem)));
      return FALSE;
    }
  on_clone (GTK_BUTTON (c->clone_button), c);
  return TRUE;
}
