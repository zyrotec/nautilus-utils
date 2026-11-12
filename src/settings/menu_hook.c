/* Main-menu entry "Nautilus Utils Settings" (PLAN.md §9.1.1, findings §9.6). The only settings file that
 * relies on Nautilus internals:
 *  - every NautilusWindow has its own GMenu (template child `app_menu`) on the header bar's GtkMenuButton
 *    with icon "open-menu-symbolic";
 *  - we insert one section holding our item right before the section whose first item is
 *    "app.preferences" (GMenu draws separators between sections), and an action group "nautilus-utils"
 *    on the window.
 * New windows are found through GTK's toplevel list ("items-changed" → idle scan); nothing keeps a pointer
 * to a window. If the menu isn't found, one warning is logged and the entry is skipped. */
#include "menu_hook.h"

#include "settings_dialog.h"

#define DONE_KEY      "st-menu-done" /* window qdata: we looked at this window (installed or gave up) */
#define ACTION_PREFIX "nautilus-utils"
#define ACTION_NAME   "settings"
#define ACTION_FULL   ACTION_PREFIX "." ACTION_NAME
#define PREFS_ACTION  "app.preferences"
#define MENU_LABEL    "Nautilus Utils Settings"

typedef struct
{
  GListModel *toplevels;   /* Borrowed from GTK (gtk_window_get_toplevels), lives for the process */
  gulong      changed_id;  /* items-changed handler on @toplevels */
  guint       scan_id;     /* Pending idle scan, or 0 */
  gboolean    warned;      /* "menu not found" already logged */
  GType       window_type; /* NautilusWindow, looked up lazily */
} MenuHook;

static MenuHook *mh; /* Created by init, freed by shutdown (main thread only). */

static GtkWidget *
find_menu_button (GtkWidget *w)
{
  if (GTK_IS_MENU_BUTTON (w) &&
      g_strcmp0 (gtk_menu_button_get_icon_name (GTK_MENU_BUTTON (w)), "open-menu-symbolic") == 0 &&
      gtk_menu_button_get_menu_model (GTK_MENU_BUTTON (w)) != NULL)
    return w;
  for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL; c = gtk_widget_get_next_sibling (c))
    {
      GtkWidget *found = find_menu_button (c);
      if (found != NULL)
        return found;
    }
  return NULL;
}

/* Index of the first top-level section of @menu whose first item has action @action, or -1. */
static int
find_section (GMenuModel *menu,
              const char *action)
{
  for (int i = 0; i < g_menu_model_get_n_items (menu); i++)
    {
      g_autoptr (GMenuModel) section = g_menu_model_get_item_link (menu, i, G_MENU_LINK_SECTION);
      g_autofree char *first = NULL;
      if (section != NULL && g_menu_model_get_n_items (section) > 0 &&
          g_menu_model_get_item_attribute (section, 0, G_MENU_ATTRIBUTE_ACTION, "s", &first) &&
          g_strcmp0 (first, action) == 0)
        return i;
    }
  return -1;
}

static GMenuModel *
window_menu (GtkWidget *window)
{
  GtkWidget *button = find_menu_button (window);
  return button != NULL ? gtk_menu_button_get_menu_model (GTK_MENU_BUTTON (button)) : NULL; /* borrowed */
}

static void
on_activate (GSimpleAction *action,
             GVariant      *parameter,
             gpointer       window)
{
  (void) action;
  (void) parameter;
  st_settings_dialog_present (window);
}

/* Adds our section + action group to @window. Returns FALSE if the menu isn't what we expect. */
static gboolean
install (GtkWidget *window)
{
  GMenuModel *menu = window_menu (window);
  if (menu == NULL || !G_IS_MENU (menu))
    return FALSE;
  if (find_section (menu, ACTION_FULL) >= 0)
    return TRUE;
  int prefs = find_section (menu, PREFS_ACTION);
  if (prefs < 0)
    return FALSE;

  g_autoptr (GMenu) section = g_menu_new ();
  g_menu_append (section, MENU_LABEL, ACTION_FULL);
  g_menu_insert_section (G_MENU (menu), prefs, NULL, G_MENU_MODEL (section));

  g_autoptr (GSimpleActionGroup) group = g_simple_action_group_new ();
  g_autoptr (GSimpleAction) action = g_simple_action_new (ACTION_NAME, NULL);
  /* Disconnected automatically when the window is destroyed. */
  g_signal_connect_object (action, "activate", G_CALLBACK (on_activate), window, 0);
  g_action_map_add_action (G_ACTION_MAP (group), G_ACTION (action));
  gtk_widget_insert_action_group (window, ACTION_PREFIX, G_ACTION_GROUP (group)); /* window takes a ref */
  return TRUE;
}

