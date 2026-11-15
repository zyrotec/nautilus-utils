/* Git: the status bar in Files' windows (PLAN.md §14.2 item 2, §14.5 item 3). The only Git file using Nautilus'
 * private widgets:
 *   - NautilusWindow: "active-slot" (the tab shown), found through GTK's toplevel list;
 *   - NautilusWindowSlot: "location" (GFile);
 *   - Files' AdwTabView, which the shared content column (nu_content) wraps; the bar goes under it.
 * If any of these is missing the bar is simply not added (one warning). */
#include "gt_hook.h"

#include <adwaita.h>
#include <nautilus-extension.h>
#include <string.h>

#include "gt_bar.h"
#include "gt_columns.h"
#include "gt_ide.h"
#include "gt_info.h"
#include "gt_repo.h"
#include "nu_content.h"

#define HOOK_KEY "gt-hook"
#define DONE_KEY "gt-hook-done"

typedef struct
{
  GtkWidget *window;     /* borrowed: the hook lives as the window's data */
  GWeakRef   column;
  GWeakRef   bar;
  GWeakRef   slot;
  gulong     active_id;
  gulong     location_id;
  char      *root;       /* repository shown, or NULL */
} WinHook;

typedef struct
{
  GListModel *toplevels; /* borrowed from GTK */
  gulong      changed_id;
  guint       scan_id;
  gboolean    enabled;
  gboolean    show_bar;
  GAppInfo   *ide;       /* owned or NULL */
  GType       window_type;
  gboolean    warned;
  gboolean    icons_added;
} Hook;

static Hook *gh;

static void
warn_once (const char *what)
{
  if (gh->warned)
    return;
  gh->warned = TRUE;
  g_warning ("Git status bar disabled: %s (Nautilus changed?)", what);
}

static GtkWidget *
find_type (GtkWidget *w, GType t, int depth)
{
  if (t != 0 && G_TYPE_CHECK_INSTANCE_TYPE (w, t))
    return w;
  if (depth <= 0 || GTK_IS_LIST_VIEW (w) || GTK_IS_GRID_VIEW (w) || GTK_IS_COLUMN_VIEW (w))
    return NULL;
  for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL; c = gtk_widget_get_next_sibling (c))
    {
      GtkWidget *r = find_type (c, t, depth - 1);
      if (r != NULL)
        return r;
    }
  return NULL;
}

static WinHook *
get_hook (GtkWidget *window)
{
  return g_object_get_data (G_OBJECT (window), HOOK_KEY);
}

static GtBar *
hook_bar (WinHook *hook)
{
  return (GtBar *) g_weak_ref_get (&hook->bar);
}

/* The active tab's folder → the repository it is in (stat only), shown in the bar. */
static void
update (WinHook *hook)
{
  g_autoptr (GtBar) bar = hook_bar (hook);
  g_autoptr (GObject) slot = g_weak_ref_get (&hook->slot);
  if (bar == NULL)
    return;
  g_autoptr (GFile) location = NULL;
  if (slot != NULL && g_object_class_find_property (G_OBJECT_GET_CLASS (slot), "location") != NULL)
    g_object_get (slot, "location", &location, NULL);
  /* Native folders only: never stat a network share from the main thread (PLAN.md §14.2 item 1). */
  g_autofree char *path = location != NULL && g_file_is_native (location) ? g_file_get_path (location) : NULL;
  g_autofree char *git_dir = NULL;
  g_autofree char *root = path != NULL ? gt_repo_find_root (path, &git_dir) : NULL;
  if (g_strcmp0 (root, hook->root) != 0)
    {
      g_free (hook->root);
      hook->root = g_strdup (root);
      g_autoptr (GtInfo) info = root != NULL ? gt_info_get (root, git_dir) : NULL;
      gt_bar_set_info (bar, info);
    }
  gtk_widget_set_visible (GTK_WIDGET (bar), gh->show_bar && hook->root != NULL);
}

static void
on_location (GObject *slot, GParamSpec *pspec, gpointer data)
{
  (void) slot, (void) pspec;
  WinHook *hook = get_hook (GTK_WIDGET (data));
  if (hook != NULL)
    update (hook);
}

