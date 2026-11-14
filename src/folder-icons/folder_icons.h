/* Folder-icons feature entry points, driven by the nautilus-utils module (src/extension.c). */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

void         fi_feature_initialize (GTypeModule *module);
void         fi_feature_shutdown   (void);
const GType *fi_feature_list_types (int *num_types);

G_END_DECLS
