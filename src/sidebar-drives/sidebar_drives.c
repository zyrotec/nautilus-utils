/* Sidebar-drives feature: set-up and tear-down (PLAN.md §8). Called from src/extension.c. */
#include "sidebar_drives.h"

#include "sidebar_hook.h"

void
sd_feature_initialize (GTypeModule *module)
{
  (void) module;
  sd_sidebar_hook_init ();
  g_debug ("sidebar-drives loaded");
}

void
sd_feature_shutdown (void)
{
  sd_sidebar_hook_shutdown ();
  g_debug ("sidebar-drives shut down");
}

const GType *
sd_feature_list_types (int *num_types)
{
  *num_types = 0;
  return NULL;
}
