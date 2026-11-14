/* Details pane hook (PLAN.md §13, findings §13.7). See dp_hook.h.
 *
 * Nautilus internals used (all looked up by name, no private headers):
 *   - NautilusWindow:active-slot (NautilusWindowSlot), its template's content AdwToolbarView whose content is
 *     the AdwTabView, and NautilusToolbar → NautilusViewControls in a GtkBox of the header bar.
 *   - NautilusWindowSlot: "selection" (only its notification is used) and "location" (GFile).
 *   - NautilusFilesView → NautilusListBase:model, a GtkSelectionModel of GtkTreeListRow → NautilusViewItem
 *     with "file", a NautilusFile (public NautilusFileInfo).
 *
 * Lifetime (PLAN.md §4.1): a WinHook is qdata on its window (freed with it, or on uninstall); it holds only
 * weak refs to widgets. Handlers on other objects are connected with g_signal_connect_object (…, window) so
 * they go away with the window, and are disconnected on uninstall. Timers are removed in win_hook_free. */
#include "dp_hook.h"

#include <adwaita.h>
#include <nautilus-extension.h>

#include "config.h"
#include "dp_pane.h"
#include "nu_settings.h"

#ifdef DP_DEBUG_DRIVER
#include <string.h>
#endif

#define HOOK_KEY      "nu-details-pane"
#define DONE_KEY      "nu-details-pane-done" /* window qdata: looked at (installed or gave up) */
#define ACTION_PREFIX "details-pane"
#define ACTION_NAME   "toggle"
#define SHORTCUT      "<Shift><Alt>p"

typedef struct
{
  GtkWidget          *window;        /* Borrowed: we are its qdata, so we never outlive it. */
  GWeakRef            toolbar_view;  /* The content AdwToolbarView (Nautilus') */
  GWeakRef            tab_view;      /* Nautilus' AdwTabView, moved into @paned */
  GWeakRef            view_controls; /* The header's NautilusViewControls */
  GWeakRef            header_bar;    /* Nautilus' content AdwHeaderBar (the line under it, P15) */
  GWeakRef            paned;         /* Ours */
  GWeakRef            pane;          /* Ours (DpPane) */
  GWeakRef            button;        /* Ours */
  GWeakRef            shortcuts;     /* Ours (GtkShortcutController on the window) */
  GWeakRef            action;        /* Ours: the stateful "details-pane.toggle" (owned by the window's group) */
  GWeakRef            slot;          /* The slot whose selection we follow */
  gulong              slot_sel_id;   /* notify::selection on @slot */
  gulong              slot_loc_id;   /* notify::location on @slot */
  gulong              active_id;     /* notify::active-slot on the window */
  gulong              vc_visible_id; /* notify::visible on @view_controls */
  gulong              position_id;   /* notify::position on @paned */
  guint               debounce_id;   /* Pending selection update, or 0 */
  guint               width_id;      /* Pending width apply / save, or 0 */
  guint               width_tries;   /* apply_width retries while the paned has no size yet */
  gboolean            applying;      /* We are setting the paned position ourselves */
  gboolean            switching;     /* Right ↔ bottom: position changes until apply_width aren't the user's */
  gboolean            dirty;         /* The selection changed while the pane was hidden */
  char               *shown_key;     /* Owned: what the pane shows (location + selected URIs) */
} WinHook;

typedef struct
{
  GListModel *toplevels;   /* Borrowed from GTK, lives for the process */
  gulong      changed_id;
  guint       scan_id;
  gboolean    enabled;
  gboolean    warned;
  gboolean    visible;     /* Global state (PLAN.md §13.1.1, P8) */
  int         width;
  int         height;      /* At the bottom (§13.10) */
  gboolean    bottom;      /* Position: bottom (Windows 7 strip) instead of right */
  gboolean    header_line; /* P15 */
  guint       save_id;     /* Pending settings write of a dragged width, or 0 */
  GType       window_type, slot_type, files_view_type, list_base_type, toolbar_type, view_controls_type;
} Hook;

static Hook *dh; /* Created by init, freed by shutdown (main thread). */

static void apply_visibility (WinHook *hook);
static void schedule_update (WinHook *hook);

/* ------------------------------------------------------------------------------------------------ */
/* Helpers                                                                                          */

/* First descendant of @w (or @w) of type @t, depth-first, at most @depth levels down. Borrowed. Stops at
 * list/grid views so it never walks into cells. */
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

static gboolean
types_ready (void)
{
  if (dh->window_type == 0)
    dh->window_type = g_type_from_name ("NautilusWindow");
  if (dh->slot_type == 0)
    dh->slot_type = g_type_from_name ("NautilusWindowSlot");
  if (dh->files_view_type == 0)
    dh->files_view_type = g_type_from_name ("NautilusFilesView");
  if (dh->list_base_type == 0)
    dh->list_base_type = g_type_from_name ("NautilusListBase");
  if (dh->toolbar_type == 0)
    dh->toolbar_type = g_type_from_name ("NautilusToolbar");
  if (dh->view_controls_type == 0)
    dh->view_controls_type = g_type_from_name ("NautilusViewControls");
  return dh->window_type != 0 && dh->toolbar_type != 0 && dh->view_controls_type != 0;
}

