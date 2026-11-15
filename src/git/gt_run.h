/* Git: running the installed `git` in the background (PLAN.md §14.1 G2, §14.2 items 1 and 6). No GTK.
 *
 * Every run: LC_ALL=C (messages are classified by text), no terminal (its own session, GIT_TERMINAL_PROMPT=0,
 * core.askPass and SSH_ASKPASS cleared, SSH_ASKPASS_REQUIRE=never), so git fails at once instead of waiting for a
 * password; reads also take no optional locks (never the user's index.lock). A timeout and the cancellable stop
 * it (the process is killed). Credentials, when given, go to git through a one-shot credential helper reading
 * them from the child's environment (never its command line), and git then asks the user's own credential
 * helper to store them (§14.5 item 5). */
#pragma once

#include <gio/gio.h>

#include "gt_parse.h"

G_BEGIN_DECLS

typedef enum
{
  GT_RUN_READ    = 1 << 0, /* a read-only command: no optional locks */
  GT_RUN_NETWORK = 1 << 1, /* talks to a remote: progress lines, credentials possible */
} GtRunFlags;

typedef struct
{
  const char *username;
  const char *token;
} GtCredentials;

/* Called on the main context for each progress line git prints (percent -1 if the line has none). */
typedef void (*GtProgressFunc) (const char *phase,
                                int         percent,
                                const char *line,
                                gpointer    user_data);

typedef struct
{
  int          exit_status; /* git's exit code; -1 if it didn't exit normally */
  GBytes      *out;         /* stdout */
  char        *error_text;  /* stderr without progress lines (at most 64 KiB), stripped */
  GtErrorKind  kind;        /* why it failed (exit_status != 0) */
} GtRunResult;

void gt_run_result_free (GtRunResult *result);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GtRunResult, gt_run_result_free)

/* Runs `git <args…>` in @cwd. @timeout_ms 0 = none. @credentials may be NULL (copied). Main context only, and
 * @cancellable is cancelled from it too. */
void         gt_run_async  (const char          *cwd,
                            const char *const   *args,
                            GtRunFlags           flags,
                            const GtCredentials *credentials,
                            guint                timeout_ms,
                            GCancellable        *cancellable,
                            GtProgressFunc       progress,
                            gpointer             progress_data,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data);

/* The result even when git failed (check exit_status); NULL with @error if git couldn't be started, timed out
 * (G_IO_ERROR_TIMED_OUT) or was cancelled (G_IO_ERROR_CANCELLED). */
GtRunResult *gt_run_finish (GAsyncResult  *result,
                            GError       **error);

/* TRUE if a `git` executable is on PATH. */
gboolean gt_run_have_git (void);

G_END_DECLS
