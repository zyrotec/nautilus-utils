/* Details pane: the only file using Nautilus internals (PLAN.md §13.3 rule 5, findings §13.7).
 *
 * Per NautilusWindow: a toggle button right of the header's view controls, Alt+Shift+P, and a GtkPaned that
 * puts our DpPane right of the window's AdwTabView (moved once per window, §13.7 item 3). The pane shows the
 * active tab's selection, read from the files view's GtkSelectionModel when the slot's "selection" notifies
 * (§13.7 item 4). If Nautilus' widgets aren't what we expect, one warning is logged and nothing is changed. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Starts watching Nautilus windows. @enabled FALSE = watch nothing until dp_hook_set_enabled (TRUE). */
void dp_hook_init (gboolean enabled);

/* Removes everything from every window and stops watching. Safe without init. */
void dp_hook_shutdown (void);

/* Feature switch (settings): installs into / removes from every window. */
void dp_hook_set_enabled (gboolean enabled);

/* Shown/hidden and width, from the settings (PLAN.md §13.4); applied to every window. */
void dp_hook_set_state (gboolean visible,
                        int      width);

#ifdef DP_DEBUG_DRIVER
/* Debug driver command (tools/smoke-nautilus.sh). Returns TRUE if @cmd was one of ours. */
gboolean dp_hook_debug_command (const char *cmd);
#endif

G_END_DECLS
