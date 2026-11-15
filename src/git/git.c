/* Feature 7 — Git (PLAN.md §14): set-up, tear-down, follows the settings. */
#include "git.h"

#include <glib-unix.h>
#include <signal.h>

#include "gt_bar.h"
#include "gt_columns.h"
#include "gt_hook.h"
#include "gt_ops.h"
#include "gt_resources.h"
#include "nu_settings.h"

static guint settings_listener;
static GType types[1];
#ifdef GT_DEBUG_DRIVER
static guint debug_signal_id;
#endif

static void
apply (void)
{
  const NuConfig *c = nu_settings_get ();
  gt_hook_configure (c->git_enabled, c->git_status_bar, c->git_ide);
  gt_columns_set_enabled (c->git_enabled);
}

static void
on_settings_changed (NuChanged changed, gpointer data)
{
  (void) data;
  if (changed & NU_CHANGED_GIT)
    apply ();
}

#ifdef GT_DEBUG_DRIVER
/* Debug driver (only in -Ddebug_driver=true builds, tools/smoke-nautilus.sh): "gt-*" commands. */
static gboolean
on_debug_signal (gpointer data)
{
  (void) data;
  const char *dir = g_getenv ("FP_DEBUG_DIR");
  g_autofree char *path = dir != NULL ? g_build_filename (dir, "cmd", NULL) : NULL;
  g_autofree char *cmd = NULL;
  if (path != NULL && g_file_get_contents (path, &cmd, NULL, NULL))
    {
      g_debug ("driver read: %s", g_strstrip (cmd)); /* shows a command this handler missed (PLAN.md §14.7) */
      gt_hook_debug_command (cmd);
    }
  return G_SOURCE_CONTINUE;
}
#endif

void
gt_feature_initialize (GTypeModule *module)
{
  g_resources_register (gt_get_resource ());
  gt_columns_register (module);
  types[0] = GT_TYPE_COLUMNS;
  gt_hook_init ();
  apply ();
  settings_listener = nu_settings_add_listener (on_settings_changed, NULL);
#ifdef GT_DEBUG_DRIVER
  debug_signal_id = g_unix_signal_add (SIGUSR2, on_debug_signal, NULL);
#endif
  g_debug ("git loaded");
}

void
gt_feature_shutdown (void)
{
#ifdef GT_DEBUG_DRIVER
  g_clear_handle_id (&debug_signal_id, g_source_remove);
#endif
  nu_settings_remove_listener (settings_listener);
  settings_listener = 0;
  gt_hook_shutdown ();
  gt_columns_shutdown ();
  gt_ops_shutdown ();
  gt_bar_unload_style ();
  g_resources_unregister (gt_get_resource ());
  g_debug ("git shut down");
}

const GType *
gt_feature_list_types (int *num_types)
{
  *num_types = 1; /* the columns (Files' public column and info providers) */
  return types;
}
