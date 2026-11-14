/* See nu_icon_store.h. */
#include "nu_icon_store.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

/* Returns a new string (caller frees). */
char *
nu_icon_store_dir (const char *data_dir)
{
  return g_build_filename (data_dir != NULL ? data_dir : g_get_user_data_dir (), "nautilus-utils", "folder-icons",
                           NULL);
}

/* ".svg" / ".png" for a supported icon file name, else NULL (static string). */
static const char *
icon_suffix (const char *name)
{
  if (name == NULL)
    return NULL;
  g_autofree char *lower = g_ascii_strdown (name, -1);
  if (g_str_has_suffix (lower, ".svg"))
    return ".svg";
  if (g_str_has_suffix (lower, ".png"))
    return ".png";
  return NULL;
}

/* Returns a new path (caller frees) or NULL with @error set. */
char *
nu_icon_store_import (GFile *source, const char *data_dir, GCancellable *cancellable, GError **error)
{
  g_return_val_if_fail (G_IS_FILE (source), NULL);
  g_autofree char *base = g_file_get_basename (source);
  const char *suffix = icon_suffix (base);
  if (suffix == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "%s is not an SVG or PNG file", base);
      return NULL;
    }
  g_autoptr (GFileInfo) info = g_file_query_info (source, G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE,
                                                  cancellable, error);
  if (info == NULL)
    return NULL;
  if (g_file_info_get_size (info) > NU_ICON_STORE_MAX_BYTES)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s is larger than %u bytes", base, NU_ICON_STORE_MAX_BYTES);
      return NULL;
    }
  g_autofree char *data = NULL;
  gsize len = 0;
  if (!g_file_load_contents (source, cancellable, &data, &len, NULL, error))
    return NULL;
  if (len > NU_ICON_STORE_MAX_BYTES)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s is larger than %u bytes", base, NU_ICON_STORE_MAX_BYTES);
      return NULL;
    }

  g_autofree char *dir = nu_icon_store_dir (data_dir);
  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      int saved = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved), "cannot create %s: %s", dir, g_strerror (saved));
      return NULL;
    }
  g_autofree char *hash = g_compute_checksum_for_data (G_CHECKSUM_SHA1, (const guchar *) data, len);
  g_autofree char *name = g_strconcat (hash, suffix, NULL);
  g_autofree char *path = g_build_filename (dir, name, NULL);
  if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
    return g_steal_pointer (&path); /* same content already stored */
  /* Temp file + rename: readers never see a half-written icon (no fsync, PLAN.md §4.4 rule 4). */
  g_autofree char *tmp = g_strdup_printf ("%s.tmp-%08x", path, g_random_int ());
  if (!g_file_set_contents_full (tmp, data, (gssize) len, G_FILE_SET_CONTENTS_NONE, 0644, error))
    {
      g_unlink (tmp);
      return NULL;
    }
  if (g_rename (tmp, path) != 0)
    {
      int saved = errno;
      g_unlink (tmp);
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved), "cannot store %s: %s", path, g_strerror (saved));
      return NULL;
    }
  return g_steal_pointer (&path);
}

typedef struct
{
  GFile *source;   /* Owned. */
  char  *data_dir; /* Owned or NULL. */
} ImportArgs;

static void
import_args_free (ImportArgs *a)
{
  g_clear_object (&a->source);
  g_clear_pointer (&a->data_dir, g_free);
  g_free (a);
}

static void
import_thread (GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
  (void) source_object;
  ImportArgs *a = task_data;
  GError *error = NULL;
  char *path = nu_icon_store_import (a->source, a->data_dir, cancellable, &error); /* owned by the task */
  if (path == NULL)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, path, g_free);
}

void
nu_icon_store_import_async (GFile *source, const char *data_dir, GCancellable *cancellable,
                            GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail (G_IS_FILE (source));
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, nu_icon_store_import_async);
  ImportArgs *a = g_new0 (ImportArgs, 1);
  a->source = g_object_ref (source);
  a->data_dir = g_strdup (data_dir);
  g_task_set_task_data (task, a, (GDestroyNotify) import_args_free);
  g_task_run_in_thread (task, import_thread);
}

/* Returns a new path (caller frees) or NULL. */
char *
nu_icon_store_import_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}
