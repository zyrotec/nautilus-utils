/* Tests for src/common/nu_icon_store.c (PLAN.md §12.1): copies by content hash, refuses other types and
 * huge files, async variant. Files live under the build dir (PLAN.md §4.2). */
#include <glib/gstdio.h>

#include "nu_icon_store.h"

static char *run_dir; /* Owned. */

/* Writes @content to @name under run_dir; returns the path (caller frees). */
static char *
write_file (const char *name, const char *content, gssize len)
{
  char *path = g_build_filename (run_dir, name, NULL);
  g_assert_true (g_file_set_contents_full (path, content, len, G_FILE_SET_CONTENTS_NONE, 0644, NULL));
  return path;
}

static void
rm_rf (const char *path)
{
  g_autoptr (GFile) f = g_file_new_for_path (path);
  g_autoptr (GFileEnumerator) e = g_file_enumerate_children (f, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                             G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
  GFileInfo *info;
  while (e != NULL && (info = g_file_enumerator_next_file (e, NULL, NULL)) != NULL)
    {
      g_autoptr (GFileInfo) owned = info;
      g_autofree char *child = g_build_filename (path, g_file_info_get_name (owned), NULL);
      rm_rf (child);
    }
  g_remove (path);
}

static void
test_import (void)
{
  g_autofree char *data = g_build_filename (run_dir, "data", NULL);
  g_autofree char *svg = write_file ("Node Icon.SVG", "<svg xmlns=\"http://www.w3.org/2000/svg\"/>", -1);
  g_autoptr (GFile) src = g_file_new_for_path (svg);
  g_autoptr (GError) error = NULL;
  g_autofree char *stored = nu_icon_store_import (src, data, NULL, &error);
  g_assert_no_error (error);
  g_autofree char *dir = nu_icon_store_dir (data);
  g_assert_true (g_str_has_prefix (stored, dir));
  g_assert_true (g_str_has_suffix (stored, ".svg"));
  g_autofree char *copy = NULL;
  g_assert_true (g_file_get_contents (stored, &copy, NULL, NULL));
  g_assert_cmpstr (copy, ==, "<svg xmlns=\"http://www.w3.org/2000/svg\"/>");

  /* The original can go away: the stored copy stays. Same content again = same stored file. */
  g_assert_cmpint (g_unlink (svg), ==, 0);
  g_assert_true (g_file_test (stored, G_FILE_TEST_IS_REGULAR));
  g_autofree char *svg2 = write_file ("again.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"/>", -1);
  g_autoptr (GFile) src2 = g_file_new_for_path (svg2);
  g_autofree char *stored2 = nu_icon_store_import (src2, data, NULL, NULL);
  g_assert_cmpstr (stored2, ==, stored);

  g_autofree char *png = write_file ("x.png", "\x89PNG", 4);
  g_autoptr (GFile) src3 = g_file_new_for_path (png);
  g_autofree char *stored3 = nu_icon_store_import (src3, data, NULL, NULL);
  g_assert_true (g_str_has_suffix (stored3, ".png"));
  g_assert_cmpstr (stored3, !=, stored);
}

static void
test_refused (void)
{
  g_autofree char *data = g_build_filename (run_dir, "data2", NULL);
  g_autofree char *txt = write_file ("notes.txt", "hello", -1);
  g_autoptr (GFile) a = g_file_new_for_path (txt);
  g_autoptr (GError) e1 = NULL;
  g_assert_null (nu_icon_store_import (a, data, NULL, &e1));
  g_assert_error (e1, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);

  g_autofree char *missing = g_build_filename (run_dir, "missing.svg", NULL);
  g_autoptr (GFile) b = g_file_new_for_path (missing);
  g_autoptr (GError) e2 = NULL;
  g_assert_null (nu_icon_store_import (b, data, NULL, &e2));
  g_assert_error (e2, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);

  gsize big = NU_ICON_STORE_MAX_BYTES + 1;
  g_autofree char *junk = g_malloc0 (big);
  g_autofree char *huge = write_file ("huge.svg", junk, (gssize) big);
  g_autoptr (GFile) c = g_file_new_for_path (huge);
  g_autoptr (GError) e3 = NULL;
  g_assert_null (nu_icon_store_import (c, data, NULL, &e3));
  g_assert_error (e3, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_autofree char *dir = nu_icon_store_dir (data);
  g_assert_false (g_file_test (dir, G_FILE_TEST_EXISTS)); /* nothing stored */
}

static void
on_imported (GObject *source, GAsyncResult *res, gpointer data)
{
  (void) source;
  char **out = data;
  g_autoptr (GError) error = NULL;
  *out = nu_icon_store_import_finish (res, &error);
  g_assert_no_error (error);
}

static void
test_async (void)
{
  g_autofree char *data = g_build_filename (run_dir, "data3", NULL);
  g_autofree char *svg = write_file ("async.svg", "<svg/>", -1);
  g_autoptr (GFile) src = g_file_new_for_path (svg);
  g_autofree char *stored = NULL;
  nu_icon_store_import_async (src, data, NULL, on_imported, &stored);
  while (stored == NULL)
    g_main_context_iteration (NULL, TRUE);
  g_assert_true (g_file_test (stored, G_FILE_TEST_IS_REGULAR));
  g_autofree char *def = nu_icon_store_dir (NULL);
  g_assert_true (g_str_has_suffix (def, "nautilus-utils/folder-icons"));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  run_dir = g_build_filename (g_getenv ("G_TEST_BUILDDIR") != NULL ? g_getenv ("G_TEST_BUILDDIR") : ".",
                              "icon-store-run", NULL);
  rm_rf (run_dir);
  g_assert_cmpint (g_mkdir_with_parents (run_dir, 0700), ==, 0);
  g_test_add_func ("/icon-store/import", test_import);
  g_test_add_func ("/icon-store/refused", test_refused);
  g_test_add_func ("/icon-store/async", test_async);
  int ret = g_test_run ();
  rm_rf (run_dir);
  g_clear_pointer (&run_dir, g_free);
  return ret;
}
