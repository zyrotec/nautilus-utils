/* Settings feature: set-up and tear-down (PLAN.md §9). Called from src/extension.c. */
#include "settings.h"

#include "menu_hook.h"
#include "nu_settings.h"
#include "settings_dialog.h"

#ifdef ST_DEBUG_DRIVER
#include <adwaita.h>
#include <glib-unix.h>
#include <signal.h>

/* Debug driver (only in -Ddebug_driver=true builds, used by tools/smoke-nautilus.sh). Shares
 * $FP_DEBUG_DIR/cmd + SIGUSR2 with the other features' drivers and ignores commands that aren't "st-…":
 *   st-set <key> <GVariant text>   change a setting as the dialog's Apply would (e.g. st-set eject-slide true,
 *                                  st-set bar-colour "'#ff0000'")
 *   st-open / st-close             open / close the settings dialog on the first window
 *   st-dump                        log the current settings */
static guint debug_signal_id;

static void
debug_set (const char *key, const char *text)
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
  g_autoptr (GSettingsSchemaKey) skey = g_settings_schema_get_key (schema, key);
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) value = g_variant_parse (g_settings_schema_key_get_value_type (skey), text, NULL, NULL, &error);
  if (value == NULL || !g_settings_set_value (pending, key, value))
    {
      g_message ("st: bad value for %s: %s", key, error != NULL ? error->message : text);
      return;
    }
  g_settings_apply (pending);
  g_message ("st: set %s = %s", key, text);
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
  else if (g_str_equal (cmd, "st-open"))
    g_message ("st: open %s", st_menu_hook_open_settings () ? "ok" : "FAILED (no menu action)");
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
