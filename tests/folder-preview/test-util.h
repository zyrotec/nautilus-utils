/* Small filesystem helpers shared by tests. All paths are UTF-8/native local paths. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* Creates a fresh temp dir. Caller owns the returned path (g_free) and should tu_rm_rf() it. */
char *tu_make_tmpdir (const char *prefix);

/* Creates a fresh dir @parent/@prefix-XXXXXX. Caller owns the returned path (g_free). Use this instead of
 * tu_make_tmpdir when thumbnailers will run: gnome-desktop's sandbox misbehaves for files and caches
 * under /tmp (PLAN.md §4.2). */
char *tu_make_tmpdir_in (const char *parent, const char *prefix);

/* Copies @src_dir/@src_name to @dst_dir/@dst_name (overwriting). */
void tu_copy (const char *src_dir, const char *src_name, const char *dst_dir, const char *dst_name);

/* Creates @parent/@name as a folder (parents too) and returns its path; caller frees. */
char *tu_mkdir (const char *parent, const char *name);

/* Writes a small non-empty file @dir/@name (non-empty so fast content type isn't x-zerosize). */
void tu_touch (const char *dir, const char *name);

/* Writes @n files named printf(@pattern, i) for i in 1..@n into @dir. */
void tu_touch_n (const char *dir, const char *pattern, guint n);

/* Deletes @path recursively without following symlinks. Missing path is fine. */
void tu_rm_rf (const char *path);

/* Iterates the default main context until *@flag >= @target. */
void tu_wait_for (const guint *flag, guint target);

G_END_DECLS
