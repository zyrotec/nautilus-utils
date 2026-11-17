/* Git: what the status bar shows about a repository (PLAN.md §14.2 item 2), shared by every window showing it.
 * Read in the background (`git status --branch` without untracked files, `git log -1`) and again whenever its git
 * folder changes (HEAD, index, refs, packed-refs, FETCH_HEAD: commits, checkouts, fetches, made here or in a
 * terminal). No GTK. Main thread. */
#pragma once

#include <gio/gio.h>

#include "gt_parse.h"

G_BEGIN_DECLS

#define GT_TYPE_INFO (gt_info_get_type ())
G_DECLARE_FINAL_TYPE (GtInfo, gt_info, GT, INFO, GObject)

/* The info for the repository at @root (its git folder @git_dir), shared: a new reference. Starts a read if
 * nothing has been read yet. */
GtInfo *gt_info_get (const char *root,
                     const char *git_dir);

/* The existing info for @root (a new reference), or NULL. */
GtInfo *gt_info_lookup (const char *root);

/* Reads again (coalesced with a read already running). */
void gt_info_refresh (GtInfo *self);

const char *gt_info_get_root     (GtInfo *self);
/* Folder name of the root ("nautilus-utils"). */
const char *gt_info_get_name     (GtInfo *self);
gboolean    gt_info_get_loaded   (GtInfo *self);  /* a read has completed */
const char *gt_info_get_branch   (GtInfo *self);  /* NULL when detached or not loaded */
const char *gt_info_get_oid      (GtInfo *self);  /* HEAD, NULL before the first commit */
const char *gt_info_get_upstream (GtInfo *self);  /* NULL: none */
gboolean    gt_info_get_counts   (GtInfo *self,
                                  int    *ahead,
                                  int    *behind); /* FALSE: no upstream (or it's gone) */
GtCommit   *gt_info_get_last     (GtInfo *self);  /* borrowed, NULL before the first commit */
const char *gt_info_get_error    (GtInfo *self);  /* why the last read failed, or NULL */

/* TRUE while a read runs. */
gboolean gt_info_is_reading (GtInfo *self);

/* Signal "changed": a read completed. Signal "reading-changed": a read started or ended (spinners). */

/* For tests: reads completed so far. */
guint gt_info_get_n_reads (GtInfo *self);

G_END_DECLS
