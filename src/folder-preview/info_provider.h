/* NautilusInfoProvider implementation (supported Nautilus API).
 *
 * Phase 1: logs (g_debug) every local directory Nautilus asks about.
 * Per PLAN.md D3 this is NOT a visibility signal (Nautilus calls it for every folder up
 * front), so it will never start scans; later phases use it only for cheap bookkeeping.
 */
#pragma once

#include <nautilus-extension.h>

G_BEGIN_DECLS

#define FP_TYPE_INFO_PROVIDER (fp_info_provider_get_type ())
G_DECLARE_FINAL_TYPE (FpInfoProvider, fp_info_provider, FP, INFO_PROVIDER, GObject)

/* Registers FpInfoProvider as a dynamic type owned by @module. Must be called from
 * nautilus_module_initialize() before FP_TYPE_INFO_PROVIDER is used. Does not take a ref on @module. */
void fp_info_provider_register (GTypeModule *module);

/* TRUE if @file is a directory on a native (local) filesystem, i.e. something we would preview.
 * Does no I/O. @file is borrowed. Exposed for tests. */
gboolean fp_info_provider_should_track (NautilusFileInfo *file);

/* Called (main thread) with the URI of every local folder Nautilus asks about. Lets the module forward
 * "this folder may have changed" to the view hook without this file depending on GTK. NULL clears it. */
typedef void (*FpFolderFunc) (const char *uri);
void fp_info_provider_set_folder_func (FpFolderFunc func);

G_END_DECLS
