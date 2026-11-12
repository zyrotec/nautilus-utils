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

G_END_DECLS
