/* Git: the status bar in Files' windows (PLAN.md §14.2 item 2). Main thread. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

void gt_hook_init      (void);
/* Feature on/off, bar shown, IDE desktop ID ("" = the first installed). */
void gt_hook_configure (gboolean    enabled,
                        gboolean    show_bar,
                        const char *ide);
/* The details pane is at the bottom: the bar goes below its strip (else under the folder view). */
void gt_hook_set_below_all (gboolean below);
void gt_hook_shutdown  (void);

/* Debug driver: "gt-dump", "gt-expect TEXT", "gt-switch NAME", "gt-switch-remote NAME", "gt-create NAME",
 * "gt-push", "gt-pull", "gt-sync". TRUE if @cmd was one of ours. */
gboolean gt_hook_debug_command (const char *cmd);

G_END_DECLS
