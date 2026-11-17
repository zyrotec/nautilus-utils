/* Feature 8 — Up and Refresh buttons right of Files' Back and Forward buttons. */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

void         nb_feature_initialize (GTypeModule *module);
void         nb_feature_shutdown   (void);
const GType *nb_feature_list_types (int *num_types);

G_END_DECLS
