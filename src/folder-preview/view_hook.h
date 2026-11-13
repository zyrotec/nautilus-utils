/* View hook: the ONLY code that touches Nautilus' private widget tree (PLAN.md §2).
 *
 * Finds grid-view icons (NautilusImage inside NautilusGridCell) as they are mapped, installs an
 * FpFolderPaintable showing the folder's preview (D4), keeps it installed when Nautilus resets the icon,
 * and cycles hover frames (PLAN.md §6.1.3). Verified against Nautilus 51.1 (PLAN.md §6.4). If the expected
 * types or properties are missing it logs one warning and disables itself; Nautilus keeps working.
 * Main thread only.
 */
#pragma once

#include "cache.h"
#include "composite.h"
#include "folder_paintable.h"

G_BEGIN_DECLS

/* Starts watching for grid icons. @cache is ref'd. Call once, from nautilus_module_initialize. */
void fp_view_hook_init (FpCache *cache);

/* Removes every hook, timer and handler, restores the icons and drops the cache ref. Safe to call when
 * not initialised. */
void fp_view_hook_shutdown (void);

/* Switches the feature on or off at runtime (settings, PLAN.md §9.2.4). Off: every icon is given back to
 * Nautilus and new icons are left alone. On: icons already on screen are hooked again. No-op if the hook
 * disabled itself (graceful failure) or isn't initialised. */
void fp_view_hook_set_enabled (gboolean enabled);

/* TRUE while previews are being drawn (initialised, not disabled, not switched off). */
gboolean fp_view_hook_get_enabled (void);

/* Sets the look (PLAN.md §11): frame style (also set on the cache), stack placement (@position, @offset px
 * at 64 × 64; moving the stack needs no new frames), hover cycling, and the custom folder /
 * overlay SVG paths for the stacked layout ("" or NULL = none; an unreadable file counts as none). Visible
 * icons are redrawn in the new look; nothing happens if nothing changed. */
void fp_view_hook_set_look (const FpStyle   *style,
                            FpStackPosition  position,
                            int              offset,
                            gboolean         cycle,
                            const char      *folder_icon,
                            const char      *overlay_icon);

/* Nautilus reported that the folder @uri changed: forget its cached frames and refresh visible icons. */
void fp_view_hook_folder_changed (const char *uri);

G_END_DECLS
