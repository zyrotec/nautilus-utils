/* Settings feature: set-up and tear-down (PLAN.md §9). Called from src/extension.c. */
#include "settings.h"

#include "menu_hook.h"
#include "nu_settings.h"

void
st_feature_initialize (GTypeModule *module)
{
  (void) module;
  nu_settings_init ();
  st_menu_hook_init ();
  g_debug ("settings loaded (schema %s)", nu_settings_available () ? "found" : "missing");
}

void
st_feature_shutdown (void)
{
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
