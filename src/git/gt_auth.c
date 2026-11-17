/* Git sign-in. See gt_auth.h. */
#include "gt_auth.h"

#include <adwaita.h>
#include <string.h>

typedef struct
{
  GWeakRef    parent;
  GtOp       *op;       /* ref: the failed operation */
  GtAuthDone  done;
  gpointer    user_data;
  GtkWidget  *user_row; /* borrowed (in the dialog) */
  GtkWidget  *token_row;
  GtkWidget  *remember;
  char       *username; /* the last one entered (kept for the next ask) */
} Ask;

static AdwDialog *open_dialog;     /* the one shown (debug driver), or NULL */
static int        store_state = -1; /* -1 unknown, 0 nowhere, 1 the user's helper, 2 libsecret available */

static void
ask_free (Ask *a)
{
  g_weak_ref_clear (&a->parent);
  g_clear_object (&a->op);
  g_free (a->username);
  g_free (a);
}

static void ask (Ask *a, gboolean refused);

/* Where entered credentials can be kept (checked once): the user's own helper, else the keyring helper. */
static int
store (void)
{
  if (store_state >= 0)
    return store_state;
  store_state = 0;
  g_autofree char *helpers = NULL;
  const char *get[] = { "git", "config", "--get-all", "credential.helper", NULL };
  if (g_spawn_sync (NULL, (char **) get, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &helpers,
                    NULL, NULL, NULL) &&
      helpers != NULL && *g_strstrip (helpers) != '\0')
    return store_state = 1;
  g_autofree char *exec_path = NULL;
  const char *ep[] = { "git", "--exec-path", NULL };
  if (g_spawn_sync (NULL, (char **) ep, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &exec_path,
                    NULL, NULL, NULL) &&
      exec_path != NULL)
    {
      g_autofree char *helper = g_build_filename (g_strstrip (exec_path), "git-credential-libsecret", NULL);
      if (g_file_test (helper, G_FILE_TEST_IS_EXECUTABLE))
        store_state = 2;
    }
  return store_state;
}

static void
finish (Ask *a, GtOp *op)
{
  if (a->done != NULL)
    a->done (op, a->user_data);
  ask_free (a);
}

static void
on_retry_done (GtOp *op, gpointer data)
{
  Ask *a = data;
  GtErrorKind kind = gt_op_get_kind (op);
  if (gt_op_get_state (op) == GT_OP_FAILED && (kind == GT_ERROR_AUTH_FAILED || kind == GT_ERROR_NEEDS_CREDENTIALS))
    {
      g_set_object (&a->op, op);
      ask (a, TRUE);
      return;
    }
  finish (a, op);
}

static void
on_response (AdwAlertDialog *dialog, const char *response, gpointer data)
{
  Ask *a = data;
  if (open_dialog == ADW_DIALOG (dialog))
    open_dialog = NULL;
  if (g_strcmp0 (response, "sign-in") != 0)
    {
      finish (a, a->op);
      return;
    }
  g_free (a->username);
  a->username = g_strdup (gtk_editable_get_text (GTK_EDITABLE (a->user_row)));
  g_autofree char *token = g_strdup (gtk_editable_get_text (GTK_EDITABLE (a->token_row)));
  gboolean keep = a->remember != NULL && gtk_check_button_get_active (GTK_CHECK_BUTTON (a->remember));
  GtCredentials c = { a->username, token, keep && store () == 2 ? "libsecret" : NULL };
  if (keep && store () == 2)
    {
      /* Asked for: git uses the keyring from now on (the user's global git configuration). */
      const char *set[] = { "git", "config", "--global", "--add", "credential.helper", "libsecret", NULL };
      g_spawn_sync (NULL, (char **) set, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                    G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, NULL, NULL);
      store_state = 1;
    }
  gt_ops_retry (a->op, &c, on_retry_done, a);
  memset (token, 0, strlen (token)); /* not kept around */
}

static void
on_changed (GtkEditable *e, gpointer data)
{
  (void) e;
  Ask *a = data;
  AdwAlertDialog *d = ADW_ALERT_DIALOG (gtk_widget_get_ancestor (a->user_row, ADW_TYPE_ALERT_DIALOG));
  if (d != NULL)
    adw_alert_dialog_set_response_enabled (d, "sign-in", *gtk_editable_get_text (GTK_EDITABLE (a->user_row)) != '\0' &&
                                                          *gtk_editable_get_text (GTK_EDITABLE (a->token_row)) != '\0');
}

