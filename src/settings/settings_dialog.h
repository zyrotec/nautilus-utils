/* "Nautilus Utils Settings" dialog (PLAN.md §9.1.2). Plain GTK/libadwaita, no Nautilus internals. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Presents the settings dialog over @parent (a window or any widget inside one). At most one dialog per
 * window: if one is already open there, nothing happens. Controls edit a delayed copy of the settings;
 * Apply writes them, closing without Apply discards them. Main thread. */
void st_settings_dialog_present (GtkWidget *parent);

/* The dialog open over @parent's window, or NULL. Borrowed (owned by GTK while presented). Debug/tests. */
GtkWidget *st_settings_dialog_get (GtkWidget *parent);

G_END_DECLS
