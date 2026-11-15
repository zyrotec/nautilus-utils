/* Git: "Clone Repository…" (PLAN.md §14.2 item 5): a dialog with the repository's URL (pasted, or taken from the
 * clipboard when it holds one), the new folder's name (from the URL, editable, must not exist) and a Clone button.
 * The clone runs as an operation in the folder (progress in the status bar), with the sign-in dialog when the
 * remote asks for credentials. Plain GTK + libadwaita. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Called when a clone finished successfully: @folder (where) and @name (the new folder). */
typedef void (*GtCloneDone) (const char *folder,
                             const char *name,
                             gpointer    user_data);

/* Opens the dialog over @parent's window for cloning into native folder @folder. */
void gt_clone_dialog_present (GtkWidget   *parent,
                              const char  *folder,
                              GtCloneDone  done,
                              gpointer     user_data);

/* Debug driver: fills the open dialog and presses Clone (FALSE if none is open or Clone is disabled). */
gboolean gt_clone_debug_submit (const char *url,
                                const char *name);

G_END_DECLS
