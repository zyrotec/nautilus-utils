/* Sidebar-drives feature: set-up and tear-down (PLAN.md §8). Called from src/extension.c. */
#include "sidebar_drives.h"

#include "nu_settings.h"
#include "sidebar_hook.h"

static guint settings_listener; /* nu_settings listener id, 0 when not registered. */

/* Settings changed (once per Apply): follow the on/off switch (PLAN.md §9.1.2). */
static void
on_settings_changed (NuChanged changed,
                     gpointer  data)
{
  (void) data;
  if (changed & NU_CHANGED_SIDEBAR_DRIVES)
    sd_sidebar_hook_set_enabled (nu_settings_get ()->sidebar_drives_enabled);
  if (changed & (NU_CHANGED_BAR_COLOUR | NU_CHANGED_ROW_CONTENT))
    sd_sidebar_hook_style_changed ();
}

void
sd_feature_initialize (GTypeModule *module)
{
  (void) module;
  sd_sidebar_hook_init ();
  sd_sidebar_hook_set_enabled (nu_settings_get ()->sidebar_drives_enabled);
  settings_listener = nu_settings_add_listener (on_settings_changed, NULL);
  g_debug ("sidebar-drives loaded");
}

void
sd_feature_shutdown (void)
{
  nu_settings_remove_listener (settings_listener);
  settings_listener = 0;
  sd_sidebar_hook_shutdown ();
  g_debug ("sidebar-drives shut down");
}

const GType *
sd_feature_list_types (int *num_types)
{
  *num_types = 0;
  return NULL;
}
