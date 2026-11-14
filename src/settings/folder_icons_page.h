/* "Folder Icons" tab of the settings dialog (PLAN.md §12.2). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Builds the tab, bound to the dialog's delayed @settings (borrowed; the dialog keeps it alive). Rule edits
 * are pending until Apply, like every setting. */
GtkWidget *st_folder_icons_page (GSettings *settings);

G_END_DECLS
