/* Git: signing in when git has no working credentials (PLAN.md §14.1 G3, §14.2 item 6). Plain GTK + libadwaita.
 *
 * An operation that failed because git needed a username and password (HTTPS) or because they were refused opens
 * a dialog for the username and a token; the operation is then run again with them, handed to git for that run
 * only (gt_run's one-shot helper). Refused again: the dialog comes back with the remote's answer. "Remember" is
 * offered when git can keep them: the user's own credential helper (it stores them anyway on success), or the
 * keyring helper (git-credential-libsecret) if it is installed — then also set as git's credential helper. */
#pragma once

#include <gtk/gtk.h>

#include "gt_ops.h"

G_BEGIN_DECLS

/* Called once with the final operation: the first one if it needed no sign-in, else the last retry (or the failed
 * one if the user cancelled). */
typedef void (*GtAuthDone) (GtOp    *op,
                            gpointer user_data);

/* If @op failed for want of credentials (or they were refused), asks for them (a dialog over @parent's window) and
 * retries until it succeeds, fails otherwise or the user cancels; then @done. Otherwise @done at once. */
void gt_auth_handle (GtkWidget  *parent,
                     GtOp       *op,
                     GtAuthDone  done,
                     gpointer    user_data);

/* Debug driver: fills the open dialog and presses Sign In (FALSE if none is open). */
gboolean gt_auth_debug_submit (const char *username,
                               const char *token);

/* Debug driver: the open dialog's heading and body, or NULL. Caller frees. */
char *gt_auth_debug_describe (void);

G_END_DECLS