static void
warn_once (const char *what)
{
  if (dh->warned)
    return;
  dh->warned = TRUE;
  g_warning ("details pane disabled: %s (Nautilus internals changed; please update the extension)", what);
}

static WinHook *
get_hook (GtkWidget *window)
{
  return g_object_get_data (G_OBJECT (window), HOOK_KEY);
}

/* New ref or NULL. */
static DpPane *
hook_pane (WinHook *hook)
{
  return g_weak_ref_get (&hook->pane);
}

/* ------------------------------------------------------------------------------------------------ */
/* Selection → pane                                                                                 */

/* The slot's selection model (new ref) or NULL (e.g. the Network view, or Files' internals changed). */
static GtkSelectionModel *
slot_selection_model (GtkWidget *slot)
{
  /* Nautilus registers these when it builds its first view, after the window exists: look them up late. */
  if (dh->files_view_type == 0)
    dh->files_view_type = g_type_from_name ("NautilusFilesView");
  if (dh->list_base_type == 0)
    dh->list_base_type = g_type_from_name ("NautilusListBase");
  if (dh->files_view_type == 0 || dh->list_base_type == 0)
    return NULL;
  GtkWidget *files_view = find_type (slot, dh->files_view_type, 8);
  GtkWidget *list_base = files_view != NULL ? find_type (files_view, dh->list_base_type, 4) : NULL;
  if (list_base == NULL || g_object_class_find_property (G_OBJECT_GET_CLASS (list_base), "model") == NULL)
    return NULL;
  GObject *model = NULL;
  g_object_get (list_base, "model", &model, NULL);
  if (model != NULL && !GTK_IS_SELECTION_MODEL (model))
    g_clear_object (&model);
  return (GtkSelectionModel *) model;
}

/* New ref: the GFile of a selection model item (GtkTreeListRow → NautilusViewItem → NautilusFile). */
static GFile *
item_location (GObject *item)
{
  g_autoptr (GObject) inner = NULL;
  if (GTK_IS_TREE_LIST_ROW (item))
    inner = gtk_tree_list_row_get_item (GTK_TREE_LIST_ROW (item));
  else
    inner = g_object_ref (item);
  if (inner == NULL || g_object_class_find_property (G_OBJECT_GET_CLASS (inner), "file") == NULL)
    return NULL;
  g_autoptr (GObject) file = NULL;
  g_object_get (inner, "file", &file, NULL);
  return file != NULL && NAUTILUS_IS_FILE_INFO (file) ? nautilus_file_info_get_location (NAUTILUS_FILE_INFO (file))
                                                      : NULL;
}

static gboolean update_now_impl (gpointer data);

static gint64 max_update_us; /* longest update_now (debug dump) */

static gboolean
update_now (gpointer data)
{
  gint64 t0 = g_get_monotonic_time ();
  gboolean r = update_now_impl (data);
  gint64 dt = g_get_monotonic_time () - t0;
  max_update_us = MAX (max_update_us, dt);
  if (dt > 16000)
    g_debug ("slow main-thread step: selection update took %" G_GINT64_FORMAT " µs", dt);
  return r;
}

static gboolean
update_now_impl (gpointer data)
{
  WinHook *hook = data;
  hook->debounce_id = 0;
  g_autoptr (DpPane) pane = hook_pane (hook);
  g_autoptr (GtkWidget) slot = g_weak_ref_get (&hook->slot);
  if (pane == NULL)
    return G_SOURCE_REMOVE;
  if (!gtk_widget_get_visible (GTK_WIDGET (pane)))
    {
      hook->dirty = TRUE;
      return G_SOURCE_REMOVE;
    }
  hook->dirty = FALSE;
  g_autoptr (GFile) location = NULL;
  if (slot != NULL && g_object_class_find_property (G_OBJECT_GET_CLASS (slot), "location") != NULL)
    g_object_get (slot, "location", &location, NULL);
  g_autoptr (GPtrArray) files = g_ptr_array_new_with_free_func (g_object_unref);
  guint n_selected = 0;
  g_autoptr (GtkSelectionModel) model = slot != NULL ? slot_selection_model (slot) : NULL;
  if (model != NULL)
    {
      g_autoptr (GtkBitset) set = gtk_selection_model_get_selection (model);
      n_selected = (guint) MIN (gtk_bitset_get_size (set), G_MAXUINT);
      GtkBitsetIter iter;
      guint i;
      for (gboolean ok = gtk_bitset_iter_init_first (&iter, set, &i); ok && files->len < DP_MULTI_MAX;
           ok = gtk_bitset_iter_next (&iter, &i))
        {
          g_autoptr (GObject) item = g_list_model_get_item (G_LIST_MODEL (model), i);
          GFile *f = item != NULL ? item_location (item) : NULL;
          if (f != NULL)
            g_ptr_array_add (files, f); /* takes the ref */
        }
    }
  /* Skip if nothing changed (Files also notifies e.g. after reloading a folder). */
  GString *key = g_string_new (NULL);
  g_autofree char *loc_uri = location != NULL ? g_file_get_uri (location) : NULL;
  g_string_append_printf (key, "%s\n%u", loc_uri != NULL ? loc_uri : "", n_selected);
  for (guint i = 0; i < files->len; i++)
    {
      g_autofree char *uri = g_file_get_uri (g_ptr_array_index (files, i));
      g_string_append_printf (key, "\n%s", uri);
    }
  if (g_strcmp0 (key->str, hook->shown_key) == 0)
    {
      g_string_free (key, TRUE);
      return G_SOURCE_REMOVE;
    }
  g_free (hook->shown_key);
  hook->shown_key = g_string_free (key, FALSE);
  dp_pane_show (pane, location, files, n_selected);
  return G_SOURCE_REMOVE;
}

