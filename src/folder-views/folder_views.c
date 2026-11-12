/* Folder-views feature: set-up and tear-down (PLAN.md §10). Called from src/extension.c. */
#include "folder_views.h"

#include "nu_settings.h"
#include "view_mode_hook.h"

static gboolean running; /* The hook is started. */

void
fv_feature_initialize (GTypeModule *module)
{
  (void) module;
  /* Live on/off from the settings dialog arrives in Phase 3 (PLAN.md §10.6); until then the switch is
   * read once at start. */
  if (nu_settings_get ()->folder_views_enabled)
    {
      fv_view_mode_hook_init ();
      running = TRUE;
    }
  g_debug ("folder-views loaded (%s)", running ? "on" : "off");
}

void
fv_feature_shutdown (void)
{
  if (running)
    fv_view_mode_hook_shutdown ();
  running = FALSE;
  g_debug ("folder-views shut down");
}

const GType *
fv_feature_list_types (int *num_types)
{
  *num_types = 0;
  return NULL;
}
