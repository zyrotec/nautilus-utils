/* Folder-preview feature: set-up and tear-down of its module-wide services. Called from the
 * nautilus-utils module entry points (src/extension.c). */
#include "folder_preview.h"

#include "cache.h"
#include "config.h"
#include "info_provider.h"
#include "nu_settings.h"
#include "thumbs.h"
#include "view_hook.h"
#include "workers.h"

/* Static storage handed out by fp_feature_list_types(); callers only read it. */
static GType fp_types[1];

/* Feature-wide services, created in initialize and released in shutdown (main thread). */
static FpWorkers *workers; /* Owned. */
static FpThumbs  *thumbs;  /* Owned; borrowed by jobs and the cache, so freed after both. */
static FpCache   *cache;   /* Owned ref (the view hook and in-flight requests hold their own). */
static guint      settings_listener; /* nu_settings listener id, 0 when not registered. */

/* Settings changed (once per Apply): follow the on/off switch (PLAN.md §9.1.2). */
static void
on_settings_changed (NuChanged changed,
                     gpointer  data)
{
  (void) data;
  if (changed & NU_CHANGED_FOLDER_PREVIEW)
    fp_view_hook_set_enabled (nu_settings_get ()->folder_preview_enabled);
}

void
fp_feature_initialize (GTypeModule *module)
{
  fp_info_provider_register (module);
  fp_types[0] = FP_TYPE_INFO_PROVIDER;

  workers = fp_workers_new (WORKER_THREADS);
  thumbs = fp_thumbs_new (WORKER_THREADS);
  cache = fp_cache_new (workers, thumbs, NULL, LRU_FOLDERS, CACHE_MAX_BYTES);
  fp_view_hook_init (cache);
  fp_view_hook_set_enabled (nu_settings_get ()->folder_preview_enabled);
  settings_listener = nu_settings_add_listener (on_settings_changed, NULL);
  fp_info_provider_set_folder_func (fp_view_hook_folder_changed);
  g_debug ("folder-preview loaded");
}

void
fp_feature_shutdown (void)
{
  /* Order matters (cache.h): stop new work, drain the pool, then drop the cache, then the factories. */
  nu_settings_remove_listener (settings_listener);
  settings_listener = 0;
  fp_info_provider_set_folder_func (NULL);
  fp_view_hook_shutdown ();
  g_clear_pointer (&workers, fp_workers_free);
  g_clear_pointer (&cache, fp_cache_unref);
  g_clear_pointer (&thumbs, fp_thumbs_free);
  g_debug ("folder-preview shut down");
}

const GType *
fp_feature_list_types (int *num_types)
{
  *num_types = G_N_ELEMENTS (fp_types);
  return fp_types;
}