static void
schedule_update (WinHook *hook)
{
  g_clear_handle_id (&hook->debounce_id, g_source_remove);
  hook->debounce_id = g_timeout_add (DP_DEBOUNCE_MS, update_now, hook);
}

static void
on_slot_changed (GObject *slot, GParamSpec *pspec, gpointer window)
{
  (void) slot;
  (void) pspec;
  WinHook *hook = get_hook (window);
  if (hook != NULL)
    schedule_update (hook);
}

static void
unwatch_slot (WinHook *hook)
{
  g_autoptr (GObject) slot = g_weak_ref_get (&hook->slot);
  if (slot != NULL)
    {
      g_clear_signal_handler (&hook->slot_sel_id, slot);
      g_clear_signal_handler (&hook->slot_loc_id, slot);
    }
  hook->slot_sel_id = hook->slot_loc_id = 0;
  g_weak_ref_set (&hook->slot, NULL);
}

/* Follows the window's active tab. */
static void
watch_active_slot (WinHook *hook)
{
  unwatch_slot (hook);
  g_autoptr (GObject) slot = NULL;
  g_object_get (hook->window, "active-slot", &slot, NULL);
  if (slot != NULL)
    {
      g_weak_ref_set (&hook->slot, slot);
      if (g_object_class_find_property (G_OBJECT_GET_CLASS (slot), "selection") != NULL)
        hook->slot_sel_id = g_signal_connect_object (slot, "notify::selection", G_CALLBACK (on_slot_changed),
                                                     hook->window, 0);
      else
        warn_once ("the tab has no \"selection\" property; the pane can't follow the selection");
      hook->slot_loc_id = g_signal_connect_object (slot, "notify::location", G_CALLBACK (on_slot_changed),
                                                   hook->window, 0);
    }
  schedule_update (hook);
}

static void
on_active_slot (GObject *window, GParamSpec *pspec, gpointer data)
{
  (void) pspec;
  (void) data;
  WinHook *hook = get_hook (GTK_WIDGET (window));
  if (hook != NULL)
    watch_active_slot (hook);
}

/* ------------------------------------------------------------------------------------------------ */
/* Visibility and width                                                                             */

/* Puts the pane at dh->width once the paned has a size (retries for a short while after a show). */
static gboolean
apply_width (gpointer data)
{
  WinHook *hook = data;
  hook->width_id = 0;
  g_autoptr (GtkWidget) paned = g_weak_ref_get (&hook->paned);
  g_autoptr (DpPane) pane = hook_pane (hook);
  if (paned == NULL || pane == NULL || !gtk_widget_get_visible (GTK_WIDGET (pane)))
    return G_SOURCE_REMOVE;
  int total = dh->bottom ? gtk_widget_get_height (paned) : gtk_widget_get_width (paned);
  if (total <= 0)
    {
      if (hook->width_tries++ < 40)
        hook->width_id = g_timeout_add (25, apply_width, hook);
      else
        hook->switching = FALSE; /* gave up: let the user's drags count again */
      return G_SOURCE_REMOVE;
    }
  int handle = gtk_paned_get_wide_handle (GTK_PANED (paned)) ? 5 : 1;
  int position = MAX (total - (dh->bottom ? dh->height : dh->width) - handle, 0);
  hook->applying = TRUE;
  gtk_paned_set_position (GTK_PANED (paned), position);
  hook->applying = FALSE;
  hook->switching = FALSE;
  return G_SOURCE_REMOVE;
}

static void
queue_apply_width (WinHook *hook)
{
  g_clear_handle_id (&hook->width_id, g_source_remove);
  hook->width_tries = 0;
  hook->width_id = g_idle_add (apply_width, hook);
}

static gboolean
save_width (gpointer data)
{
  (void) data;
  dh->save_id = 0;
  nu_settings_set_details_pane_state (dh->visible, dh->width);
  nu_settings_set_details_pane_height (dh->height);
  return G_SOURCE_REMOVE;
}

