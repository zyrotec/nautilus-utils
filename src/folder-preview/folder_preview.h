/* Folder-preview feature entry points, driven by the nautilus-utils module (src/extension.c). */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

/* Registers the feature's types on @module and starts its services. Main thread, once per load. */
void fp_feature_initialize (GTypeModule *module);

/* Stops the feature's services. Main thread; safe to call after fp_feature_initialize() only. */
void fp_feature_shutdown (void);

/* Returns the feature's Nautilus provider types (static storage, do not free); sets @num_types. */
const GType *fp_feature_list_types (int *num_types);

G_END_DECLS