static void
watch_active_slot (WinHook *hook)
{
  g_autoptr (GObject) old = g_weak_ref_get (&hook->slot);
  if (old != NULL)
    g_clear_signal_handler (&hook->location_id, old);
  hook->location_id = 0;
  g_autoptr (GObject) slot = NULL;
  g_object_get (hook->window, "active-slot", &slot, NULL);
  g_weak_ref_set (&hook->slot, slot);
  if (slot != NULL && g_object_class_find_property (G_OBJECT_GET_CLASS (slot), "location") != NULL)
    hook->location_id = g_signal_connect_object (slot, "notify::location", G_CALLBACK (on_location), hook->window, 0);
  update (hook);
}

static void
on_active_slot (GObject *window, GParamSpec *pspec, gpointer data)
{
  (void) pspec, (void) data;
  WinHook *hook = get_hook (GTK_WIDGET (window));
  if (hook != NULL)
    watch_active_slot (hook);
}

static void
win_hook_free (gpointer data)
{
  WinHook *hook = data;
  g_weak_ref_clear (&hook->column);
  g_weak_ref_clear (&hook->bar);
  g_weak_ref_clear (&hook->slot);
  g_free (hook->root);
  g_free (hook);
}

static void
ensure_icons (void)
{
  if (gh->icons_added || gdk_display_get_default () == NULL)
    return;
  gh->icons_added = TRUE;
  gtk_icon_theme_add_resource_path (gtk_icon_theme_get_for_display (gdk_display_get_default ()),
                                    "/io/github/zyrotec/NautilusUtils/git/icons");
}

static gboolean
install (GtkWidget *window)
{
  if (g_object_class_find_property (G_OBJECT_GET_CLASS (window), "active-slot") == NULL)
    {
      warn_once ("the window has no \"active-slot\" property");
      return FALSE;
    }
  GtkWidget *tab_view = find_type (window, ADW_TYPE_TAB_VIEW, 30);
  GtkWidget *column = tab_view != NULL ? nu_content_acquire (tab_view) : NULL;
  if (column == NULL)
    {
      warn_once ("Files' tab view is not where expected");
      return FALSE;
    }
  ensure_icons ();
  WinHook *hook = g_new0 (WinHook, 1);
  hook->window = window;
  g_weak_ref_init (&hook->column, column);
  GtkWidget *bar = gt_bar_new ();
  gt_bar_set_ide (GT_BAR (bar), gh->ide);
  gtk_widget_set_visible (bar, FALSE);
  nu_content_add_bottom (column, bar);
  g_weak_ref_init (&hook->bar, bar);
  g_object_set_data_full (G_OBJECT (window), HOOK_KEY, hook, win_hook_free);
  hook->active_id = g_signal_connect (window, "notify::active-slot", G_CALLBACK (on_active_slot), NULL);
  watch_active_slot (hook);
  g_debug ("status bar installed in a window");
  return TRUE;
}

/* Removes the bar; the column puts Files' tab view back if nothing else uses it. */
static void
uninstall (GtkWidget *window)
{
  g_object_set_data (G_OBJECT (window), DONE_KEY, NULL);
  WinHook *hook = get_hook (window);
  if (hook == NULL)
    return;
  g_clear_signal_handler (&hook->active_id, window);
  g_autoptr (GObject) slot = g_weak_ref_get (&hook->slot);
  if (slot != NULL)
    g_clear_signal_handler (&hook->location_id, slot);
  g_autoptr (GtkWidget) column = g_weak_ref_get (&hook->column);
  g_autoptr (GtBar) bar = hook_bar (hook);
  if (column != NULL && bar != NULL && gtk_widget_get_parent (GTK_WIDGET (bar)) == column)
    gtk_box_remove (GTK_BOX (column), GTK_WIDGET (bar));
  if (column != NULL)
    nu_content_release (column);
  g_object_set_data (G_OBJECT (window), HOOK_KEY, NULL);
}

