#include "scanner.h"

#include <string.h>

#include "config.h"

/* Queried only for the files we keep (follows symlinks). */
#define CANDIDATE_ATTRIBUTES "standard::type,time::modified,thumbnail::path,thumbnail::is-valid,thumbnail::failed"

/* ------------------------------------------------------------------------------------------------ */
/* Result types                                                                                     */

void
fp_candidate_free (FpCandidate *candidate)
{
  if (candidate == NULL)
    return;
  g_clear_object (&candidate->file);
  g_clear_pointer (&candidate->name, g_free);
  g_clear_pointer (&candidate->display_name, g_free);
  g_clear_pointer (&candidate->content_type, g_free);
  g_clear_pointer (&candidate->collate_key, g_free);
  g_clear_pointer (&candidate->thumbnail_path, g_free);
  g_free (candidate);
}

/* Called by g_atomic_rc_box_release_full when the last reference goes; frees the contents
 * (the box itself is freed by GLib). */
static void
scan_result_clear (gpointer data)
{
  FpScanResult *result = data;
  g_clear_object (&result->folder);
  g_clear_pointer (&result->candidates, g_ptr_array_unref);
}

FpScanResult *
fp_scan_result_ref (FpScanResult *result)
{
  g_return_val_if_fail (result != NULL, NULL);
  return g_atomic_rc_box_acquire (result);
}

void
fp_scan_result_unref (FpScanResult *result)
{
  if (result == NULL)
    return;
  g_atomic_rc_box_release_full (result, scan_result_clear);
}

/* ------------------------------------------------------------------------------------------------ */
/* Classification                                                                                   */

FpMediaKind
fp_scanner_classify (GFileInfo *info)
{
  g_return_val_if_fail (G_IS_FILE_INFO (info), FP_MEDIA_NONE);

  /* The attribute getters (not g_file_info_get_is_hidden etc.) return FALSE/0/NULL for attributes that
   * are absent instead of emitting criticals. */
  if (g_file_info_get_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN) ||
      g_file_info_get_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP))
    return FP_MEDIA_NONE;

  /* Symlinks are followed when enumerating, so a link to a regular file counts; directories, broken
   * links and special files don't. */
  if (g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_STANDARD_TYPE) != G_FILE_TYPE_REGULAR)
    return FP_MEDIA_NONE;

  /* Borrowed string owned by @info. */
  const char *type = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);
  if (type == NULL)
    return FP_MEDIA_NONE;
  if (g_str_has_prefix (type, "image/"))
    return FP_MEDIA_IMAGE;
  if (g_str_has_prefix (type, "video/"))
    return FP_MEDIA_VIDEO;
  if (g_str_has_prefix (type, "audio/"))
    return FP_MEDIA_AUDIO;
  return FP_MEDIA_NONE;
}

/* ------------------------------------------------------------------------------------------------ */
/* Bounded top-N collection                                                                         */

typedef struct
{
  char      *key;   /* Owned collate key. */
  GFileInfo *info;  /* Owned ref. */
} Entry;

static void
entry_free (Entry *entry)
{
  g_clear_pointer (&entry->key, g_free);
  g_clear_object (&entry->info);
  g_free (entry);
}

/* Natural order, ties broken by raw file name so the order is total and deterministic. */
static int
entry_compare (const char *key_a, GFileInfo *info_a, const char *key_b, GFileInfo *info_b)
{
  int c = strcmp (key_a, key_b);
  if (c != 0)
    return c;
  return g_strcmp0 (g_file_info_get_attribute_byte_string (info_a, G_FILE_ATTRIBUTE_STANDARD_NAME),
                    g_file_info_get_attribute_byte_string (info_b, G_FILE_ATTRIBUTE_STANDARD_NAME));
}

/* Borrowed string owned by @info: display name, falling back to the raw name. */
static const char *
display_name_of (GFileInfo *info)
{
  const char *dn = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME);
  if (dn == NULL)
    dn = g_file_info_get_attribute_byte_string (info, G_FILE_ATTRIBUTE_STANDARD_NAME);
  return dn != NULL ? dn : "";
}

/* Inserts into the sorted @entries, keeping at most @bound items (the smallest).
 * Takes ownership of @key; refs @info only if it is kept. */
static void
bounded_insert (GPtrArray *entries, guint bound, char *key, GFileInfo *info)
{
  g_autofree char *owned_key = key;

  if (entries->len == bound)
    {
      Entry *last = g_ptr_array_index (entries, entries->len - 1);
      if (entry_compare (owned_key, info, last->key, last->info) >= 0)
        return; /* Not among the first @bound; owned_key is freed. */
    }

  /* Binary search for the first entry greater than the new one. */
  guint lo = 0, hi = entries->len;
  while (lo < hi)
    {
      guint mid = lo + (hi - lo) / 2;
      Entry *e = g_ptr_array_index (entries, mid);
      if (entry_compare (e->key, e->info, owned_key, info) <= 0)
        lo = mid + 1;
      else
        hi = mid;
    }

  Entry *entry = g_new0 (Entry, 1);
  entry->key = g_steal_pointer (&owned_key);
  entry->info = g_object_ref (info);
  g_ptr_array_insert (entries, (gint) lo, entry);
  if (entries->len > bound)
    g_ptr_array_remove_index (entries, entries->len - 1); /* entry_free via the array's free func */
}

