/* Feature 8 — Up and Refresh buttons next to Files' Back and Forward buttons.
 *
 * Nautilus internals used (looked up by name, no private headers):
 *   - NautilusWindow, found through GTK's toplevel list;
 *   - NautilusHistoryControls (an AdwBin around a GtkBox [Back][Forward]); a window has two of them, one in the
 *     header bar and one in the bottom bar of a narrow window. Our buttons go into that box, after Forward.
 *   - The window's "slot.up" and "slot.reload" actions (Alt+Up and F5), which Files enables and disables itself
 *     (Up is off in "/"), so the buttons follow without any code of ours.
 * If NautilusHistoryControls is missing or laid out differently, nothing is added (one warning). */
#include "nav_buttons.h"

#include <adwaita.h>

#include <string.h>

#include "nu_settings.h"

#ifdef NB_DEBUG_DRIVER
#include <glib-unix.h>
#include <signal.h>
#endif

#define OURS_KEY "nu-nav-button" /* on our buttons */
#define DONE_KEY "nu-nav-done"   /* on windows we looked at */

static struct
{
  gboolean    initialised;
  gboolean    enabled;
  GListModel *toplevels; /* borrowed from GTK */
  gulong      changed_id;
  guint       scan_id;
  guint       listener;
  GType       window_type, controls_type;
  gboolean    warned;
#ifdef NB_DEBUG_DRIVER
  guint       debug_signal_id;
#endif
} nb;

static GType types[1];

static void
warn_once (const char *what)
{
  if (nb.warned)
    return;
  nb.warned = TRUE;
  g_warning ("Up and Refresh buttons disabled: %s (Nautilus changed?)", what);
}

static void
collect (GtkWidget *w, GType t, GPtrArray *out)
{
  if (G_TYPE_CHECK_INSTANCE_TYPE (w, t))
    {
      g_ptr_array_add (out, w);
      return;
    }
  if (GTK_IS_LIST_VIEW (w) || GTK_IS_GRID_VIEW (w) || GTK_IS_COLUMN_VIEW (w))
    return; /* the folder view: never history controls, and possibly thousands of children */
  for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL; c = gtk_widget_get_next_sibling (c))
    collect (c, t, out);
}

static GtkWidget *
nav_button (const char *icon, const char *tooltip, const char *action)
{
  GtkWidget *b = gtk_button_new_from_icon_name (icon);
  gtk_widget_set_tooltip_text (b, tooltip);
  gtk_widget_set_valign (b, GTK_ALIGN_CENTER);
  gtk_actionable_set_action_name (GTK_ACTIONABLE (b), action);
  gtk_accessible_update_property (GTK_ACCESSIBLE (b), GTK_ACCESSIBLE_PROPERTY_LABEL, tooltip, -1);
  g_object_set_data (G_OBJECT (b), OURS_KEY, GINT_TO_POINTER (1));
  return b;
}

/* The box [Back][Forward] inside @controls, or NULL if it isn't laid out like that. */
static GtkWidget *
controls_box (GtkWidget *controls)
{
  GtkWidget *box = ADW_IS_BIN (controls) ? adw_bin_get_child (ADW_BIN (controls)) : NULL;
  if (!GTK_IS_BOX (box))
    return NULL;
  for (GtkWidget *c = gtk_widget_get_first_child (box); c != NULL; c = gtk_widget_get_next_sibling (c))
    if (!GTK_IS_BUTTON (c))
      return NULL;
  return box;
}

static guint
count_ours (GtkWidget *box)
{
  guint n = 0;
  for (GtkWidget *c = gtk_widget_get_first_child (box); c != NULL; c = gtk_widget_get_next_sibling (c))
    n += g_object_get_data (G_OBJECT (c), OURS_KEY) != NULL;
  return n;
}

static void
add_to (GtkWidget *controls)
{
  GtkWidget *box = controls_box (controls);
  if (box == NULL)
    {
      warn_once ("the Back / Forward buttons are not where expected");
      return;
    }
  if (count_ours (box) > 0)
    return;
  gtk_box_append (GTK_BOX (box), nav_button ("go-up-symbolic", "Up", "slot.up"));
  gtk_box_append (GTK_BOX (box), nav_button ("view-refresh-symbolic", "Refresh", "slot.reload"));
}

static void
remove_from (GtkWidget *controls)
{
  GtkWidget *box = controls_box (controls);
  if (box == NULL)
    return;
  GtkWidget *c = gtk_widget_get_first_child (box);
  while (c != NULL)
    {
      GtkWidget *next = gtk_widget_get_next_sibling (c);
      if (g_object_get_data (G_OBJECT (c), OURS_KEY) != NULL)
        gtk_box_remove (GTK_BOX (box), c);
      c = next;
    }
}

static gboolean
types_ready (void)
{
  if (nb.window_type == 0)
    nb.window_type = g_type_from_name ("NautilusWindow");
  if (nb.controls_type == 0)
    nb.controls_type = g_type_from_name ("NautilusHistoryControls");
  return nb.window_type != 0 && nb.controls_type != 0;
}

