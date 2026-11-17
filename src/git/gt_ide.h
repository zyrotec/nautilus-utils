/* Git: "Open in IDE" (PLAN.md §14.2 item 2): the installed IDEs and opening a folder in one. No GTK. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* Known IDEs that are installed (GAppInfo, a new list of new references), in the order of GT_IDE_IDS. */
GList *gt_ide_list (void);

/* The IDE to use: @preferred (a desktop ID) if installed, else the first installed one; NULL if none. New ref. */
GAppInfo *gt_ide_get (const char *preferred);

/* Opens @folder in @ide. */
gboolean gt_ide_open (GAppInfo  *ide,
                      GFile     *folder,
                      GError   **error);

G_END_DECLS