/* Names listed in @folder's ".hidden" file (GLib's convention for hiding files). Returns a new set the
 * caller owns, or NULL if there is no such file. */
static GHashTable *
read_hidden_list (GFile *folder)
{
  g_autoptr (GFile) list = g_file_get_child (folder, ".hidden");
  g_autofree char *path = g_file_get_path (list);
  g_autofree char *contents = NULL;
  if (path == NULL || !g_file_get_contents (path, &contents, NULL, NULL))
    return NULL;
  GHashTable *set = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_auto (GStrv) lines = g_strsplit (contents, "\n", -1);
  for (guint i = 0; lines[i] != NULL; i++)
    if (lines[i][0] != '\0')
      g_hash_table_add (set, g_strdup (lines[i]));
  return set;
}

/* Fills in the name-derived attributes fp_scanner_classify needs, the way GLib would but without a
 * stat(): is-hidden (dot file or listed in .hidden), is-backup (trailing '~'), fast-content-type (guessed
 * from the name only, which is what GLib's "fast" type is). @hidden may be NULL. */
static void
set_name_attributes (GFileInfo *info, GHashTable *hidden)
{
  const char *name = g_file_info_get_attribute_byte_string (info, G_FILE_ATTRIBUTE_STANDARD_NAME);
  if (name == NULL)
    return;
  gboolean is_hidden = name[0] == '.' || (hidden != NULL && g_hash_table_contains (hidden, name));
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN, is_hidden);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP, g_str_has_suffix (name, "~"));
  gboolean uncertain = FALSE;
  g_autofree char *type = g_content_type_guess (name, NULL, 0, &uncertain);
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE, type);
}

/* Enumerates @folder and returns (transfer full) the first @bound eligible children in natural order.
 * *n_eligible receives the count of all eligible children. NULL + @error on failure. */
static GPtrArray *
collect_entries (GFile         *folder,
                 guint          bound,
                 guint         *n_eligible,
                 GCancellable  *cancellable,
                 GError       **error)
{
  g_autoptr (GFileEnumerator) enumerator =
    g_file_enumerate_children (folder, FP_SCAN_ENUM_ATTRIBUTES, G_FILE_QUERY_INFO_NONE, cancellable, error);
  if (enumerator == NULL)
    return NULL;

  g_autoptr (GPtrArray) entries = g_ptr_array_new_with_free_func ((GDestroyNotify) entry_free);
  g_autoptr (GHashTable) hidden = read_hidden_list (folder);
  *n_eligible = 0;

  for (guint i = 1;; i++)
    {
      /* Cancellation point every ENUM_BATCH entries (next_file also checks it inside GIO). */
      if (i % ENUM_BATCH == 0 && g_cancellable_set_error_if_cancelled (cancellable, error))
        return NULL;

      g_autoptr (GError) local_error = NULL;
      g_autoptr (GFileInfo) info = g_file_enumerator_next_file (enumerator, cancellable, &local_error);
      if (info == NULL)
        {
          if (local_error != NULL)
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return NULL;
            }
          break; /* End of directory. */
        }

      set_name_attributes (info, hidden);
      if (fp_scanner_classify (info) == FP_MEDIA_NONE)
        continue;

      (*n_eligible)++;
      bounded_insert (entries, bound, g_utf8_collate_key_for_filename (display_name_of (info), -1), info);
    }

  /* Closing a local enumerator doesn't fail in practice; a failure here loses nothing we need. */
  g_file_enumerator_close (enumerator, NULL, NULL);
  return g_steal_pointer (&entries);
}

/* Builds a candidate for @entry, or returns NULL if it must be skipped (cached thumbnail failure, or
 * the file vanished). Caller owns the result. */
static FpCandidate *
make_candidate (GFile *folder, Entry *entry, GCancellable *cancellable)
{
  const char *name = g_file_info_get_attribute_byte_string (entry->info, G_FILE_ATTRIBUTE_STANDARD_NAME);
  if (name == NULL)
    return NULL;

  g_autoptr (GFile) child = g_file_get_child (folder, name);
  /* Type, mtime and thumbnail info only for kept files: much cheaper than requesting them for every
   * child. */
  g_autoptr (GFileInfo) thumb = g_file_query_info (child, CANDIDATE_ATTRIBUTES, G_FILE_QUERY_INFO_NONE, cancellable, NULL);
  if (thumb == NULL)
    return NULL; /* Deleted since enumeration, or cancelled (the caller checks cancellation). */
  if (g_file_info_get_attribute_uint32 (thumb, G_FILE_ATTRIBUTE_STANDARD_TYPE) != G_FILE_TYPE_REGULAR)
    return NULL; /* Replaced by something else since enumeration. */

  gboolean valid = g_file_info_get_attribute_boolean (thumb, G_FILE_ATTRIBUTE_THUMBNAIL_IS_VALID);
  if (valid && g_file_info_get_attribute_boolean (thumb, G_FILE_ATTRIBUTE_THUMBNAILING_FAILED))
    return NULL; /* A still-valid failure record: e.g. audio without cover art. */

  FpCandidate *c = g_new0 (FpCandidate, 1);
  c->file = g_steal_pointer (&child);
  c->name = g_strdup (name);
  c->display_name = g_strdup (display_name_of (entry->info));
  c->content_type = g_strdup (g_file_info_get_attribute_string (entry->info,
                                                                G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE));
  c->collate_key = g_strdup (entry->key);
  c->mtime = g_file_info_get_attribute_uint64 (thumb, G_FILE_ATTRIBUTE_TIME_MODIFIED);
  c->kind = fp_scanner_classify (entry->info);
  const char *thumb_path = g_file_info_get_attribute_byte_string (thumb, G_FILE_ATTRIBUTE_THUMBNAIL_PATH);
  if (valid && thumb_path != NULL)
    c->thumbnail_path = g_strdup (thumb_path);
  return c;
}