/* The user dragged the handle: remember the pane's new width (saved after the drag settles). */
static void
on_position (GObject *paned, GParamSpec *pspec, gpointer window)
{
  (void) pspec;
  WinHook *hook = get_hook (window);
  g_autoptr (DpPane) pane = hook != NULL ? hook_pane (hook) : NULL;
  if (hook == NULL || hook->applying || hook->switching || pane == NULL || !gtk_widget_get_visible (GTK_WIDGET (pane)))
    return;
  int total = dh->bottom ? gtk_widget_get_height (GTK_WIDGET (paned)) : gtk_widget_get_width (GTK_WIDGET (paned));
  if (total <= 0 || !gtk_widget_get_mapped (GTK_WIDGET (paned)))
    return;
  int handle = gtk_paned_get_wide_handle (GTK_PANED (paned)) ? 5 : 1;
  int size = total - gtk_paned_get_position (GTK_PANED (paned)) - handle;
  if (dh->bottom)
    {
      size = CLAMP (size, NU_DETAILS_PANE_HEIGHT_MIN, NU_DETAILS_PANE_HEIGHT_MAX);
      if (size == dh->height)
        return;
      dh->height = size;
      dp_pane_set_preferred_height (pane, size);
    }
  else
    {
      size = CLAMP (size, NU_DETAILS_PANE_WIDTH_MIN, NU_DETAILS_PANE_WIDTH_MAX);
      if (size == dh->width)
        return;
      dh->width = size;
      dp_pane_set_preferred_width (pane, size);
    }
  g_clear_handle_id (&dh->save_id, g_source_remove);
  dh->save_id = g_timeout_add (500, save_width, NULL);
}

/* Shows the pane when the state says so and the window is wide (header view controls shown, §13.7 item 2). */
static void
apply_visibility (WinHook *hook)
{
  g_autoptr (DpPane) pane = hook_pane (hook);
  g_autoptr (GtkWidget) vc = g_weak_ref_get (&hook->view_controls);
  if (pane == NULL)
    return;
  gboolean show = dh->visible && (vc == NULL || gtk_widget_get_visible (vc));
  gboolean was = gtk_widget_get_visible (GTK_WIDGET (pane));
  dp_pane_set_preferred_width (pane, dh->width);
  dp_pane_set_preferred_height (pane, dh->height);
  gtk_widget_set_visible (GTK_WIDGET (pane), show);
  /* P15: a line under Files' content header bar while the pane is shown. */
  g_autoptr (GtkWidget) header = g_weak_ref_get (&hook->header_bar);
  if (header != NULL)
    {
      if (show && dh->header_line)
        gtk_widget_add_css_class (header, "dp-header-line");
      else
        gtk_widget_remove_css_class (header, "dp-header-line");
    }
  if (show && !was)
    {
      queue_apply_width (hook);
      if (hook->dirty || hook->shown_key == NULL)
        schedule_update (hook);
    }
  g_autoptr (GSimpleAction) action = g_weak_ref_get (&hook->action);
  if (action != NULL)
    g_simple_action_set_state (action, g_variant_new_boolean (dh->visible));
}

/* Right or bottom (§13.10 P14): our paned's orientation and the pane's layout. */
static void
apply_position (WinHook *hook)
{
  g_autoptr (GtkWidget) paned = g_weak_ref_get (&hook->paned);
  g_autoptr (DpPane) pane = hook_pane (hook);
  if (paned == NULL || pane == NULL)
    return;
  GtkOrientation o = dh->bottom ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL;
  if (gtk_orientable_get_orientation (GTK_ORIENTABLE (paned)) != o)
    {
      /* The old position means nothing in the new direction: not a drag (it stored a width as the height). */
      hook->switching = TRUE;
      gtk_orientable_set_orientation (GTK_ORIENTABLE (paned), o);
      dp_pane_set_layout (pane, dh->bottom ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL);
      queue_apply_width (hook);
    }
}

static void
on_view_controls_visible (GObject *vc, GParamSpec *pspec, gpointer window)
{
  (void) vc;
  (void) pspec;
  WinHook *hook = get_hook (window);
  if (hook != NULL)
    apply_visibility (hook);
}

static void
for_each_hook (void (*fn) (WinHook *))
{
  guint n = g_list_model_get_n_items (dh->toplevels);
  for (guint i = 0; i < n; i++)
    {
      g_autoptr (GObject) w = g_list_model_get_item (dh->toplevels, i);
      WinHook *hook = get_hook (GTK_WIDGET (w));
      if (hook != NULL)
        fn (hook);
    }
}

/* Button / shortcut: flips the global state; every window follows (P8). */
static void
on_toggle (GSimpleAction *action, GVariant *value, gpointer window)
{
  (void) action;
  (void) window;
  dh->visible = g_variant_get_boolean (value);
  for_each_hook (apply_visibility);
  if (!nu_settings_set_details_pane_state (dh->visible, dh->width))
    g_debug ("settings unavailable: the pane state is not remembered");
}

/* ------------------------------------------------------------------------------------------------ */
/* Install / uninstall                                                                              */

static void
win_hook_free (gpointer data)
{
  WinHook *hook = data;
  g_clear_handle_id (&hook->debounce_id, g_source_remove);
  g_clear_handle_id (&hook->width_id, g_source_remove);
  g_weak_ref_clear (&hook->toolbar_view);
  g_weak_ref_clear (&hook->tab_view);
  g_weak_ref_clear (&hook->view_controls);
  g_weak_ref_clear (&hook->header_bar);
  g_weak_ref_clear (&hook->paned);
  g_weak_ref_clear (&hook->pane);
  g_weak_ref_clear (&hook->button);
  g_weak_ref_clear (&hook->shortcuts);
  g_weak_ref_clear (&hook->action);
  g_weak_ref_clear (&hook->slot);
  g_free (hook->shown_key);
  g_free (hook);
}

