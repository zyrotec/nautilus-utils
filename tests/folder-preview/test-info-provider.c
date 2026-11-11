/* Unit tests for src/info_provider.c, linked statically (no Nautilus process needed). */
#include <nautilus-extension.h>

#include "fake-file-info.h"
#include "info_provider.h"

/* ---- A GTypeModule that just registers our dynamic type (Nautilus' real one also dlopens). ---- */

#define TEST_TYPE_MODULE (test_module_get_type ())
G_DECLARE_FINAL_TYPE (TestModule, test_module, TEST, MODULE, GTypeModule)
struct _TestModule { GTypeModule parent_instance; };
G_DEFINE_FINAL_TYPE (TestModule, test_module, G_TYPE_TYPE_MODULE)

static gboolean
test_module_load (GTypeModule *module)
{
  fp_info_provider_register (module);
  return TRUE;
}

static void test_module_unload (GTypeModule *module) { (void) module; }
static void test_module_init (TestModule *self) { (void) self; }

static void
test_module_class_init (TestModuleClass *klass)
{
  G_TYPE_MODULE_CLASS (klass)->load = test_module_load;
  G_TYPE_MODULE_CLASS (klass)->unload = test_module_unload;
}

/* ---- Capture debug messages from our log domain (g_debug goes through g_log, so a domain handler sees them). ---- */

static GPtrArray *captured; /* Owned strings (g_free); created in main, freed at end of main. */

static void
capture_handler (const char     *domain,
                 GLogLevelFlags  level,
                 const char     *message,
                 gpointer        user_data)
{
  (void) domain;
  (void) level;
  (void) user_data;
  g_ptr_array_add (captured, g_strdup (message));
}

/* Returns a new provider the caller unrefs. */
static NautilusInfoProvider *
new_provider (void)
{
  return NAUTILUS_INFO_PROVIDER (g_object_new (FP_TYPE_INFO_PROVIDER, NULL));
}

static void
test_should_track (void)
{
  struct { const char *uri; gboolean dir; gboolean expected; } cases[] = {
    { "file:///tmp",                    TRUE,  TRUE  },
    { "file:///tmp/a%20b/c",            TRUE,  TRUE  },
    { "file:///tmp/notes.txt",          FALSE, FALSE },
    { "sftp://example.invalid/folder",  TRUE,  FALSE },
    { "smb://server/share",             TRUE,  FALSE },
    { "mtp://phone/DCIM",               TRUE,  FALSE },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autoptr (FakeFileInfo) f = fake_file_info_new (cases[i].uri, cases[i].dir);
      g_test_message ("%s dir=%d", cases[i].uri, cases[i].dir);
      g_assert_cmpint (fp_info_provider_should_track (NAUTILUS_FILE_INFO (f)), ==, cases[i].expected);
    }
}

static void
test_update_logs_directories_only (void)
{
  g_autoptr (NautilusInfoProvider) provider = new_provider ();
  g_autoptr (FakeFileInfo) dir = fake_file_info_new ("file:///tmp/photos", TRUE);
  g_autoptr (FakeFileInfo) file = fake_file_info_new ("file:///tmp/photos/a.png", FALSE);
  g_autoptr (FakeFileInfo) remote = fake_file_info_new ("sftp://example.invalid/x", TRUE);

  guint n_invoked = 0;
  GClosure *done = fake_update_closure_new (&n_invoked);
  g_ptr_array_set_size (captured, 0);
  NautilusOperationHandle *handle = NULL;
  g_assert_cmpint (nautilus_info_provider_update_file_info (provider, NAUTILUS_FILE_INFO (dir), done, &handle),
                   ==, NAUTILUS_OPERATION_COMPLETE);
  g_assert_cmpint (nautilus_info_provider_update_file_info (provider, NAUTILUS_FILE_INFO (file), done, &handle),
                   ==, NAUTILUS_OPERATION_COMPLETE);
  g_assert_cmpint (nautilus_info_provider_update_file_info (provider, NAUTILUS_FILE_INFO (remote), done, &handle),
                   ==, NAUTILUS_OPERATION_COMPLETE);
  g_assert_null (handle);
  /* Synchronous completion: the closure must never be invoked. */
  g_assert_cmpuint (n_invoked, ==, 0);
  g_closure_unref (done);

  g_assert_cmpuint (captured->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (captured, 0), ==, "folder: file:///tmp/photos");
}

/* Repetition makes leaks visible to ASan/LSan and valgrind (PLAN.md §4.2). */
static void
test_stress (void)
{
  guint n_invoked = 0;
  GClosure *done = fake_update_closure_new (&n_invoked);
  for (int round = 0; round < 50; round++)
    {
      g_autoptr (NautilusInfoProvider) provider = new_provider ();
      for (int i = 0; i < 400; i++)
        {
          g_autofree char *uri = g_strdup_printf ("file:///tmp/stress/%d", i);
          g_autoptr (FakeFileInfo) f = fake_file_info_new (uri, i % 3 != 0);
          NautilusOperationHandle *handle = NULL;
          g_assert_cmpint (nautilus_info_provider_update_file_info (provider, NAUTILUS_FILE_INFO (f), done, &handle),
                           ==, NAUTILUS_OPERATION_COMPLETE);
        }
    }
  g_assert_cmpuint (n_invoked, ==, 0);
  g_closure_unref (done);
  g_ptr_array_set_size (captured, 0);
}

int
main (int argc, char **argv)
{
  captured = g_ptr_array_new_with_free_func (g_free);
  g_test_init (&argc, &argv, NULL);
  g_log_set_handler ("folder-preview", G_LOG_LEVEL_DEBUG, capture_handler, NULL);

  /* Kept for the whole process: GLib never lets a GTypeModule with registered types be finalized. */
  GTypeModule *module = g_object_new (TEST_TYPE_MODULE, NULL);
  g_assert_true (g_type_module_use (module));

  g_test_add_func ("/info-provider/should-track", test_should_track);
  g_test_add_func ("/info-provider/update-logs-directories-only", test_update_logs_directories_only);
  g_test_add_func ("/info-provider/stress", test_stress);
  int ret = g_test_run ();

  g_clear_pointer (&captured, g_ptr_array_unref);
  return ret;
}
