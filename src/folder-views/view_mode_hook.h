/* Folder views: the only code touching Nautilus internals for this feature (PLAN.md §10.3.1). Watches every
 * tab (NautilusWindowSlot), applies each folder's saved view type and icon size when the tab opens it, and
 * saves the user's changes per folder. Main thread only. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Starts watching tabs: present ones and every tab created later. Captures Files' default once
 * (PLAN.md §10.1.1). Pair with fv_view_mode_hook_shutdown(). */
void fv_view_mode_hook_init (void);

/* Stops watching, cancels pending reads/writes, restores Files' global keys to our default and frees
 * everything. Safe without init. */
void fv_view_mode_hook_shutdown (void);

/* Re-applies the right view to every open tab: after Reset All (@forget_saved: drop the in-memory cache,
 * so every folder is read again and older-generation values are ignored) or when our default changed.
 * No-op without init. */
void fv_view_mode_hook_refresh (gboolean forget_saved);

/* Debug driver (only does something in -Ddebug_driver=true builds): lives for the whole module, so the smoke
 * test can also check that nothing is attached while the feature is off. */
void fv_view_mode_hook_debug_init (void);
void fv_view_mode_hook_debug_shutdown (void);

G_END_DECLS
