/* Feature 7 — Git (PLAN.md §14). */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

void         gt_feature_initialize (GTypeModule *module);
void         gt_feature_shutdown   (void);
const GType *gt_feature_list_types (int *num_types);

G_END_DECLS
