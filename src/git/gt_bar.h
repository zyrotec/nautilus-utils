/* GtBar: the Git status bar under the folder view (PLAN.md §14.1 G1, §14.2 item 2). Plain GTK + libadwaita, no
 * Nautilus types: the hook (or a demo) tells it which repository to show.
 *
 *   [⑂ main ▾]  ↑2 ↓1 [↑][↓][⟳]                              [progress ▾]  [Open in VS Code | ▾]
 *
 * Branch popover: search, local and remote branches (selecting one switches to it), Create. The counts' tooltip
 * has the last commit; Push / Pull / Sync sit right beside them. Progress popover: this repository's operations
 * with git's message when one fails. Open in …: a split button, its menu the other installed editors. */
#pragma once

#include <adwaita.h>

#include "gt_info.h"

G_BEGIN_DECLS

#define GT_TYPE_BAR (gt_bar_get_type ())
G_DECLARE_FINAL_TYPE (GtBar, gt_bar, GT, BAR, GtkWidget)

GtkWidget *gt_bar_new (void);

/* The repository shown (NULL: none). The bar keeps a reference. */
void gt_bar_set_info (GtBar  *self,
                      GtInfo *info);
GtInfo *gt_bar_get_info (GtBar *self);

/* Outside a repository: the folder whose operations (a clone) the bar shows, alone (NULL: none). */
void gt_bar_set_scope (GtBar      *self,
                       const char *folder);

/* The IDE for "Open in …" (NULL hides the button). */
void gt_bar_set_ide (GtBar    *self,
                     GAppInfo *ide);

/* Actions (the popovers use these; so does the debug driver). Each starts an operation (gt_ops). */
void gt_bar_switch_branch (GtBar      *self,
                           const char *name,
                           gboolean    remote);
void gt_bar_create_branch (GtBar      *self,
                           const char *name);
void gt_bar_refresh       (GtBar      *self);
void gt_bar_push          (GtBar      *self);
void gt_bar_pull          (GtBar      *self);
void gt_bar_sync          (GtBar      *self);
gboolean gt_bar_open_ide  (GtBar      *self);

/* One line describing what is shown (debug driver and tests). Caller frees. */
char *gt_bar_describe (GtBar *self);

/* The options for every bar: the line above it, and its own background (NULL: the view's). */
void gt_bar_set_look (gboolean       border,
                      const GdkRGBA *background);

/* The CSS the bar needs, added once (removed by gt_bar_unload_style). */
void gt_bar_unload_style (void);

G_END_DECLS
