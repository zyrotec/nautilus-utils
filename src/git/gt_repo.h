/* Git: finding the repository a folder belongs to (PLAN.md §14.2 item 1, §14.5 item 6). No processes: Files asks
 * about every folder it shows, so this only looks for ".git" with stat(), walking up. No GTK. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* The working tree root that native folder @path is in, or NULL if it isn't in one. A ".git" folder or a ".git"
 * file ("gitdir: …", worktrees and submodules) counts; the walk stops at a filesystem boundary (as git does by
 * default) and at the root. *@git_dir (optional) receives the repository's git folder (absolute). Callers free.
 * Blocking (stat only): native paths. */
char *gt_repo_find_root (const char *path,
                         char      **git_dir);

/* @path relative to @root ('/'-separated, "" for the root itself), or NULL if it isn't inside. Caller frees. */
char *gt_repo_relative (const char *root,
                        const char *path);

G_END_DECLS
