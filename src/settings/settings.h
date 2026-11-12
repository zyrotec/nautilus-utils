/* Settings feature entry points (PLAN.md §9), driven by the nautilus-utils module (src/extension.c).
 * Initialized first, so the other features can read the settings in their own initialize. */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

/* Starts the shared settings service (nu_settings) and the main-menu entry. Main thread, once per load. */
void st_feature_initialize (GTypeModule *module);

/* Removes the menu entry and stops the settings service. Main thread. */
void st_feature_shutdown (void);

/* No provider types: returns NULL with @num_types = 0. */
const GType *st_feature_list_types (int *num_types);

G_END_DECLS