static gboolean
install (GtkWidget *window)
{
  GtkWidget *toolbar = find_type (window, dh->toolbar_type, 30);
  GtkWidget *vc = toolbar != NULL ? find_type (toolbar, dh->view_controls_type, 12) : NULL;
  GtkWidget *vc_box = vc != NULL ? gtk_widget_get_parent (vc) : NULL;
  if (vc == NULL || !GTK_IS_BOX (vc_box))
    {
      warn_once ("the header bar's view controls were not found");
      return FALSE;
    }
  GtkWidget *tab_view = find_type (window, ADW_TYPE_TAB_VIEW, 30);
  GtkWidget *toolbar_view = tab_view != NULL ? gtk_widget_get_parent (tab_view) : NULL;
  if (!ADW_IS_TOOLBAR_VIEW (toolbar_view) || adw_toolbar_view_get_content (ADW_TOOLBAR_VIEW (toolbar_view)) != tab_view)
    {
      warn_once ("the tab view is not where expected");
      return FALSE;
    }
  if (g_object_class_find_property (G_OBJECT_GET_CLASS (window), "active-slot") == NULL)
    {
      warn_once ("the window has no \"active-slot\" property");
      return FALSE;
    }

  WinHook *hook = g_new0 (WinHook, 1);
  hook->window = window;
  g_weak_ref_init (&hook->header_bar, find_type (toolbar, ADW_TYPE_HEADER_BAR, 4));
  g_weak_ref_init (&hook->toolbar_view, toolbar_view);
  g_weak_ref_init (&hook->tab_view, tab_view);
  g_weak_ref_init (&hook->view_controls, vc);
  g_object_set_data_full (G_OBJECT (window), HOOK_KEY, hook, win_hook_free);

  /* The paned: [Nautilus' tab view | our pane] (§13.7 item 3), done once per window. */
  GtkWidget *paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class (paned, "dp-paned"); /* its line is drawn by the pane instead (P13) */
  gtk_paned_set_resize_start_child (GTK_PANED (paned), TRUE);
  gtk_paned_set_shrink_start_child (GTK_PANED (paned), FALSE);
  gtk_paned_set_resize_end_child (GTK_PANED (paned), FALSE);
  gtk_paned_set_shrink_end_child (GTK_PANED (paned), FALSE);
  GtkWidget *pane = dp_pane_new ();
  gtk_widget_set_visible (pane, FALSE);
  g_object_ref (tab_view); /* keep it alive while it has no parent */
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), NULL);
  gtk_paned_set_start_child (GTK_PANED (paned), tab_view);
  gtk_paned_set_end_child (GTK_PANED (paned), pane);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), paned);
  g_object_unref (tab_view);
  g_weak_ref_init (&hook->paned, paned);
  g_weak_ref_init (&hook->pane, pane);
  hook->position_id = g_signal_connect_object (paned, "notify::position", G_CALLBACK (on_position), window, 0);

  /* Action, button, shortcut. */
  g_autoptr (GSimpleActionGroup) group = g_simple_action_group_new ();
  g_autoptr (GSimpleAction) action = g_simple_action_new_stateful (ACTION_NAME, NULL,
                                                                   g_variant_new_boolean (dh->visible));
  g_signal_connect_object (action, "change-state", G_CALLBACK (on_toggle), window, 0);
  g_action_map_add_action (G_ACTION_MAP (group), G_ACTION (action));
  gtk_widget_insert_action_group (window, ACTION_PREFIX, G_ACTION_GROUP (group)); /* window takes a ref */
  g_weak_ref_init (&hook->action, action);

  GtkWidget *button = gtk_toggle_button_new ();
  gtk_button_set_icon_name (GTK_BUTTON (button), "sidebar-show-right-symbolic");
  gtk_widget_set_tooltip_text (button, "Details Pane");
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, "Details Pane",
                                  GTK_ACCESSIBLE_PROPERTY_KEY_SHORTCUTS, "Alt+Shift+P", -1);
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
  gtk_actionable_set_action_name (GTK_ACTIONABLE (button), ACTION_PREFIX "." ACTION_NAME);
  gtk_widget_insert_after (button, vc_box, vc);
  g_object_bind_property (vc, "visible", button, "visible", G_BINDING_SYNC_CREATE);
  g_weak_ref_init (&hook->button, button);
  hook->vc_visible_id = g_signal_connect_object (vc, "notify::visible", G_CALLBACK (on_view_controls_visible),
                                                 window, 0);

  GtkEventController *shortcuts = gtk_shortcut_controller_new ();
  gtk_event_controller_set_propagation_phase (shortcuts, GTK_PHASE_CAPTURE);
  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                        gtk_shortcut_new (gtk_shortcut_trigger_parse_string (SHORTCUT),
                                                          gtk_named_action_new (ACTION_PREFIX "." ACTION_NAME)));
  gtk_widget_add_controller (window, shortcuts); /* window owns it */
  g_weak_ref_init (&hook->shortcuts, shortcuts);

  hook->active_id = g_signal_connect (window, "notify::active-slot", G_CALLBACK (on_active_slot), NULL);
  watch_active_slot (hook);
  apply_position (hook);
  apply_visibility (hook);
  g_debug ("installed in a window");
  return TRUE;
}

