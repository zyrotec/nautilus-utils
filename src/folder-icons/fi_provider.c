/* See fi_provider.h. */
#include "fi_provider.h"

struct _FiInfoProvider
{
  GObject parent_instance;
};

static void fi_info_provider_iface_init (NautilusInfoProviderInterface *iface);

static FiFolderFunc folder_func; /* main thread only */

G_DEFINE_DYNAMIC_TYPE_EXTENDED (FiInfoProvider, fi_info_provider, G_TYPE_OBJECT, G_TYPE_FLAG_FINAL,
                                G_IMPLEMENT_INTERFACE_DYNAMIC (NAUTILUS_TYPE_INFO_PROVIDER,
                                                               fi_info_provider_iface_init))

void
fi_info_provider_set_folder_func (FiFolderFunc func)
{
  folder_func = func;
}

static NautilusOperationResult
fi_info_provider_update_file_info (NautilusInfoProvider     *provider,
                                   NautilusFileInfo         *file,
                                   GClosure                 *update_complete,
                                   NautilusOperationHandle **handle)
{
  (void) provider;
  (void) update_complete; /* Not kept: always completes synchronously. */
  (void) handle;
  /* Cheap: a type check here, a name lookup in the callback; any I/O is queued (PLAN.md §12.3 rule 1). */
  if (folder_func != NULL && nautilus_file_info_is_directory (file))
    folder_func (file);
  return NAUTILUS_OPERATION_COMPLETE;
}

static void
fi_info_provider_cancel_update (NautilusInfoProvider    *provider,
                                NautilusOperationHandle *handle)
{
  /* Never returns IN_PROGRESS, so there is nothing to cancel. */
  (void) provider;
  (void) handle;
}

static void
fi_info_provider_iface_init (NautilusInfoProviderInterface *iface)
{
  iface->update_file_info = fi_info_provider_update_file_info;
  iface->cancel_update = fi_info_provider_cancel_update;
}

static void
fi_info_provider_init (FiInfoProvider *self)
{
  (void) self;
}

static void
fi_info_provider_class_init (FiInfoProviderClass *klass)
{
  (void) klass;
}

static void
fi_info_provider_class_finalize (FiInfoProviderClass *klass)
{
  (void) klass;
}

void
fi_info_provider_register (GTypeModule *module)
{
  fi_info_provider_register_type (module);
}