/* Adds (enabled) or removes our buttons in every Files window. */
static void
apply_all (gboolean add)
{
  if (!types_ready ())
    {
      if (add && nb.window_type != 0)
        warn_once ("NautilusHistoryControls is missing");
      return;
    }
  for (guint i = 0; i < g_list_model_get_n_items (nb.toplevels); i++)
    {
      g_autoptr (GObject) w = g_list_model_get_item (nb.toplevels, i);
      if (!G_TYPE_CHECK_INSTANCE_TYPE (w, nb.window_type))
        continue;
      if (add && g_object_get_data (w, DONE_KEY) != NULL)
        continue;
      g_object_set_data (w, DONE_KEY, add ? GINT_TO_POINTER (1) : NULL);
      g_autoptr (GPtrArray) controls = g_ptr_array_new ();
      collect (GTK_WIDGET (w), nb.controls_type, controls);
      for (guint j = 0; j < controls->len; j++)
        (add ? add_to : remove_from) (g_ptr_array_index (controls, j));
    }
}

static gboolean
scan_idle (gpointer data)
{
  (void) data;
  nb.scan_id = 0;
  if (nb.enabled)
    apply_all (TRUE);
  return G_SOURCE_REMOVE;
}

static void
on_toplevels_changed (GListModel *list, guint position, guint removed, guint added, gpointer data)
{
  (void) list, (void) position, (void) removed, (void) data;
  if (added > 0 && nb.enabled && nb.scan_id == 0)
    nb.scan_id = g_idle_add (scan_idle, NULL);
}

static void
set_enabled (gboolean enabled)
{
  if (nb.enabled == enabled)
    return;
  nb.enabled = enabled;
  if (enabled)
    {
      if (nb.scan_id == 0)
        nb.scan_id = g_idle_add (scan_idle, NULL);
    }
  else
    {
      g_clear_handle_id (&nb.scan_id, g_source_remove);
      apply_all (FALSE);
    }
}

static void
on_settings_changed (NuChanged changed, gpointer data)
{
  (void) data;
  if (changed & NU_CHANGED_NAV_BUTTONS)
    set_enabled (nu_settings_get ()->nav_buttons_enabled);
}

#ifdef NB_DEBUG_DRIVER
/* Debug driver (-Ddebug_driver=true builds, tools/smoke-nautilus.sh): "nb-dump" logs our buttons per window. */
static gboolean
on_debug_signal (gpointer data)
{
  (void) data;
  const char *dir = g_getenv ("FP_DEBUG_DIR");
  g_autofree char *path = dir != NULL ? g_build_filename (dir, "cmd", NULL) : NULL;
  g_autofree char *cmd = NULL;
  if (path == NULL || !g_file_get_contents (path, &cmd, NULL, NULL) || !g_str_has_prefix (g_strstrip (cmd), "nb-dump"))
    return G_SOURCE_CONTINUE;
  if (!types_ready ())
    {
      g_message ("nb: types missing");
      return G_SOURCE_CONTINUE;
    }
  for (guint i = 0; i < g_list_model_get_n_items (nb.toplevels); i++)
    {
      g_autoptr (GObject) w = g_list_model_get_item (nb.toplevels, i);
      if (!G_TYPE_CHECK_INSTANCE_TYPE (w, nb.window_type))
        continue;
      g_autoptr (GPtrArray) controls = g_ptr_array_new ();
      collect (GTK_WIDGET (w), nb.controls_type, controls);
      g_autoptr (GString) s = g_string_new (NULL);
      for (guint j = 0; j < controls->len; j++)
        {
          GtkWidget *box = controls_box (g_ptr_array_index (controls, j));
          g_string_append_printf (s, " controls%u=[", j);
          for (GtkWidget *c = box != NULL ? gtk_widget_get_first_child (box) : NULL; c != NULL; c = gtk_widget_get_next_sibling (c))
            g_string_append_printf (s, "%s%s%s", c != gtk_widget_get_first_child (box) ? "," : "",
                                    gtk_actionable_get_action_name (GTK_ACTIONABLE (c)) != NULL
                                      ? gtk_actionable_get_action_name (GTK_ACTIONABLE (c)) : "?",
                                    gtk_widget_is_sensitive (c) ? "" : "(off)");
          g_string_append (s, "]");
        }
      g_message ("nb: window%s", s->str);
    }
  return G_SOURCE_CONTINUE;
}
#endif

void
nb_feature_initialize (GTypeModule *module)
{
  (void) module;
  if (nb.initialised)
    return;
  nb.initialised = TRUE;
  nb.toplevels = gtk_window_get_toplevels (); /* borrowed */
  nb.changed_id = g_signal_connect (nb.toplevels, "items-changed", G_CALLBACK (on_toplevels_changed), NULL);
  nb.listener = nu_settings_add_listener (on_settings_changed, NULL);
  set_enabled (nu_settings_get ()->nav_buttons_enabled);
#ifdef NB_DEBUG_DRIVER
  nb.debug_signal_id = g_unix_signal_add (SIGUSR2, on_debug_signal, NULL);
#endif
  g_debug ("nav-buttons loaded");
}

void
nb_feature_shutdown (void)
{
  if (!nb.initialised)
    return;
#ifdef NB_DEBUG_DRIVER
  g_clear_handle_id (&nb.debug_signal_id, g_source_remove);
#endif
  nu_settings_remove_listener (nb.listener);
  set_enabled (FALSE);
  g_clear_signal_handler (&nb.changed_id, nb.toplevels);
  memset (&nb, 0, sizeof nb);
  g_debug ("nav-buttons shut down");
}

const GType *
nb_feature_list_types (int *num_types)
{
  *num_types = 0;
  return types;
}