static void
ask (Ask *a, gboolean refused)
{
  g_autoptr (GtkWidget) parent = g_weak_ref_get (&a->parent);
  g_autofree char *target = gt_op_get_auth_target (a->op);
  g_autofree char *host = NULL;
  if (target != NULL)
    {
      g_autoptr (GUri) uri = g_uri_parse (target, G_URI_FLAGS_NONE, NULL);
      host = uri != NULL && g_uri_get_host (uri) != NULL ? g_strdup (g_uri_get_host (uri)) : g_strdup (target);
    }
  gboolean github = host != NULL && (g_str_equal (host, "github.com") || g_str_has_suffix (host, ".github.com"));
  g_autofree char *heading = g_strdup_printf ("Sign In to %s", host != NULL ? host : "the Remote");
  g_autofree char *body = g_strdup_printf (
    "%s%s needs a username and a token (or password) to %s.%s",
    refused ? "The remote refused them. " : "", host != NULL ? host : "The remote", gt_op_get_title (a->op),
    github ? " GitHub needs a personal access token with the “repo” scope, not your password." : "");
  AdwDialog *d = adw_alert_dialog_new (heading, body);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (d), "cancel", "_Cancel", "sign-in", "_Sign In", NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (d), "sign-in", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (d), "sign-in");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (d), "cancel");
  adw_alert_dialog_set_response_enabled (ADW_ALERT_DIALOG (d), "sign-in", FALSE);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (list, "boxed-list");
  a->user_row = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (a->user_row), "Username");
  if (a->username != NULL)
    gtk_editable_set_text (GTK_EDITABLE (a->user_row), a->username);
  a->token_row = adw_password_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (a->token_row), github ? "Personal Access Token" : "Token or Password");
  adw_entry_row_set_activates_default (ADW_ENTRY_ROW (a->token_row), TRUE);
  g_signal_connect (a->user_row, "changed", G_CALLBACK (on_changed), a);
  g_signal_connect (a->token_row, "changed", G_CALLBACK (on_changed), a);
  gtk_list_box_append (GTK_LIST_BOX (list), a->user_row);
  gtk_list_box_append (GTK_LIST_BOX (list), a->token_row);
  gtk_box_append (GTK_BOX (box), list);
  if (github)
    {
      GtkWidget *link = gtk_link_button_new_with_label ("https://github.com/settings/tokens", "Create a token on GitHub");
      gtk_widget_set_halign (link, GTK_ALIGN_START);
      gtk_box_append (GTK_BOX (box), link);
    }
  a->remember = NULL;
  switch (store ())
    {
    case 1:
      {
        GtkWidget *l = gtk_label_new ("Your Git credential helper keeps them after a successful sign-in.");
        gtk_widget_add_css_class (l, "dim-label");
        gtk_label_set_wrap (GTK_LABEL (l), TRUE);
        gtk_box_append (GTK_BOX (box), l);
        break;
      }
    case 2:
      a->remember = gtk_check_button_new_with_label ("Remember in the keyring (sets Git’s credential helper to "
                                                     "“libsecret”)");
      gtk_box_append (GTK_BOX (box), a->remember);
      break;
    default:
      {
        GtkWidget *l = gtk_label_new ("Used for this time only. To have them remembered, install "
                                      "git-credential-libsecret or set up a Git credential helper.");
        gtk_widget_add_css_class (l, "dim-label");
        gtk_label_set_wrap (GTK_LABEL (l), TRUE);
        gtk_box_append (GTK_BOX (box), l);
        break;
      }
    }
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (d), box);
  g_signal_connect (d, "response", G_CALLBACK (on_response), a);
  g_object_set_data (G_OBJECT (d), "gt-ask", a);
  on_changed (NULL, a);
  open_dialog = d;
  adw_dialog_present (d, parent);
  gtk_widget_grab_focus (a->username != NULL ? a->token_row : a->user_row);
}

void
gt_auth_handle (GtkWidget *parent, GtOp *op, GtAuthDone done, gpointer user_data)
{
  g_return_if_fail (GT_IS_OP (op));
  GtErrorKind kind = gt_op_get_kind (op);
  if (gt_op_get_state (op) != GT_OP_FAILED || (kind != GT_ERROR_NEEDS_CREDENTIALS && kind != GT_ERROR_AUTH_FAILED))
    {
      if (done != NULL)
        done (op, user_data);
      return;
    }
  Ask *a = g_new0 (Ask, 1);
  g_weak_ref_init (&a->parent, parent);
  a->op = g_object_ref (op);
  a->done = done;
  a->user_data = user_data;
  ask (a, kind == GT_ERROR_AUTH_FAILED);
}

gboolean
gt_auth_debug_submit (const char *username, const char *token)
{
  if (open_dialog == NULL)
    return FALSE;
  GtkWidget *child = adw_alert_dialog_get_extra_child (ADW_ALERT_DIALOG (open_dialog));
  GtkWidget *list = gtk_widget_get_first_child (child);
  GtkWidget *user = gtk_widget_get_first_child (list);
  GtkWidget *tok = gtk_widget_get_next_sibling (user);
  gtk_editable_set_text (GTK_EDITABLE (user), username);
  gtk_editable_set_text (GTK_EDITABLE (tok), token);
  /* As a click on Sign In: the response, then the dialog closes (without a second, "cancel" response). */
  AdwDialog *d = open_dialog;
  Ask *a = g_object_get_data (G_OBJECT (d), "gt-ask");
  g_signal_emit_by_name (d, "response", "sign-in");
  g_signal_handlers_disconnect_by_func (d, on_response, a);
  adw_dialog_force_close (d);
  return TRUE;
}

char *
gt_auth_debug_describe (void)
{
  if (open_dialog == NULL)
    return NULL;
  return g_strdup_printf ("%s — %s", adw_alert_dialog_get_heading (ADW_ALERT_DIALOG (open_dialog)),
                          adw_alert_dialog_get_body (ADW_ALERT_DIALOG (open_dialog)));
}
