#include "test-util.h"

#include <glib/gstdio.h>

char *
tu_make_tmpdir (const char *prefix)
{
  g_autofree char *tmpl = g_strdup_printf ("%s-XXXXXX", prefix);
  g_autoptr (GError) error = NULL;
  char *dir = g_dir_make_tmp (tmpl, &error);
  g_assert_no_error (error);
  return dir;
}

char *
tu_make_tmpdir_in (const char *parent, const char *prefix)
{
  g_assert_cmpint (g_mkdir_with_parents (parent, 0755), ==, 0);
  g_autofree char *tmpl = g_strdup_printf ("%s-XXXXXX", prefix);
  char *path = g_build_filename (parent, tmpl, NULL);
  g_assert_nonnull (g_mkdtemp (path)); /* fills in the XXXXXX in place */
  return path;
}

void
tu_copy (const char *src_dir, const char *src_name, const char *dst_dir, const char *dst_name)
{
  g_autofree char *src_path = g_build_filename (src_dir, src_name, NULL);
  g_autofree char *dst_path = g_build_filename (dst_dir, dst_name, NULL);
  g_autoptr (GFile) src = g_file_new_for_path (src_path);
  g_autoptr (GFile) dst = g_file_new_for_path (dst_path);
  g_autoptr (GError) error = NULL;
  g_file_copy (src, dst, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &error);
  g_assert_no_error (error);
}

char *
tu_mkdir (const char *parent, const char *name)
{
  char *path = g_build_filename (parent, name, NULL);
  g_assert_cmpint (g_mkdir_with_parents (path, 0755), ==, 0);
  return path;
}

void
tu_touch (const char *dir, const char *name)
{
  g_autofree char *path = g_build_filename (dir, name, NULL);
  g_autoptr (GError) error = NULL;
  g_file_set_contents (path, "x", 1, &error);
  g_assert_no_error (error);
}

void
tu_touch_n (const char *dir, const char *pattern, guint n)
{
  for (guint i = 1; i <= n; i++)
    {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
      g_autofree char *name = g_strdup_printf (pattern, i);
#pragma GCC diagnostic pop
      tu_touch (dir, name);
    }
}

static void
rm_rf_file (GFile *file)
{
  g_autoptr (GFileInfo) info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                                  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
  if (info == NULL)
    return;
  if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
    {
      /* Make sure we can list/delete inside even if a test removed permissions. */
      g_autofree char *path = g_file_get_path (file);
      g_chmod (path, 0755);
      g_autoptr (GFileEnumerator) e = g_file_enumerate_children (file, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
      while (e != NULL)
        {
          GFile *child = NULL; /* Borrowed from the enumerator by g_file_enumerator_iterate. */
          if (!g_file_enumerator_iterate (e, NULL, &child, NULL, NULL) || child == NULL)
            break;
          rm_rf_file (child);
        }
    }
  g_file_delete (file, NULL, NULL);
}

void
tu_rm_rf (const char *path)
{
  g_autoptr (GFile) file = g_file_new_for_path (path);
  rm_rf_file (file);
}

void
tu_wait_for (const guint *flag, guint target)
{
  while (*flag < target)
    g_main_context_iteration (NULL, TRUE);
}
