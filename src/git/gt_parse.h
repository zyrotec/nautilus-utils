/* Git: parsers for git's machine-readable output (PLAN.md §14.2). Pure functions, no I/O, no GTK. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* One file's state in `git status`; for folders also "contains changes". */
typedef enum
{
  GT_STATE_NONE = 0,  /* unchanged (tracked) or not known */
  GT_STATE_MODIFIED,
  GT_STATE_ADDED,
  GT_STATE_DELETED,
  GT_STATE_RENAMED,
  GT_STATE_COPIED,
  GT_STATE_TYPE_CHANGED,
  GT_STATE_UNTRACKED,
  GT_STATE_IGNORED,
  GT_STATE_CONFLICT,
  GT_STATE_CONTAINS,  /* a folder with changes inside */
} GtState;

/* Display name of @state ("" for GT_STATE_NONE). Static. */
const char *gt_state_label (GtState state);

/* `git status --porcelain=v2 -z --branch` run at the repository root. */
typedef struct
{
  char       *oid;       /* HEAD commit, NULL before the first commit */
  char       *head;      /* branch name, NULL when detached */
  char       *upstream;  /* e.g. "origin/main", or NULL */
  gboolean    has_ab;    /* ahead / behind known (an upstream exists and was found) */
  int         ahead, behind;
  GHashTable *files;     /* root-relative path ('/'-separated, no trailing '/') → GtState (GINT_TO_POINTER) */
  GHashTable *dirty;     /* every parent folder of a changed (not ignored) path → GINT_TO_POINTER (1) */
} GtStatus;

GtStatus *gt_status_parse (const char *data,
                           gsize       len);
void      gt_status_free  (GtStatus *status);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GtStatus, gt_status_free)

/* State of root-relative @path: its own entry; a folder without one but with changes inside: CONTAINS; inside an
 * ignored or untracked folder: that folder's state. */
GtState gt_status_state (const GtStatus *status,
                         const char     *path,
                         gboolean        is_dir);

/* A branch from `git for-each-ref --format=GT_BRANCH_FORMAT refs/heads refs/remotes`. */
#define GT_BRANCH_FORMAT "%(refname)%00%(HEAD)%00%(upstream:short)%00%(objectname:short)%00%(committerdate:unix)%00%(contents:subject)"
typedef struct
{
  char    *name;     /* "main", or "origin/main" for a remote branch */
  gboolean remote;
  gboolean current;
  char    *upstream; /* local branches: "origin/main" or NULL */
  char    *hash;     /* short */
  gint64   date;     /* unix seconds */
  char    *subject;
} GtBranch;

void       gt_branch_free     (GtBranch *branch);
/* Locals first (current first), then remotes; each part by name. Remote "HEAD" pointers are left out. */
GPtrArray *gt_branches_parse  (const char *data,
                               gsize       len);

/* A commit from `git log --format=GT_LOG_FORMAT --name-only -z`. */
#define GT_LOG_FORMAT "%x1e%H%x00%an%x00%at%x00%s"
typedef struct
{
  gatomicrefcount ref;
  char           *hash;
  char           *author;
  gint64          date;
  char           *subject;
} GtCommit;

GtCommit *gt_commit_ref   (GtCommit *commit);
void      gt_commit_unref (GtCommit *commit);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GtCommit, gt_commit_unref)

/* Last commit of each item in folder @dir (root-relative, "" = the root): item name → GtCommit* (a reference
 * each). Items not seen in @data are missing. */
GHashTable *gt_log_last_commits (const char *data,
                                 gsize       len,
                                 const char *dir);

/* The first commit record in @data (e.g. `git log -1`), or NULL. Caller owns the reference. */
GtCommit *gt_log_first (const char *data,
                        gsize       len);

/* Why a git run failed, from its error text (LC_ALL=C). */
typedef enum
{
  GT_ERROR_OTHER = 0,
  GT_ERROR_NEEDS_CREDENTIALS, /* HTTPS: git would have asked for a username / password */
  GT_ERROR_AUTH_FAILED,       /* HTTPS: the credentials were refused */
  GT_ERROR_SSH,               /* SSH: key refused, host unknown, … */
} GtErrorKind;

GtErrorKind gt_error_kind (const char *text);

/* A progress line ("Receiving objects:  45% (450/1000)") → its phase ("Receiving objects", caller frees) and
 * percentage; FALSE if it isn't one. */
gboolean gt_progress_parse (const char *line,
                            char      **phase,
                            int        *percent);

/* TRUE for a URL git can clone: http(s)://, ssh://, git://, file:// with a path, or scp-like user@host:path. */
gboolean gt_url_is_valid  (const char *url);
/* The folder name git would make for @url ("nautilus-utils" for …/nautilus-utils.git), or NULL. Caller frees. */
char    *gt_url_dir_name  (const char *url);

G_END_DECLS
