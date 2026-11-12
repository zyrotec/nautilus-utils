/* "Nautilus Utils Settings" entry in Nautilus' main menu (PLAN.md §9.1.1). Main thread only. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Starts adding the entry to every Nautilus window (existing and future). Idempotent. */
void st_menu_hook_init (void);

/* Removes the entry and its action from every window and frees all state. Safe without init. */
void st_menu_hook_shutdown (void);

/* Debug driver: opens the settings dialog on the first Nautilus window that has our action.
 * Returns FALSE if none does. */
gboolean st_menu_hook_open_settings (void);

G_END_DECLS