FpScanResult *
fp_scan_folder (GFile         *folder,
                guint          window,
                GCancellable  *cancellable,
                GError       **error)
{
  g_return_val_if_fail (G_IS_FILE (folder), NULL);
  g_return_val_if_fail (window > 0, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!g_file_is_native (folder))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Only local folders are scanned");
      return NULL;
    }

  g_autoptr (GFileInfo) folder_info =
    g_file_query_info (folder, "standard::type,time::modified,time::modified-usec",
                       G_FILE_QUERY_INFO_NONE, cancellable, error);
  if (folder_info == NULL)
    return NULL;
  if (g_file_info_get_attribute_uint32 (folder_info, G_FILE_ATTRIBUTE_STANDARD_TYPE) != G_FILE_TYPE_DIRECTORY)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY, "Not a folder");
      return NULL;
    }

  /* Start with a bound equal to the window. If cached failures leave us short while more eligible files
   * exist beyond the bound, enumerate again with a 4× larger bound (rare: needs > window failures). */
  guint bound = window;
  for (;;)
    {
      guint n_eligible = 0;
      g_autoptr (GPtrArray) entries = collect_entries (folder, bound, &n_eligible, cancellable, error);
      if (entries == NULL)
        return NULL;

      g_autoptr (GPtrArray) candidates = g_ptr_array_new_with_free_func ((GDestroyNotify) fp_candidate_free);
      guint examined = 0;
      for (; examined < entries->len && candidates->len < window; examined++)
        {
          FpCandidate *c = make_candidate (folder, g_ptr_array_index (entries, examined), cancellable);
          if (c != NULL)
            g_ptr_array_add (candidates, c);
        }
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        return NULL;

      gboolean short_but_more_exist = candidates->len < window && n_eligible > bound;
      if (!short_but_more_exist || bound >= G_MAXUINT / 4)
        {
          FpScanResult *result = g_atomic_rc_box_new0 (FpScanResult);
          result->folder = g_object_ref (folder);
          result->mtime_sec = g_file_info_get_attribute_uint64 (folder_info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
          result->mtime_usec = g_file_info_get_attribute_uint32 (folder_info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
          result->candidates = g_steal_pointer (&candidates);
          result->n_eligible = n_eligible;
          result->more_available = n_eligible > examined;
          return result;
        }
      bound *= 4;
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Async front end                                                                                  */

typedef struct
{
  GFile *folder;  /* Owned ref. */
  guint  window;
} ScanArgs;

static void
scan_args_free (ScanArgs *args)
{
  g_clear_object (&args->folder);
  g_free (args);
}

/* FpWorkFunc: runs in a pool thread, returns @task exactly once. */
static void
scan_work (GTask *task, gpointer task_data, GCancellable *cancellable)
{
  ScanArgs *args = task_data;
  g_autoptr (GError) error = NULL;
  FpScanResult *result = fp_scan_folder (args->folder, args->window, cancellable, &error);
  if (result != NULL)
    g_task_return_pointer (task, result, (GDestroyNotify) fp_scan_result_unref); /* task owns @result */
  else
    g_task_return_error (task, g_steal_pointer (&error));
}

void
fp_scan_async (FpWorkers           *workers,
               GFile               *folder,
               guint                window,
               int                  priority,
               GCancellable        *cancellable,
               GAsyncReadyCallback  callback,
               gpointer             user_data)
{
  g_return_if_fail (workers != NULL);
  g_return_if_fail (G_IS_FILE (folder));
  g_return_if_fail (window > 0);
  g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, fp_scan_async);
  ScanArgs *args = g_new0 (ScanArgs, 1);
  args->folder = g_object_ref (folder);
  args->window = window;
  g_task_set_task_data (task, args, (GDestroyNotify) scan_args_free); /* task owns @args */
  fp_workers_push (workers, task, priority, scan_work);                /* pool takes its own task ref */
}

FpScanResult *
fp_scan_finish (GAsyncResult  *result,
                GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
  g_return_val_if_fail (g_task_get_source_tag (G_TASK (result)) == fp_scan_async, NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}
