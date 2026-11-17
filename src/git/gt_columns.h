/* Git columns in Files' list view (PLAN.md §14.2 item 3, §14.5 item 1) through Files' public column and info
 * providers: Git Status, Last Commit, Commit Hash, Commit Author. Shown and hidden with Files' own Visible Columns.
 *
 * Files asks for each file's values; per folder, one `git status` and one bounded `git log --name-only` walk answer
 * every file in it at once (files asked for meanwhile wait). The answers are kept per folder and read again when
 * the repository changes (its git folder: commit, checkout, …) or when a file of the folder is asked for again a
 * little later (Files asks again after a file changes). Only native folders inside a repository start git. */
#pragma once

#include <nautilus-extension.h>

G_BEGIN_DECLS

#define GT_TYPE_COLUMNS (gt_columns_get_type ())
G_DECLARE_FINAL_TYPE (GtColumns, gt_columns, GT, COLUMNS, GObject)

/* Attribute names (the columns' "attribute"). */
#define GT_ATTR_STATUS "nu_git_status"
#define GT_ATTR_COMMIT "nu_git_commit"
#define GT_ATTR_HASH   "nu_git_hash"
#define GT_ATTR_AUTHOR "nu_git_author"

void gt_columns_register (GTypeModule *module);

/* Feature on/off: off answers nothing (the columns stay listed but empty) and drops what was read. */
void gt_columns_set_enabled (gboolean enabled);

/* Drops everything (shutdown). */
void gt_columns_shutdown (void);

G_END_DECLS
