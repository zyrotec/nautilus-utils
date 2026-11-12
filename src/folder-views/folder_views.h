/* Folder-views feature entry points, driven by the nautilus-utils module (src/extension.c). PLAN.md §10. */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

/* Starts the feature (if switched on in the settings). Main thread, once per load, after the settings. */
void fv_feature_initialize (GTypeModule *module);

/* Stops it and frees everything. Main thread; safe after fv_feature_initialize() only. */
void fv_feature_shutdown (void);

/* No Nautilus provider types: returns NULL and sets @num_types to 0. */
const GType *fv_feature_list_types (int *num_types);

G_END_DECLS
