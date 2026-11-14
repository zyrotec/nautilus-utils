/* Folder icons by name: applying the decisions (PLAN.md §12.3, §12.7). GIO only, no Nautilus types.
 *
 * fi_applier_check() is called (main thread) for every folder Files lists. Folders whose name is not a
 * candidate cost one hash lookup. Candidates are queued (duplicates merged) and processed at most
 * @max_inflight at a time, each in a worker thread: read the folder's metadata, fi_decide(), write, and —
 * if something was written — nudge the folder so Files shows the change at once (§12.7 item 3). A folder is
 * checked once per rule set; fi_applier_set_rules() starts over.
 *
 * The metadata access is a small backend table, so tests can use an in-memory fake (GIO_USE_VFS=local has
 * no gvfs metadata). fi_applier_gio_backend() is the real one.
 */
#pragma once

#include <gio/gio.h>

#include "fi_rules.h"

G_BEGIN_DECLS

/* All functions run in worker threads (several at once) and must be thread-safe. */
typedef struct
{
  /* Reads the three attributes (each a new string or NULL if unset). FALSE with @error on failure. */
  gboolean (*read)  (GFile        *folder,
                     char        **icon,
                     char        **icon_name,
                     char        **marker,
                     gpointer      data,
                     GError      **error);
  /* Applies @action (never FI_ACTION_NONE); @icon_uri is the icon for FI_ACTION_SET. */
  gboolean (*write) (GFile        *folder,
                     FiActionKind  action,
                     const char   *icon_uri,
                     gpointer      data,
                     GError      **error);
  /* Makes Files re-read the folder (may be NULL). Errors are ignored. */
  void     (*nudge) (GFile        *folder,
                     gpointer      data);
} FiBackend;

/* GIO metadata + the access-time nudge for local folders. Static, never freed. */
const FiBackend *fi_applier_gio_backend (void);

typedef struct _FiApplier FiApplier;

/* Called on the main thread after a folder was processed with the action taken (FI_ACTION_NONE also
 * when reading or writing failed: @error_message is then set). Not called after fi_applier_free(). */
typedef void (*FiDoneFunc) (const char   *uri,
                            FiActionKind  action,
                            const char   *error_message,
                            gpointer      user_data);

/* @backend is borrowed (must outlive every job: static, or kept until the applier is freed and its jobs
 * have finished — see fi_applier_get_n_inflight); @backend_data is passed to it. Starts with no rules.
 * Main thread only. Caller frees with fi_applier_free(). */
FiApplier *fi_applier_new (const FiBackend *backend,
                           gpointer         backend_data,
                           guint            max_inflight);

/* Drops the queue and stops reporting; jobs already running finish in the background (they keep what they
 * need alive). */
void fi_applier_free (FiApplier *applier);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FiApplier, fi_applier_free)

void fi_applier_set_done_func (FiApplier  *applier,
                               FiDoneFunc  func,
                               gpointer    user_data);

/* Takes ownership of @rules. Clears the queue and the "already checked" memory: every folder is checked
 * again when Files lists it next. */
void fi_applier_set_rules (FiApplier *applier,
                           FiRules   *rules);

/* Borrowed. */
FiRules *fi_applier_get_rules (FiApplier *applier);

/* Queues @folder (ref'd) if its @name is a candidate and it wasn't checked for the current rules yet.
 * Returns TRUE if it was queued. */
gboolean fi_applier_check (FiApplier  *applier,
                           GFile      *folder,
                           const char *name);

/* Counters (main thread): queued + running jobs, and actions taken since creation. */
guint fi_applier_get_n_pending  (FiApplier *applier);
guint fi_applier_get_n_inflight (void);
guint fi_applier_get_n_set      (FiApplier *applier);
guint fi_applier_get_n_cleared  (FiApplier *applier);
guint fi_applier_get_n_errors   (FiApplier *applier);

G_END_DECLS
