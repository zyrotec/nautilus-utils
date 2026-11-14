/* Loads the real libnautilus-utils.so (folder-preview is its first feature) through GModule the way Nautilus does, and exercises the
 * entry points and repeated use/unuse sessions. (GLib keeps the module loaded once a class exists, so
 * unload never actually happens — matching Nautilus, which never unloads extensions.) Path comes from $FP_MODULE_PATH. */
#include <gmodule.h>
#include <nautilus-extension.h>

#include "fake-file-info.h"

typedef void (*InitFunc) (GTypeModule *module);
typedef void (*ShutdownFunc) (void);
typedef void (*ListTypesFunc) (const GType **types, int *num_types);

#define TEST_TYPE_LOADER (test_loader_get_type ())
G_DECLARE_FINAL_TYPE (TestLoader, test_loader, TEST, LOADER, GTypeModule)

struct _TestLoader
{
  GTypeModule parent_instance;
  GModule *library;       /* Owned while loaded; closed in unload. */
  ShutdownFunc shutdown;  /* Borrowed symbol from library; valid only while loaded. */
  ListTypesFunc list_types;
  guint n_loads;
  guint n_unloads;
};

G_DEFINE_FINAL_TYPE (TestLoader, test_loader, G_TYPE_TYPE_MODULE)

static gboolean
test_loader_load (GTypeModule *module)
{
  TestLoader *self = TEST_LOADER (module);
  const char *path = g_getenv ("FP_MODULE_PATH");
  g_assert_nonnull (path);

  self->library = g_module_open (path, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
  if (self->library == NULL)
    g_error ("g_module_open(%s): %s", path, g_module_error ());

  InitFunc init = NULL;
  g_assert_true (g_module_symbol (self->library, "nautilus_module_initialize", (gpointer *) &init));
  g_assert_true (g_module_symbol (self->library, "nautilus_module_shutdown", (gpointer *) &self->shutdown));
  g_assert_true (g_module_symbol (self->library, "nautilus_module_list_types", (gpointer *) &self->list_types));

  init (module);
  self->n_loads++;
  return TRUE;
}

static void
test_loader_unload (GTypeModule *module)
{
  TestLoader *self = TEST_LOADER (module);
  self->shutdown ();
  self->shutdown = NULL;
  self->list_types = NULL;
  g_clear_pointer (&self->library, g_module_close);
  self->n_unloads++;
}

static void test_loader_init (TestLoader *self) { (void) self; }

static void
test_loader_class_init (TestLoaderClass *klass)
{
  G_TYPE_MODULE_CLASS (klass)->load = test_loader_load;
  G_TYPE_MODULE_CLASS (klass)->unload = test_loader_unload;
}

/* Kept for the whole process (a GTypeModule with registered types may never be finalized). */
static TestLoader *loader;

/* One "Nautilus session": use the module, create every provider, call it, drop everything. */
static void
exercise_once (void)
{
  g_assert_true (g_type_module_use (G_TYPE_MODULE (loader)));

  const GType *types = NULL;
  int n_types = 0;
  loader->list_types (&types, &n_types);
  g_assert_cmpint (n_types, ==, 2); /* folder preview + folder icons info providers */

  g_autoptr (GPtrArray) objects = g_ptr_array_new_with_free_func (g_object_unref);
  for (int i = 0; i < n_types; i++)
    g_ptr_array_add (objects, g_object_new (types[i], NULL));

  /* Nautilus drops its use right after creating the objects; the live instances keep the module loaded. */
  g_type_module_unuse (G_TYPE_MODULE (loader));

  for (guint i = 0; i < objects->len; i++)
    {
      GObject *obj = g_ptr_array_index (objects, i);
      g_assert_true (NAUTILUS_IS_INFO_PROVIDER (obj));
      g_autoptr (FakeFileInfo) dir = fake_file_info_new ("file:///tmp/module-test", TRUE);
      NautilusOperationHandle *handle = NULL;
      guint n_invoked = 0;
      GClosure *done = fake_update_closure_new (&n_invoked);
      g_assert_cmpint (nautilus_info_provider_update_file_info (NAUTILUS_INFO_PROVIDER (obj),
                                                                NAUTILUS_FILE_INFO (dir), done, &handle),
                       ==, NAUTILUS_OPERATION_COMPLETE);
      g_assert_cmpuint (n_invoked, ==, 0);
      g_closure_unref (done);

      /* Nautilus only cancels handles we handed out, and we never hand any out; call the vfunc directly
       * with a dummy handle to prove it is a harmless no-op. */
      NautilusOperationHandle *dummy = (NautilusOperationHandle *) &n_invoked;
      NAUTILUS_INFO_PROVIDER_GET_IFACE (obj)->cancel_update (NAUTILUS_INFO_PROVIDER (obj), dummy);
    }
}

static void
test_load_and_call (void)
{
  exercise_once ();
  g_assert_cmpuint (loader->n_loads, >=, 1);
}

static void
test_repeated_sessions (void)
{
  for (int i = 0; i < 20; i++)
    exercise_once ();
  g_test_message ("loads=%u unloads=%u", loader->n_loads, loader->n_unloads);
}

/* Must run last: after shutdown the module may not be used again. Later phases free all global state
 * in shutdown, so running it under ASan/valgrind matters. */
static void
test_shutdown (void)
{
  g_assert_nonnull (loader->shutdown);
  loader->shutdown ();
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  loader = g_object_new (TEST_TYPE_LOADER, NULL);
  g_test_add_func ("/module/load-and-call", test_load_and_call);
  g_test_add_func ("/module/repeated-sessions", test_repeated_sessions);
  g_test_add_func ("/module/shutdown", test_shutdown);
  return g_test_run ();
}