static gboolean
scan_idle (gpointer data)
{
  (void) data;
  gh->scan_id = 0;
  if (!gh->enabled)
    return G_SOURCE_REMOVE;
  if (gh->window_type == 0)
    gh->window_type = g_type_from_name ("NautilusWindow");
  if (gh->window_type == 0)
    return G_SOURCE_REMOVE;
  for (guint i = 0; i < g_list_model_get_n_items (gh->toplevels); i++)
    {
      g_autoptr (GObject) w = g_list_model_get_item (gh->toplevels, i);
      if (!G_TYPE_CHECK_INSTANCE_TYPE (w, gh->window_type) || g_object_get_data (w, DONE_KEY) != NULL)
        continue;
      g_object_set_data (w, DONE_KEY, GINT_TO_POINTER (1));
      install (GTK_WIDGET (w));
    }
  return G_SOURCE_REMOVE;
}

static void
on_toplevels_changed (GListModel *list, guint position, guint removed, guint added, gpointer data)
{
  (void) list, (void) position, (void) removed, (void) data;
  if (added > 0 && gh->scan_id == 0)
    gh->scan_id = g_idle_add (scan_idle, NULL);
}

static void
for_each_hook (void (*func) (WinHook *hook))
{
  for (guint i = 0; gh != NULL && i < g_list_model_get_n_items (gh->toplevels); i++)
    {
      g_autoptr (GObject) w = g_list_model_get_item (gh->toplevels, i);
      WinHook *hook = GTK_IS_WIDGET (w) ? get_hook (GTK_WIDGET (w)) : NULL;
      if (hook != NULL)
        func (hook);
    }
}

static void
uninstall_all (void)
{
  for (guint i = 0; i < g_list_model_get_n_items (gh->toplevels); i++)
    {
      g_autoptr (GObject) w = g_list_model_get_item (gh->toplevels, i);
      if (GTK_IS_WIDGET (w) && (get_hook (GTK_WIDGET (w)) != NULL || g_object_get_data (w, DONE_KEY) != NULL))
        uninstall (GTK_WIDGET (w));
    }
}

void
gt_hook_init (void)
{
  if (gh != NULL)
    return;
  gh = g_new0 (Hook, 1);
  gh->toplevels = gtk_window_get_toplevels (); /* borrowed */
  gh->changed_id = g_signal_connect (gh->toplevels, "items-changed", G_CALLBACK (on_toplevels_changed), NULL);
}

static void
apply_ide (WinHook *hook)
{
  g_autoptr (GtBar) bar = hook_bar (hook);
  if (bar != NULL)
    gt_bar_set_ide (bar, gh->ide);
}

void
gt_hook_configure (gboolean enabled, gboolean show_bar, const char *ide)
{
  if (gh == NULL)
    return;
  g_clear_object (&gh->ide);
  gh->ide = gt_ide_get (ide);
  for_each_hook (apply_ide);
  gh->show_bar = show_bar;
  for_each_hook (update);
  if (enabled == gh->enabled)
    return;
  gh->enabled = enabled;
  if (enabled)
    {
      if (gh->scan_id == 0)
        gh->scan_id = g_idle_add (scan_idle, NULL);
    }
  else
    {
      g_clear_handle_id (&gh->scan_id, g_source_remove);
      uninstall_all ();
    }
}

void
gt_hook_shutdown (void)
{
  if (gh == NULL)
    return;
  g_clear_signal_handler (&gh->changed_id, gh->toplevels);
  g_clear_handle_id (&gh->scan_id, g_source_remove);
  uninstall_all ();
  g_clear_object (&gh->ide);
  g_clear_pointer (&gh, g_free);
}

/* ---- debug driver ---- */

static WinHook *
first_hook (void)
{
  for (guint i = 0; gh != NULL && i < g_list_model_get_n_items (gh->toplevels); i++)
    {
      g_autoptr (GObject) w = g_list_model_get_item (gh->toplevels, i);
      WinHook *hook = GTK_IS_WIDGET (w) ? get_hook (GTK_WIDGET (w)) : NULL;
      if (hook != NULL)
        return hook;
    }
  return NULL;
}

