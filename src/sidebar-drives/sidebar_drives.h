/* Sidebar-drives feature entry points, driven by the nautilus-utils module (src/extension.c). */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

/* Starts the sidebar hook. Main thread, once per load. The feature registers no Nautilus provider types. */
void sd_feature_initialize (GTypeModule *module);

/* Stops the sidebar hook and frees its state. Main thread. */
void sd_feature_shutdown (void);

/* No provider types: returns NULL with @num_types = 0. */
const GType *sd_feature_list_types (int *num_types);

G_END_DECLS
