/* NautilusInfoProvider for folder icons by name (supported Nautilus API, PLAN.md §12.3). Files calls it for
 * every file it lists; it only hands directories to the feature's callback and never blocks. */
#pragma once

#include <nautilus-extension.h>

G_BEGIN_DECLS

#define FI_TYPE_INFO_PROVIDER (fi_info_provider_get_type ())
G_DECLARE_FINAL_TYPE (FiInfoProvider, fi_info_provider, FI, INFO_PROVIDER, GObject)

/* Registers FiInfoProvider as a dynamic type owned by @module (from nautilus_module_initialize). */
void fi_info_provider_register (GTypeModule *module);

/* Called (main thread) with every directory Files asks about (@folder borrowed). NULL clears it. */
typedef void (*FiFolderFunc) (NautilusFileInfo *folder);
void fi_info_provider_set_folder_func (FiFolderFunc func);

G_END_DECLS