/* Removes our section and action group from @window (feature shutdown). */
static void
uninstall (GtkWidget *window)
{
  g_object_set_data (G_OBJECT (window), DONE_KEY, NULL);
  GMenuModel *menu = window_menu (window);
  if (menu != NULL && G_IS_MENU (menu))
    {
      int ours = find_section (menu, ACTION_FULL);
      if (ours >= 0)
        g_menu_remove (G_MENU (menu), ours);
    }
  gtk_widget_insert_action_group (window, ACTION_PREFIX, NULL);
}

static gboolean
is_nautilus_window (GObject *w)
{
  if (mh->window_type == 0)
    mh->window_type = g_type_from_name ("NautilusWindow");
  return mh->window_type != 0 && g_type_is_a (G_OBJECT_TYPE (w), mh->window_type);
}

static gboolean
scan_idle (gpointer data)
{
  (void) data;
  mh->scan_id = 0;
  guint n = g_list_model_get_n_items (mh->toplevels);
  for (guint i = 0; i < n; i++)
    {
      g_autoptr (GObject) w = g_list_model_get_item (mh->toplevels, i);
      if (!is_nautilus_window (w) || g_object_get_data (w, DONE_KEY) != NULL)
        continue;
      g_object_set_data (w, DONE_KEY, GINT_TO_POINTER (1));
      if (!install (GTK_WIDGET (w)) && !mh->warned)
        {
          mh->warned = TRUE;
          g_warning ("Nautilus' main menu was not found; the \"" MENU_LABEL "\" entry is not shown "
                     "(Nautilus internals changed; please update the extension)");
        }
    }
  return G_SOURCE_REMOVE;
}

/* A window is added to the toplevel list before its template (and so its menu) exists: look in an idle. */
static void
on_toplevels_changed (GListModel *list,
                      guint       position,
                      guint       removed,
                      guint       added,
                      gpointer    data)
{
  (void) list;
  (void) position;
  (void) removed;
  (void) data;
  if (added > 0 && mh->scan_id == 0)
    mh->scan_id = g_idle_add (scan_idle, NULL);
}

void
st_menu_hook_init (void)
{
  if (mh != NULL)
    return;
  mh = g_new0 (MenuHook, 1);
  mh->toplevels = gtk_window_get_toplevels ();
  mh->changed_id = g_signal_connect (mh->toplevels, "items-changed", G_CALLBACK (on_toplevels_changed), NULL);
  mh->scan_id = g_idle_add (scan_idle, NULL); /* windows that already exist */
}

void
st_menu_hook_shutdown (void)
{
  if (mh == NULL)
    return;
  g_clear_signal_handler (&mh->changed_id, mh->toplevels);
  g_clear_handle_id (&mh->scan_id, g_source_remove);
  guint n = g_list_model_get_n_items (mh->toplevels);
  for (guint i = 0; i < n; i++)
    {
      g_autoptr (GObject) w = g_list_model_get_item (mh->toplevels, i);
      if (g_object_get_data (w, DONE_KEY) != NULL)
        uninstall (GTK_WIDGET (w));
    }
  g_clear_pointer (&mh, g_free);
}

gboolean
st_menu_hook_open_settings (void)
{
  if (mh == NULL)
    return FALSE;
  guint n = g_list_model_get_n_items (mh->toplevels);
  for (guint i = 0; i < n; i++)
    {
      g_autoptr (GObject) w = g_list_model_get_item (mh->toplevels, i);
      if (is_nautilus_window (w) && gtk_widget_activate_action (GTK_WIDGET (w), ACTION_FULL, NULL))
        return TRUE;
    }
  return FALSE;
}
