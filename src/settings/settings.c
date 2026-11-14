/* Settings feature: set-up and tear-down (PLAN.md §9). Called from src/extension.c. */
#include "settings.h"

#include "menu_hook.h"
#include "nu_settings.h"
#include "settings_dialog.h"

#ifdef ST_DEBUG_DRIVER
#include <adwaita.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>

/* Debug driver (only in -Ddebug_driver=true builds, used by tools/smoke-nautilus.sh). Shares
 * $FP_DEBUG_DIR/cmd + SIGUSR2 with the other features' drivers and ignores commands that aren't "st-…":
 *   st-set <key> <GVariant text>   change a setting as the dialog's Apply would (e.g. st-set eject-slide true,
 *                                  st-set bar-colour "'#ff0000'")
 *   st-pending <key> <GVariant>    an unapplied change in the open dialog (as editing a control would)
 *   st-reset <key>                 back to the schema default
 *   st-size <width> <height>       resize the first window (narrow dialog layout)
 *   st-diff                        log the keys the open dialog would change on Apply
 *   st-maximize                    maximize the first window (README screenshots)
 *   st-open / st-close             open / close the settings dialog on the first window
 *   st-tab <name>                  show tab <name> of the open dialog (e.g. folder-icons)
 *   st-dump                        log the current settings */
static guint debug_signal_id;

/* Sets @key in the delayed @pending from GVariant @text; applies it when @apply. */
static void
debug_set_in (GSettings *pending, const char *key, const char *text, gboolean apply)
{
  g_autoptr (GSettingsSchema) schema = NULL;
  g_object_get (pending, "settings-schema", &schema, NULL);
  if (!g_settings_schema_has_key (schema, key))
    {
      g_message ("st: unknown key %s", key);
      return;
    }
  g_autoptr (GSettingsSchemaKey) skey = g_settings_schema_get_key (schema, key);
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) value = g_variant_parse (g_settings_schema_key_get_value_type (skey), text, NULL, NULL, &error);
  if (value == NULL || !g_settings_set_value (pending, key, value))
    {
      g_message ("st: bad value for %s: %s", key, error != NULL ? error->message : text);
      return;
    }
  if (apply)
    g_settings_apply (pending);
  g_message ("st: %s %s = %s", apply ? "set" : "pending", key, text);
}

static void
debug_set (const char *key, const char *text)
{
  g_autoptr (GSettings) pending = nu_settings_new_delayed ();
  if (pending != NULL)
    debug_set_in (pending, key, text, TRUE);
}

static void
debug_reset (const char *key)
{
  g_autoptr (GSettings) pending = nu_settings_new_delayed ();
  if (pending == NULL)
    return;
  g_autoptr (GSettingsSchema) schema = NULL;
  g_object_get (pending, "settings-schema", &schema, NULL);
  if (!g_settings_schema_has_key (schema, key))
    {
      g_message ("st: unknown key %s", key);
      return;
    }
  g_settings_reset (pending, key);
  g_settings_apply (pending);
  g_message ("st: reset %s", key);
}

static void
debug_close (void)
{
  GListModel *toplevels = gtk_window_get_toplevels (); /* borrowed */
  for (guint i = 0; i < g_list_model_get_n_items (toplevels); i++)
    {
      g_autoptr (GtkWidget) window = g_list_model_get_item (toplevels, i);
      GtkWidget *dialog = st_settings_dialog_get (window);
      if (dialog != NULL)
        adw_dialog_close (ADW_DIALOG (dialog));
    }
}

/* Depth-first search for the first AdwViewStack below @w (borrowed). */
static GtkWidget *
find_view_stack (GtkWidget *w)
{
  if (ADW_IS_VIEW_STACK (w))
    return w;
  for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL; c = gtk_widget_get_next_sibling (c))
    {
      GtkWidget *found = find_view_stack (c);
      if (found != NULL)
        return found;
    }
  return NULL;
}

/* An unapplied change in the open dialog, as if a control were edited (Apply becomes sensitive). */
static void
debug_pending (const char *key, const char *text)
{
  GListModel *toplevels = gtk_window_get_toplevels (); /* borrowed */
  for (guint i = 0; i < g_list_model_get_n_items (toplevels); i++)
    {
      g_autoptr (GtkWidget) window = g_list_model_get_item (toplevels, i);
      GtkWidget *dialog = st_settings_dialog_get (window);
      GSettings *pending = dialog != NULL ? g_object_get_data (G_OBJECT (dialog), "st-settings") : NULL;
      if (pending != NULL)
        {
          debug_set_in (pending, key, text, FALSE);
          return;
        }
    }
  g_message ("st: no open dialog");
}

/* Resizes the first window (to try the dialog's narrow layout). */
static void
debug_size (const char *text)
{
  int w = 0, h = 0;
  GListModel *toplevels = gtk_window_get_toplevels (); /* borrowed */
  if (sscanf (text, "%d %d", &w, &h) != 2 || w <= 0 || h <= 0 || g_list_model_get_n_items (toplevels) == 0)
    return;
  g_autoptr (GtkWindow) window = g_list_model_get_item (toplevels, 0);
  gtk_window_unmaximize (window);
  gtk_window_set_default_size (window, w, h);
  g_message ("st: size %d x %d", w, h);
}

