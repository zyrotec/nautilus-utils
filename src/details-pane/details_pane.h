/* Details-pane feature entry points, driven by the nautilus-utils module (src/extension.c). */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

/* Starts the feature (settings, window hook). Main thread, once per load. */
void dp_feature_initialize (GTypeModule *module);

/* Stops it and puts every window back as Nautilus built it. */
void dp_feature_shutdown (void);

/* No provider types (static storage, do not free); sets @num_types to 0. */
const GType *dp_feature_list_types (int *num_types);

G_END_DECLS
