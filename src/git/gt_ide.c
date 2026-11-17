/* Git IDEs. See gt_ide.h. */
#include "gt_ide.h"

#include <gio/gdesktopappinfo.h>

/* Desktop IDs, in order of preference when nothing is chosen (distribution packages and Flatpaks). */
static const char *const ide_ids[] = {
  "code.desktop", "com.microsoft.VSCode.desktop", "com.visualstudio.code.desktop", "code-oss.desktop", "codium.desktop", "com.vscodium.codium.desktop",
  "dev.zed.Zed.desktop", "zed.desktop", "org.gnome.Builder.desktop", "jetbrains-idea.desktop",
  "jetbrains-webstorm.desktop", "jetbrains-pycharm.desktop", "jetbrains-clion.desktop", "sublime_text.desktop",
  "com.sublimetext.three.desktop", "org.kde.kate.desktop", "org.gnome.TextEditor.desktop", NULL,
};

GList *
gt_ide_list (void)
{
  GList *out = NULL;
  for (int i = 0; ide_ids[i] != NULL; i++)
    {
      GDesktopAppInfo *app = g_desktop_app_info_new (ide_ids[i]);
      if (app != NULL)
        out = g_list_append (out, app);
    }
  return out;
}

GAppInfo *
gt_ide_get (const char *preferred)
{
  if (preferred != NULL && *preferred != '\0')
    {
      GDesktopAppInfo *app = g_desktop_app_info_new (preferred);
      if (app != NULL)
        return G_APP_INFO (app);
    }
  GList *all = gt_ide_list ();
  GAppInfo *first = all != NULL ? g_object_ref (all->data) : NULL;
  g_list_free_full (all, g_object_unref);
  return first;
}

gboolean
gt_ide_open (GAppInfo *ide, GFile *folder, GError **error)
{
  g_return_val_if_fail (G_IS_APP_INFO (ide) && G_IS_FILE (folder), FALSE);
  GList one = { folder, NULL, NULL };
  return g_app_info_launch (ide, &one, NULL, error);
}