/* Logs every key whose pending value in the open dialog differs from the stored one. */
static void
debug_diff (void)
{
  GListModel *toplevels = gtk_window_get_toplevels (); /* borrowed */
  for (guint i = 0; i < g_list_model_get_n_items (toplevels); i++)
    {
      g_autoptr (GtkWidget) window = g_list_model_get_item (toplevels, i);
      GtkWidget *dialog = st_settings_dialog_get (window);
      GSettings *pending = dialog != NULL ? g_object_get_data (G_OBJECT (dialog), "st-settings") : NULL;
      if (pending == NULL)
        continue;
      g_autoptr (GSettings) stored = nu_settings_new_delayed ();
      g_autoptr (GSettingsSchema) schema = NULL;
      g_object_get (pending, "settings-schema", &schema, NULL);
      g_auto (GStrv) keys = g_settings_schema_list_keys (schema);
      guint n = 0;
      for (guint k = 0; keys[k] != NULL; k++)
        {
          g_autoptr (GVariant) a = g_settings_get_value (pending, keys[k]);
          g_autoptr (GVariant) b = g_settings_get_value (stored, keys[k]);
          if (!g_variant_equal (a, b))
            {
              g_autofree char *ta = g_variant_print (a, FALSE);
              g_autofree char *tb = g_variant_print (b, FALSE);
              g_message ("st: diff %s: pending %s, stored %s", keys[k], ta, tb);
              n++;
            }
        }
      g_message ("st: diff done (%u keys, has-unapplied=%d)", n, g_settings_get_has_unapplied (pending));
      return;
    }
  g_message ("st: no open dialog");
}

/* Maximizes the first window (README screenshots). */
static void
debug_maximize (void)
{
  GListModel *toplevels = gtk_window_get_toplevels (); /* borrowed */
  if (g_list_model_get_n_items (toplevels) == 0)
    return;
  g_autoptr (GtkWindow) window = g_list_model_get_item (toplevels, 0);
  gtk_window_maximize (window);
  g_message ("st: maximize");
}

static void
debug_tab (const char *name)
{
  GListModel *toplevels = gtk_window_get_toplevels (); /* borrowed */
  for (guint i = 0; i < g_list_model_get_n_items (toplevels); i++)
    {
      g_autoptr (GtkWidget) window = g_list_model_get_item (toplevels, i);
      GtkWidget *dialog = st_settings_dialog_get (window);
      GtkWidget *stack = dialog != NULL ? find_view_stack (dialog) : NULL;
      if (stack != NULL)
        {
          adw_view_stack_set_visible_child_name (ADW_VIEW_STACK (stack), name);
          g_message ("st: tab %s", name);
          return;
        }
    }
  g_message ("st: no open dialog");
}

static gboolean
on_debug_signal (gpointer data)
{
  (void) data;
  const char *dir = g_getenv ("FP_DEBUG_DIR");
  g_autofree char *path = dir != NULL ? g_build_filename (dir, "cmd", NULL) : NULL;
  g_autofree char *cmd = NULL;
  if (path == NULL || !g_file_get_contents (path, &cmd, NULL, NULL))
    return G_SOURCE_CONTINUE;
  g_strstrip (cmd);
  if (g_str_has_prefix (cmd, "st-set "))
    {
      g_auto (GStrv) parts = g_strsplit (cmd, " ", 3);
      if (g_strv_length (parts) == 3)
        debug_set (parts[1], parts[2]);
    }
  else if (g_str_has_prefix (cmd, "st-pending "))
    {
      g_auto (GStrv) parts = g_strsplit (cmd, " ", 3);
      if (g_strv_length (parts) == 3)
        debug_pending (parts[1], parts[2]);
    }
  else if (g_str_has_prefix (cmd, "st-size "))
    debug_size (cmd + 8);
  else if (g_str_equal (cmd, "st-diff"))
    debug_diff ();
  else if (g_str_equal (cmd, "st-maximize"))
    debug_maximize ();
  else if (g_str_has_prefix (cmd, "st-reset "))
    debug_reset (cmd + 9);
  else if (g_str_equal (cmd, "st-open"))
    g_message ("st: open %s", st_menu_hook_open_settings () ? "ok" : "FAILED (no menu action)");
  else if (g_str_has_prefix (cmd, "st-tab "))
    debug_tab (cmd + 7);
  else if (g_str_equal (cmd, "st-close"))
    debug_close ();
  else if (g_str_equal (cmd, "st-dump"))
    {
      const NuConfig *c = nu_settings_get ();
      g_message ("st dump: available=%d preview=%d drives=%d mode=%d bars=%d labels=%d eject-slide=%d",
                 nu_settings_available (), c->folder_preview_enabled, c->sidebar_drives_enabled,
                 c->bar_colour_mode, c->show_usage_bars, c->show_size_labels, c->eject_slide);
    }
  return G_SOURCE_CONTINUE;
}
#endif

void
st_feature_initialize (GTypeModule *module)
{
  (void) module;
  nu_settings_init ();
  st_menu_hook_init ();
#ifdef ST_DEBUG_DRIVER
  debug_signal_id = g_unix_signal_add (SIGUSR2, on_debug_signal, NULL);
#endif
  g_debug ("settings loaded (schema %s)", nu_settings_available () ? "found" : "missing");
}

void
st_feature_shutdown (void)
{
#ifdef ST_DEBUG_DRIVER
  g_clear_handle_id (&debug_signal_id, g_source_remove);
#endif
  st_menu_hook_shutdown ();
  nu_settings_shutdown ();
  g_debug ("settings shut down");
}

const GType *
st_feature_list_types (int *num_types)
{
  *num_types = 0;
  return NULL;
}
