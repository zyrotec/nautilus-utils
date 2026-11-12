/* Sidebar hook: the ONLY sidebar-drives code that touches Nautilus' private sidebar widgets (PLAN.md §8.1.4,
 * §8.6). Adds a usage bar + "X free of Y" to drive rows and an OS drive row above them. Verified against
 * Nautilus 51.1; if the expected types/properties are missing it logs one warning and leaves the sidebar alone. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Main thread, once. */
void sd_sidebar_hook_init (void);

/* Main thread. Removes our rows/decorations' live state and frees everything; safe without init. */
void sd_sidebar_hook_shutdown (void);

/* Switches the feature on or off at runtime (settings, PLAN.md §9.2.4). Off: our rows, network list and
 * decorations are removed and Nautilus' rows restored; On: open sidebars are decorated again. No-op if the
 * hook disabled itself (graceful failure) or isn't initialised. */
void sd_sidebar_hook_set_enabled (gboolean enabled);

/* The bar colour, show-bars/show-labels or eject-slide settings changed: reload the colour CSS and update
 * every decorated row (PLAN.md §9.1.2, §9.1.4, §9.1.5). */
void sd_sidebar_hook_style_changed (void);

G_END_DECLS
