/* nautilus-utils module entry points that Nautilus looks up with g_module_symbol().
 * Each feature lives in its own src/<feature>/ directory and exposes initialize / shutdown / list_types;
 * this file only fans the calls out and concatenates the provider types. */
#include <gmodule.h>
#include <nautilus-extension.h>

#include "folder-preview/folder_preview.h"
#include "folder-views/folder_views.h"
#include "settings/settings.h"
#include "sidebar-drives/sidebar_drives.h"

typedef struct
{
  void          (*initialize) (GTypeModule *module);
  void          (*shutdown)   (void);
  const GType * (*list_types) (int *num_types);
} NuFeature;

static const NuFeature features[] = {
  /* First: the other features read the shared settings it starts (PLAN.md §9.2.3). */
  { st_feature_initialize, st_feature_shutdown, st_feature_list_types },
  { fp_feature_initialize, fp_feature_shutdown, fp_feature_list_types },
  { sd_feature_initialize, sd_feature_shutdown, sd_feature_list_types },
  { fv_feature_initialize, fv_feature_shutdown, fv_feature_list_types },
};

/* Upper bound on provider types across all features; checked in nautilus_module_initialize(). */
#define NU_MAX_TYPES 16

/* Static storage handed to Nautilus by nautilus_module_list_types(); Nautilus only reads it. */
static GType all_types[NU_MAX_TYPES];
static int n_all_types;

G_MODULE_EXPORT void
nautilus_module_initialize (GTypeModule *module)
{
  n_all_types = 0;
  for (gsize i = 0; i < G_N_ELEMENTS (features); i++)
    {
      features[i].initialize (module);

      int n = 0;
      const GType *types = features[i].list_types (&n);
      g_return_if_fail (n_all_types + n <= NU_MAX_TYPES);
      for (int j = 0; j < n; j++)
        all_types[n_all_types++] = types[j];
    }
}

G_MODULE_EXPORT void
nautilus_module_shutdown (void)
{
  /* Reverse order of initialization. */
  for (gsize i = G_N_ELEMENTS (features); i > 0; i--)
    features[i - 1].shutdown ();
}

G_MODULE_EXPORT void
nautilus_module_list_types (const GType **types,
                            int          *num_types)
{
  /* Borrowed pointer to static storage; caller must not free it. */
  *types = all_types;
  *num_types = n_all_types;
}
