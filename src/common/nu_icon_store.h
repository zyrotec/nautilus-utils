/* Icon store for "folder icons by name" (PLAN.md §12.1): user-chosen icon files are copied into
 * $XDG_DATA_HOME/nautilus-utils/folder-icons/<sha1 of the content>.<svg|png>, so folders keep their icon when
 * the original file is moved or deleted. Identical files are stored once. Stored files are never deleted
 * automatically (folders not visited since may still point at them). GIO only. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define NU_ICON_STORE_MAX_BYTES (4u * 1024u * 1024u)

/* The store directory (@data_dir NULL = $XDG_DATA_HOME). Caller frees. */
char *nu_icon_store_dir (const char *data_dir);

/* Blocking: copies @source (an .svg or .png file, at most NU_ICON_STORE_MAX_BYTES) into the store under
 * @data_dir (NULL = $XDG_DATA_HOME) and returns the stored file's path (caller frees), or NULL with @error
 * set (G_IO_ERROR_NOT_SUPPORTED for other file types, G_IO_ERROR_FAILED for too large files, or the I/O
 * error). Worker threads or tests only. */
char *nu_icon_store_import (GFile        *source,
                            const char   *data_dir,
                            GCancellable *cancellable,
                            GError      **error);

/* nu_icon_store_import in a GLib worker thread. @callback runs in the caller's main context. */
void nu_icon_store_import_async (GFile               *source,
                                 const char          *data_dir,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data);

/* Caller frees the returned path; NULL with @error set on failure. */
char *nu_icon_store_import_finish (GAsyncResult  *result,
                                   GError       **error);

G_END_DECLS