/* Puts the window back exactly as Nautilus built it. */
static void
uninstall (GtkWidget *window)
{
  g_object_set_data (G_OBJECT (window), DONE_KEY, NULL);
  WinHook *hook = get_hook (window);
  if (hook == NULL)
    {
      gtk_widget_insert_action_group (window, ACTION_PREFIX, NULL);
      return;
    }
  unwatch_slot (hook);
  g_clear_signal_handler (&hook->active_id, window);
  g_autoptr (GtkWidget) vc = g_weak_ref_get (&hook->view_controls);
  if (vc != NULL)
    g_clear_signal_handler (&hook->vc_visible_id, vc);
  g_autoptr (GtkWidget) paned = g_weak_ref_get (&hook->paned);
  g_autoptr (GtkWidget) tab_view = g_weak_ref_get (&hook->tab_view);
  g_autoptr (GtkWidget) toolbar_view = g_weak_ref_get (&hook->toolbar_view);
  if (paned != NULL)
    {
      g_clear_signal_handler (&hook->position_id, paned);
      g_autoptr (DpPane) pane = hook_pane (hook);
      if (pane != NULL)
        dp_pane_stop_media (pane);
      if (tab_view != NULL && toolbar_view != NULL &&
          adw_toolbar_view_get_content (ADW_TOOLBAR_VIEW (toolbar_view)) == paned)
        {
          gtk_paned_set_start_child (GTK_PANED (paned), NULL); /* our ref (tab_view) keeps it alive */
          adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), tab_view); /* drops the paned */
        }
    }
  g_autoptr (GtkWidget) button = g_weak_ref_get (&hook->button);
  if (button != NULL)
    gtk_widget_unparent (button);
  g_autoptr (GtkWidget) header = g_weak_ref_get (&hook->header_bar);
  if (header != NULL)
    gtk_widget_remove_css_class (header, "dp-header-line"); /* Files' header bar exactly as before */
  g_autoptr (GtkEventController) shortcuts = g_weak_ref_get (&hook->shortcuts);
  if (shortcuts != NULL)
    gtk_widget_remove_controller (window, shortcuts);
  gtk_widget_insert_action_group (window, ACTION_PREFIX, NULL);
  g_object_set_data (G_OBJECT (window), HOOK_KEY, NULL); /* frees the hook */
}

static gboolean
scan_idle (gpointer data)
{
  (void) data;
  dh->scan_id = 0;
  if (!dh->enabled || !types_ready ())
    return G_SOURCE_REMOVE;
  guint n = g_list_model_get_n_items (dh->toplevels);
  for (guint i = 0; i < n; i++)
    {
      g_autoptr (GObject) w = g_list_model_get_item (dh->toplevels, i);
      if (!G_TYPE_CHECK_INSTANCE_TYPE (w, dh->window_type) || g_object_get_data (w, DONE_KEY) != NULL)
        continue;
      g_object_set_data (w, DONE_KEY, GINT_TO_POINTER (1));
      install (GTK_WIDGET (w));
    }
  return G_SOURCE_REMOVE;
}

static void
on_toplevels_changed (GListModel *list, guint position, guint removed, guint added, gpointer data)
{
  (void) list;
  (void) position;
  (void) removed;
  (void) data;
  if (added > 0 && dh->scan_id == 0)
    dh->scan_id = g_idle_add (scan_idle, NULL);
}

static void
uninstall_all (void)
{
  guint n = g_list_model_get_n_items (dh->toplevels);
  for (guint i = 0; i < n; i++)
    {
      g_autoptr (GObject) w = g_list_model_get_item (dh->toplevels, i);
      if (g_object_get_data (w, DONE_KEY) != NULL || get_hook (GTK_WIDGET (w)) != NULL)
        uninstall (GTK_WIDGET (w));
    }
}

void
dp_hook_init (gboolean enabled)
{
  if (dh != NULL)
    return;
  dh = g_new0 (Hook, 1);
  const NuConfig *c = nu_settings_get ();
  dh->visible = c->details_pane_visible;
  dh->width = c->details_pane_width;
  dh->height = c->details_pane_height;
  dh->bottom = c->details_pane_position == NU_PANE_BOTTOM;
  dh->header_line = c->details_pane_header_border;
  dh->toplevels = gtk_window_get_toplevels ();
  dh->changed_id = g_signal_connect (dh->toplevels, "items-changed", G_CALLBACK (on_toplevels_changed), NULL);
  dp_hook_set_enabled (enabled);
}

