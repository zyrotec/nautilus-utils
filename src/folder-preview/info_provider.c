#include "info_provider.h"

struct _FpInfoProvider
{
  GObject parent_instance;
};

static void fp_info_provider_iface_init (NautilusInfoProviderInterface *iface);

static FpFolderFunc folder_func; /* main thread only */

void
fp_info_provider_set_folder_func (FpFolderFunc func)
{
  folder_func = func;
}

G_DEFINE_DYNAMIC_TYPE_EXTENDED (FpInfoProvider, fp_info_provider, G_TYPE_OBJECT, G_TYPE_FLAG_FINAL,
                                G_IMPLEMENT_INTERFACE_DYNAMIC (NAUTILUS_TYPE_INFO_PROVIDER,
                                                               fp_info_provider_iface_init))

gboolean
fp_info_provider_should_track (NautilusFileInfo *file)
{
  g_return_val_if_fail (NAUTILUS_IS_FILE_INFO (file), FALSE);

  if (!nautilus_file_info_is_directory (file))
    return FALSE;

  /* We own the returned GFile; g_autoptr unrefs it. g_file_is_native() does no I/O. */
  g_autoptr (GFile) location = nautilus_file_info_get_location (file);
  return location != NULL && g_file_is_native (location);
}

static NautilusOperationResult
fp_info_provider_update_file_info (NautilusInfoProvider     *provider,
                                   NautilusFileInfo         *file,
                                   GClosure                 *update_complete,
                                   NautilusOperationHandle **handle)
{
  (void) provider;
  (void) update_complete; /* Not kept: we always complete synchronously, so Nautilus never needs it. */
  (void) handle;          /* Left untouched: only meaningful when returning IN_PROGRESS. */

  if (fp_info_provider_should_track (file))
    {
      /* We own the URI string; g_autofree frees it. */
      g_autofree char *uri = nautilus_file_info_get_uri (file);
      g_debug ("folder: %s", uri);
      if (folder_func != NULL)
        folder_func (uri);
    }

  /* Cheap and non-blocking: no I/O happens here (PLAN.md §6.2 rule 1). */
  return NAUTILUS_OPERATION_COMPLETE;
}

static void
fp_info_provider_cancel_update (NautilusInfoProvider    *provider,
                                NautilusOperationHandle *handle)
{
  /* Nothing to cancel: update_file_info never returns IN_PROGRESS, so Nautilus never holds a handle of ours. */
  (void) provider;
  (void) handle;
}

static void
fp_info_provider_iface_init (NautilusInfoProviderInterface *iface)
{
  iface->update_file_info = fp_info_provider_update_file_info;
  iface->cancel_update = fp_info_provider_cancel_update;
}

static void
fp_info_provider_init (FpInfoProvider *self)
{
  (void) self;
}

static void
fp_info_provider_class_init (FpInfoProviderClass *klass)
{
  (void) klass;
}

static void
fp_info_provider_class_finalize (FpInfoProviderClass *klass)
{
  (void) klass;
}

void
fp_info_provider_register (GTypeModule *module)
{
  fp_info_provider_register_type (module);
}
