/* Feature 7 — Git (PLAN.md §14): set-up, tear-down, follows the settings. */
#include "git.h"

#include <glib-unix.h>
#include <signal.h>

#include "gt_bar.h"
#include "gt_columns.h"
#include "gt_hook.h"
#include "gt_ops.h"
#include "gt_page.h"
#include "dp_pane.h"
#include "gt_resources.h"
#include "nu_settings.h"

static guint settings_listener;
static GType types[1];
#ifdef GT_DEBUG_DRIVER
static guint debug_signal_id;
#endif

static void
on_pane_view (gboolean git, gpointer data)
{
  (void) data;
  nu_settings_set_git_pane_git (git);
}

/* What was applied last: only what changed is applied again — rebuilding every pane's Git page or looking up the
 * installed IDEs on any Git setting change kept the main loop busy for about a second under ASan (PLAN.md §14.14). */
static struct
{
  gboolean applied;
  gboolean enabled, graph, pane_git;
} last;

static void
apply (void)
{
  const NuConfig *c = nu_settings_get ();
  gt_hook_set_below_all (c->details_pane_enabled && c->details_pane_position == NU_PANE_BOTTOM);
  gt_hook_configure (c->git_enabled, c->git_status_bar, c->git_ide);
  gt_columns_set_enabled (c->git_enabled);
  gt_columns_set_emblems (c->git_emblems);
  GdkRGBA bg = { c->git_bar_colour.r / 255.0f, c->git_bar_colour.g / 255.0f, c->git_bar_colour.b / 255.0f, 1.0f };
  gt_bar_set_look (c->git_bar_border, c->git_bar_custom_colour ? &bg : NULL);
  gt_page_set_tooltips (c->git_commit_tooltip);
  /* The details pane's "Details | Git" (PLAN.md §14.2 item 4); the choice is remembered. */
  if (!last.applied || last.pane_git != c->git_pane_git)
    dp_pane_set_extra_selected (c->git_pane_git);
  if (!last.applied || last.enabled != c->git_enabled || last.graph != c->git_history_graph)
    {
      if (c->git_enabled)
        dp_pane_set_extra ("Git", gt_page_available, gt_page_new, on_pane_view, gt_page_describe,
                           GINT_TO_POINTER (c->git_history_graph));
      else
        dp_pane_set_extra (NULL, NULL, NULL, NULL, NULL, NULL);
    }
  last.applied = TRUE;
  last.enabled = c->git_enabled;
  last.graph = c->git_history_graph;
  last.pane_git = c->git_pane_git;
}

static void
on_settings_changed (NuChanged changed, gpointer data)
{
  (void) data;
  /* The details pane's position decides where the bar goes. */
  if (changed & (NU_CHANGED_GIT | NU_CHANGED_DP_LOOK | NU_CHANGED_DETAILS_PANE))
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
  /* The emblems and the branch icon, for Files' views too (not only our widgets). */
  if (gdk_display_get_default () != NULL)
    gtk_icon_theme_add_resource_path (gtk_icon_theme_get_for_display (gdk_display_get_default ()),
                                      "/io/github/zyrotec/NautilusUtils/git/icons");
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
  dp_pane_set_extra (NULL, NULL, NULL, NULL, NULL, NULL);
  last.applied = FALSE;
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