void
dp_hook_set_enabled (gboolean enabled)
{
  if (dh == NULL || dh->enabled == enabled)
    return;
  dh->enabled = enabled;
  if (enabled)
    {
      if (dh->scan_id == 0)
        dh->scan_id = g_idle_add (scan_idle, NULL);
    }
  else
    {
      g_clear_handle_id (&dh->scan_id, g_source_remove);
      uninstall_all ();
    }
}

void
dp_hook_set_state (gboolean visible,
                   int      width,
                   int      height)
{
  if (dh == NULL)
    return;
  width = CLAMP (width, NU_DETAILS_PANE_WIDTH_MIN, NU_DETAILS_PANE_WIDTH_MAX);
  height = CLAMP (height, NU_DETAILS_PANE_HEIGHT_MIN, NU_DETAILS_PANE_HEIGHT_MAX);
  gboolean width_changed = width != dh->width || height != dh->height;
  if (visible == dh->visible && !width_changed)
    return;
  dh->visible = visible;
  dh->width = width;
  dh->height = height;
  for_each_hook (apply_visibility);
  if (width_changed)
    for_each_hook (queue_apply_width);
}

void
dp_hook_set_look (gboolean bottom,
                  gboolean header_line)
{
  if (dh == NULL)
    return;
  dh->bottom = bottom;
  dh->header_line = header_line;
  for_each_hook (apply_position);
  for_each_hook (apply_visibility);
}

void
dp_hook_shutdown (void)
{
  if (dh == NULL)
    return;
  g_clear_signal_handler (&dh->changed_id, dh->toplevels);
  g_clear_handle_id (&dh->scan_id, g_source_remove);
  if (dh->save_id != 0)
    {
      g_clear_handle_id (&dh->save_id, g_source_remove);
      nu_settings_set_details_pane_state (dh->visible, dh->width);
    }
  uninstall_all ();
  g_clear_pointer (&dh, g_free);
}

/* ------------------------------------------------------------------------------------------------ */
/* Debug driver                                                                                     */

#ifdef DP_DEBUG_DRIVER
/* The first window with our hook (FP_DEBUG_ROOT's window in the smoke test), or NULL. Borrowed. */
static WinHook *
first_hook (void)
{
  guint n = dh != NULL ? g_list_model_get_n_items (dh->toplevels) : 0;
  for (guint i = 0; i < n; i++)
    {
      g_autoptr (GObject) w = g_list_model_get_item (dh->toplevels, i);
      WinHook *hook = get_hook (GTK_WIDGET (w));
      if (hook != NULL)
        return hook;
    }
  return NULL;
}

static void
debug_select (WinHook *hook, const char *arg)
{
  g_autoptr (GtkWidget) slot = g_weak_ref_get (&hook->slot);
  g_autoptr (GtkSelectionModel) model = slot != NULL ? slot_selection_model (slot) : NULL;
  if (model == NULL)
    {
      GtkWidget *fv = slot != NULL ? find_type (slot, dh->files_view_type, 8) : NULL;
      GtkWidget *lb = fv != NULL ? find_type (fv, dh->list_base_type, 4) : NULL;
      g_message ("dp select: no selection model (slot=%s files-view=%s list-base=%s)",
                 slot != NULL ? G_OBJECT_TYPE_NAME (slot) : "-", fv != NULL ? G_OBJECT_TYPE_NAME (fv) : "-",
                 lb != NULL ? G_OBJECT_TYPE_NAME (lb) : "-");
      return;
    }
  guint n = g_list_model_get_n_items (G_LIST_MODEL (model));
  if (g_str_equal (arg, "all"))
    gtk_selection_model_select_all (model);
  else if (g_str_equal (arg, "none"))
    gtk_selection_model_unselect_all (model);
  else if (g_str_has_prefix (arg, "name "))
    {
      const char *name = arg + 5;
      for (guint i = 0; i < n; i++)
        {
          g_autoptr (GObject) item = g_list_model_get_item (G_LIST_MODEL (model), i);
          g_autoptr (GFile) f = item_location (item);
          g_autofree char *base = f != NULL ? g_file_get_basename (f) : NULL;
          if (g_strcmp0 (base, name) == 0)
            {
              gtk_selection_model_select_item (model, i, TRUE);
              return;
            }
        }
      g_message ("dp select: no item named %s", name);
    }
  else
    {
      guint64 i = g_ascii_strtoull (arg, NULL, 10);
      if (i < n)
        gtk_selection_model_select_item (model, (guint) i, TRUE);
    }
}

