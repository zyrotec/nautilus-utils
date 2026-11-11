/* Benchmark helper (LD_PRELOAD, test instances only): makes Nautilus skip loading nautilus-python, so
 * benchmarks can be run without the owner's Python extensions. Interposes g_module_open and refuses only
 * libnautilus-python.so; everything else goes to the real function. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <gmodule.h>
#include <string.h>

GModule *
g_module_open (const gchar *file_name, GModuleFlags flags)
{
  static GModule *(*real) (const gchar *, GModuleFlags);
  if (real == NULL)
    real = (GModule *(*) (const gchar *, GModuleFlags)) dlsym (RTLD_NEXT, "g_module_open");
  if (file_name != NULL && strstr (file_name, "libnautilus-python") != NULL)
    return NULL;
  return real (file_name, flags);
}

GModule *
g_module_open_full (const gchar *file_name, GModuleFlags flags, GError **error)
{
  static GModule *(*real) (const gchar *, GModuleFlags, GError **);
  if (real == NULL)
    real = (GModule *(*) (const gchar *, GModuleFlags, GError **)) dlsym (RTLD_NEXT, "g_module_open_full");
  if (file_name != NULL && strstr (file_name, "libnautilus-python") != NULL)
    {
      g_set_error_literal (error, G_MODULE_ERROR, G_MODULE_ERROR_FAILED, "skipped by no-python-shim (benchmark)");
      return NULL;
    }
  return real (file_name, flags, error);
}
