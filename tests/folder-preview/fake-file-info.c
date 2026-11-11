#include "fake-file-info.h"

struct _FakeFileInfo
{
  GObject parent_instance;
  GFile *location;   /* Owned; released in finalize. */
  gboolean is_directory;
};

static void fake_file_info_iface_init (NautilusFileInfoInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (FakeFileInfo, fake_file_info, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (NAUTILUS_TYPE_FILE_INFO, fake_file_info_iface_init))

/* Returns a new string the caller frees. */
static char *
fake_get_uri (NautilusFileInfo *info)
{
  return g_file_get_uri (FAKE_FILE_INFO (info)->location);
}

/* Returns a new string the caller frees. */
static char *
fake_get_uri_scheme (NautilusFileInfo *info)
{
  return g_file_get_uri_scheme (FAKE_FILE_INFO (info)->location);
}

/* Returns a new string the caller frees. */
static char *
fake_get_name (NautilusFileInfo *info)
{
  return g_file_get_basename (FAKE_FILE_INFO (info)->location);
}

static gboolean
fake_is_directory (NautilusFileInfo *info)
{
  return FAKE_FILE_INFO (info)->is_directory;
}

static GFileType
fake_get_file_type (NautilusFileInfo *info)
{
  return FAKE_FILE_INFO (info)->is_directory ? G_FILE_TYPE_DIRECTORY : G_FILE_TYPE_REGULAR;
}

/* Returns a new reference the caller unrefs. */
static GFile *
fake_get_location (NautilusFileInfo *info)
{
  return g_object_ref (FAKE_FILE_INFO (info)->location);
}

static gboolean
fake_is_gone (NautilusFileInfo *info)
{
  (void) info;
  return FALSE;
}

static void
fake_file_info_iface_init (NautilusFileInfoInterface *iface)
{
  iface->is_gone = fake_is_gone;
  iface->get_name = fake_get_name;
  iface->get_uri = fake_get_uri;
  iface->get_uri_scheme = fake_get_uri_scheme;
  iface->is_directory = fake_is_directory;
  iface->get_file_type = fake_get_file_type;
  iface->get_location = fake_get_location;
}

static void
fake_file_info_finalize (GObject *object)
{
  g_clear_object (&FAKE_FILE_INFO (object)->location);
  G_OBJECT_CLASS (fake_file_info_parent_class)->finalize (object);
}

static void
fake_file_info_class_init (FakeFileInfoClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = fake_file_info_finalize;
}

static void
fake_file_info_init (FakeFileInfo *self)
{
  (void) self;
}

FakeFileInfo *
fake_file_info_new (const char *uri,
                    gboolean    is_directory)
{
  FakeFileInfo *self = g_object_new (FAKE_TYPE_FILE_INFO, NULL);
  self->location = g_file_new_for_uri (uri);
  self->is_directory = is_directory;
  return self;
}

static void
count_invocation (gpointer data)
{
  guint *n_invoked = data;
  (*n_invoked)++;
}

GClosure *
fake_update_closure_new (guint *n_invoked)
{
  GClosure *closure = g_cclosure_new_swap (G_CALLBACK (count_invocation), n_invoked, NULL);
  g_closure_ref (closure);
  g_closure_sink (closure);
  g_closure_set_marshal (closure, g_cclosure_marshal_generic);
  return closure;
}
