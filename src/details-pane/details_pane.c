/* Details-pane feature: set-up, tear-down and settings (PLAN.md §13). Called from src/extension.c.
 * Everything the user sees is in dp_pane.c; everything touching Nautilus' widgets in dp_hook.c. */
#include "details_pane.h"

#include "dp_hook.h"
#include "dp_pane.h"
#include "dp_thumb.h"
#include "folder-preview/view_hook.h"
#include "nu_settings.h"

#ifdef DP_DEBUG_DRIVER
#include <glib-unix.h>
#include <signal.h>
#endif

static guint settings_listener; /* nu_settings listener id, 0 when not registered */
#ifdef DP_DEBUG_DRIVER
static guint debug_signal_id;
#endif

static void
on_settings_changed (NuChanged changed, gpointer data)
{
  (void) data;
  const NuConfig *c = nu_settings_get ();
  if (changed & NU_CHANGED_DP_STATE)
    dp_hook_set_state (c->details_pane_visible, c->details_pane_width);
  if (changed & NU_CHANGED_DETAILS_PANE)
    dp_hook_set_enabled (c->details_pane_enabled);
}

#ifdef DP_DEBUG_DRIVER
/* Debug driver (only in -Ddebug_driver=true builds, tools/smoke-nautilus.sh): "dp-*" commands. */
static gboolean
on_debug_signal (gpointer data)
{
  (void) data;
  const char *dir = g_getenv ("FP_DEBUG_DIR");
  g_autofree char *path = dir != NULL ? g_build_filename (dir, "cmd", NULL) : NULL;
  g_autofree char *cmd = NULL;
  if (path != NULL && g_file_get_contents (path, &cmd, NULL, NULL))
    dp_hook_debug_command (g_strstrip (cmd));
  return G_SOURCE_CONTINUE;
}
#endif

void
dp_feature_initialize (GTypeModule *module)
{
  (void) module;
  /* Folders in the pane get the folder-preview look (P4) when that feature draws them. */
  dp_pane_set_folder_renderer (fp_view_hook_render_folder);
  dp_hook_init (nu_settings_get ()->details_pane_enabled);
  settings_listener = nu_settings_add_listener (on_settings_changed, NULL);
#ifdef DP_DEBUG_DRIVER
  debug_signal_id = g_unix_signal_add (SIGUSR2, on_debug_signal, NULL);
#endif
  g_debug ("details-pane loaded");
}

void
dp_feature_shutdown (void)
{
#ifdef DP_DEBUG_DRIVER
  g_clear_handle_id (&debug_signal_id, g_source_remove);
#endif
  nu_settings_remove_listener (settings_listener);
  settings_listener = 0;
  dp_hook_shutdown ();
  dp_pane_set_folder_renderer (NULL);
  dp_pane_unload_style ();
  dp_thumb_shutdown ();
  g_debug ("details-pane shut down");
}

const GType *
dp_feature_list_types (int *num_types)
{
  *num_types = 0;
  return NULL;
}
