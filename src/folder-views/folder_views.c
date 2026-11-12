/* Folder-views feature: set-up and tear-down (PLAN.md §10). Called from src/extension.c. */
#include "folder_views.h"

#include "nu_settings.h"
#include "view_mode_hook.h"

static gboolean running;           /* The hook is started. */
static guint    settings_listener; /* nu_settings listener id, 0 when not registered. */

/* Follows the settings (once per Apply, or right away for Reset All): on/off, Reset All, our default. */
static void
on_settings_changed (NuChanged changed,
                     gpointer  data)
{
  (void) data;
  if (changed & NU_CHANGED_FOLDER_VIEWS)
    {
      gboolean on = nu_settings_get ()->folder_views_enabled;
      if (on && !running)
        fv_view_mode_hook_init (); /* attaches to every open tab and applies each folder's view */
      else if (!on && running)
        fv_view_mode_hook_shutdown (); /* detaches everything, puts Files' keys back to our default */
      running = on;
      g_debug ("folder views switched %s", on ? "on" : "off");
    }
  if (running && (changed & (NU_CHANGED_FV_GENERATION | NU_CHANGED_FV_DEFAULT)))
    fv_view_mode_hook_refresh ((changed & NU_CHANGED_FV_GENERATION) != 0);
}

void
fv_feature_initialize (GTypeModule *module)
{
  (void) module;
  running = nu_settings_get ()->folder_views_enabled;
  if (running)
    fv_view_mode_hook_init ();
  if (nu_settings_available ())
    settings_listener = nu_settings_add_listener (on_settings_changed, NULL);
  fv_view_mode_hook_debug_init ();
  g_debug ("folder-views loaded (%s)", running ? "on" : "off");
}

void
fv_feature_shutdown (void)
{
  fv_view_mode_hook_debug_shutdown ();
  nu_settings_remove_listener (settings_listener);
  settings_listener = 0;
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