gboolean
gt_hook_debug_command (const char *cmd)
{
  if (!g_str_has_prefix (cmd, "gt-"))
    return FALSE;
  WinHook *hook = first_hook ();
  g_autoptr (GtBar) bar = hook != NULL ? hook_bar (hook) : NULL;
  if (bar == NULL)
    {
      g_message ("gt: no window hooked (%s)", cmd);
      return TRUE;
    }
  if (g_str_equal (cmd, "gt-dump"))
    {
      g_autofree char *d = gt_bar_describe (bar);
      g_message ("gt dump: bar=%d %s", gtk_widget_get_visible (GTK_WIDGET (bar)), d);
    }
  else if (g_str_has_prefix (cmd, "gt-expect "))
    {
      g_autofree char *d = gt_bar_describe (bar);
      g_autofree char *full = g_strdup_printf ("bar=%d %s", gtk_widget_get_visible (GTK_WIDGET (bar)), d);
      const char *want = cmd + strlen ("gt-expect ");
      if (strstr (full, want) != NULL)
        g_message ("gt expect ok: %s", want);
      else
        g_message ("gt expect FAIL: %s — %s", want, full);
    }
  else if (g_str_has_prefix (cmd, "gt-switch "))
    gt_bar_switch_branch (bar, cmd + strlen ("gt-switch "), FALSE);
  else if (g_str_has_prefix (cmd, "gt-switch-remote "))
    gt_bar_switch_branch (bar, cmd + strlen ("gt-switch-remote "), TRUE);
  else if (g_str_has_prefix (cmd, "gt-create "))
    gt_bar_create_branch (bar, cmd + strlen ("gt-create "));
  else if (g_str_equal (cmd, "gt-cols"))
    {
      /* The Git columns visible in the list view — in the test session's in-memory settings only. */
      if (g_strcmp0 (g_getenv ("GSETTINGS_BACKEND"), "memory") != 0)
        g_message ("gt: gt-cols only with in-memory settings");
      else
        {
          g_autoptr (GSettings) s = g_settings_new ("org.gnome.nautilus.list-view");
          const char *cols[] = { "name", "NuGit::status", "NuGit::commit", "NuGit::hash", "NuGit::author", NULL };
          g_settings_set_strv (s, "default-visible-columns", cols);
          g_message ("gt cols: on");
        }
    }
  else if (g_str_has_prefix (cmd, "gt-attr "))
    {
      /* "gt-attr NAME [TEXT]": the Git values Files has for NAME in the shown folder; with TEXT, a check. */
      g_autofree char *rest = g_strdup (cmd + strlen ("gt-attr "));
      char *space = strchr (rest, ' ');
      const char *want = NULL;
      if (space != NULL)
        *space = '\0', want = space + 1;
      g_autoptr (GObject) slot = g_weak_ref_get (&hook->slot);
      g_autoptr (GFile) location = NULL;
      if (slot != NULL)
        g_object_get (slot, "location", &location, NULL);
      g_autoptr (GFile) child = location != NULL ? g_file_get_child (location, rest) : NULL;
      g_autoptr (NautilusFileInfo) fi = child != NULL ? nautilus_file_info_lookup (child) : NULL;
      g_autofree char *st = fi != NULL ? nautilus_file_info_get_string_attribute (fi, GT_ATTR_STATUS) : NULL;
      g_autofree char *cm = fi != NULL ? nautilus_file_info_get_string_attribute (fi, GT_ATTR_COMMIT) : NULL;
      g_autofree char *hs = fi != NULL ? nautilus_file_info_get_string_attribute (fi, GT_ATTR_HASH) : NULL;
      g_autofree char *au = fi != NULL ? nautilus_file_info_get_string_attribute (fi, GT_ATTR_AUTHOR) : NULL;
      g_autofree char *full = g_strdup_printf ("%s: status=%s|commit=%s|hash=%s|author=%s|", rest, st ? st : "(none)",
                                               cm ? cm : "(none)", hs ? hs : "(none)", au ? au : "(none)");
      if (want == NULL)
        g_message ("gt attr %s", full);
      else if (strstr (full, want) != NULL)
        g_message ("gt expect ok: %s %s", rest, want);
      else
        g_message ("gt expect FAIL: %s %s — %s", rest, want, full);
    }
  else if (g_str_equal (cmd, "gt-push"))
    gt_bar_push (bar);
  else if (g_str_equal (cmd, "gt-pull"))
    gt_bar_pull (bar);
  else if (g_str_equal (cmd, "gt-sync"))
    gt_bar_sync (bar);
  else
    g_message ("gt: unknown command %s", cmd);
  return TRUE;
}