gboolean
dp_hook_debug_command (const char *cmd)
{
  if (!g_str_has_prefix (cmd, "dp-"))
    return FALSE;
  WinHook *hook = first_hook ();
  g_autoptr (DpPane) pane = hook != NULL ? hook_pane (hook) : NULL;
  if (g_str_equal (cmd, "dp-dump"))
    {
      guint windows = 0;
      guint n = dh != NULL ? g_list_model_get_n_items (dh->toplevels) : 0;
      for (guint i = 0; i < n; i++)
        {
          g_autoptr (GObject) w = g_list_model_get_item (dh->toplevels, i);
          windows += get_hook (GTK_WIDGET (w)) != NULL;
        }
      g_autofree char *desc = pane != NULL ? dp_pane_describe (pane) : g_strdup ("-");
      g_autoptr (GtkWidget) button = hook != NULL ? g_weak_ref_get (&hook->button) : NULL;
      g_autoptr (GtkWidget) header = hook != NULL ? g_weak_ref_get (&hook->header_bar) : NULL;
      g_autoptr (GtkWidget) paned = hook != NULL ? g_weak_ref_get (&hook->paned) : NULL;
      g_message ("dp look: bottom=%d paned-vertical=%d height=%d header-line=%d", dh != NULL && dh->bottom,
                 paned != NULL && gtk_orientable_get_orientation (GTK_ORIENTABLE (paned)) == GTK_ORIENTATION_VERTICAL,
                 dh != NULL ? dh->height : 0, header != NULL && gtk_widget_has_css_class (header, "dp-header-line"));
      g_message ("dp dump: enabled=%d windows=%u state=%d width=%d shown=%d button=%d max-main-us=%" G_GINT64_FORMAT
                 " max-update-us=%" G_GINT64_FORMAT " %s", dh != NULL && dh->enabled,
                 windows, dh != NULL && dh->visible, dh != NULL ? dh->width : 0,
                 pane != NULL && gtk_widget_get_mapped (GTK_WIDGET (pane)),
                 button != NULL && gtk_widget_get_mapped (button), dp_pane_get_max_main_us (), max_update_us, desc);
    }
  else if (g_str_has_prefix (cmd, "dp-expect "))
    {
      g_autofree char *desc = pane != NULL ? dp_pane_describe (pane) : g_strdup ("");
      const char *want = cmd + strlen ("dp-expect ");
      gboolean ok = strstr (desc, want) != NULL;
      g_message ("dp expect %s: %s", ok ? "ok" : "FAIL", want);
      if (!ok)
        g_message ("dp expect FAIL detail: %s", desc);
    }
  else if (hook == NULL)
    g_message ("dp: no window");
  else if (g_str_equal (cmd, "dp-toggle"))
    gtk_widget_activate_action (hook->window, ACTION_PREFIX "." ACTION_NAME, NULL);
  else if (g_str_has_prefix (cmd, "dp-select "))
    debug_select (hook, cmd + strlen ("dp-select "));
  else if (g_str_equal (cmd, "dp-play"))
    g_message ("dp play: %d", pane != NULL && dp_pane_play (pane));
  else if (g_str_equal (cmd, "dp-stop") && pane != NULL)
    dp_pane_stop_media (pane);
  else if (g_str_has_prefix (cmd, "dp-tab "))
    {
      g_autoptr (GtkWidget) tab_view = g_weak_ref_get (&hook->tab_view);
      int n = (int) g_ascii_strtoll (cmd + strlen ("dp-tab "), NULL, 10);
      if (tab_view != NULL && n >= 0 && n < adw_tab_view_get_n_pages (ADW_TAB_VIEW (tab_view)))
        adw_tab_view_set_selected_page (ADW_TAB_VIEW (tab_view), adw_tab_view_get_nth_page (ADW_TAB_VIEW (tab_view), n));
    }
  else if (g_str_has_prefix (cmd, "dp-png "))
    {
      /* The whole window to a PNG (the owner's review of the look; cairo renders text off screen). */
      const char *out = cmd + strlen ("dp-png ");
      int w = gtk_widget_get_width (hook->window), h = gtk_widget_get_height (hook->window);
      g_autoptr (GdkPaintable) shot = gtk_widget_paintable_new (hook->window);
      GtkSnapshot *snap = gtk_snapshot_new (); /* consumed below */
      GdkRGBA bg = adw_style_manager_get_dark (adw_style_manager_get_default ()) ? (GdkRGBA) { 0.12f, 0.12f, 0.13f, 1 }
                                                                                 : (GdkRGBA) { 1, 1, 1, 1 };
      gtk_snapshot_append_color (snap, &bg, &GRAPHENE_RECT_INIT (0, 0, w, h));
      gdk_paintable_snapshot (shot, snap, w, h);
      g_autoptr (GskRenderNode) node = gtk_snapshot_free_to_node (snap);
      g_autoptr (GskRenderer) renderer = gsk_cairo_renderer_new ();
      gboolean ok = FALSE;
      if (node != NULL && gsk_renderer_realize_for_display (renderer, gtk_widget_get_display (hook->window), NULL))
        {
          g_autoptr (GdkTexture) tex = gsk_renderer_render_texture (renderer, node, &GRAPHENE_RECT_INIT (0, 0, w, h));
          ok = gdk_texture_save_to_png (tex, out);
          gsk_renderer_unrealize (renderer);
        }
      g_message ("dp png: %s %s", ok ? "saved" : "FAILED", out);
    }
  else if (g_str_equal (cmd, "dp-close-window"))
    gtk_window_close (GTK_WINDOW (hook->window));
  else if (g_str_has_prefix (cmd, "dp-width "))
    dp_hook_set_state (dh->visible, (int) g_ascii_strtoll (cmd + strlen ("dp-width "), NULL, 10), dh->height);
  else
    g_message ("dp: unknown command %s", cmd);
  return TRUE;
}
#endif
