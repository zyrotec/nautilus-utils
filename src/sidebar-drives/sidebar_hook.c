/* All unsupported Nautilus-internals code of the sidebar-drives feature lives here (PLAN.md §8.1.4, §8.6).
 *
 * How it works (verified on Nautilus 51.1, PLAN.md §8.6):
 *  - An emission hook on GtkWidget::map sees every NautilusSidebar / NautilusSidebarRow that appears,
 *    including all the rows Nautilus recreates on every sidebar rebuild, and schedules apply() for that
 *    sidebar in an idle (coalesced). apply() is idempotent.
 *  - Drive rows (rows with a GMount) get a vertical box: Nautilus' own label, a usage bar and a caption.
 *  - The OS drive row is our own NautilusSidebarRow (uri file:///, label "" so Nautilus' sort puts it first
 *    among the mounts, our own title label). It is not activatable, so Nautilus' activation code never sees
 *    it: our own gesture/keys open "/" through Nautilus' "slot.open-location" action. Nautilus' Computer
 *    row is left alone (owner decision S5, PLAN.md §8.4).
 *  - Network shares (S6): Nautilus sorts them above the local drives and its rows' section/place type can't
 *    be changed, so their Nautilus rows are hidden and shown as our own rows in our own GtkListBox below a
 *    separator, under the local drives. Click opens the share (slot.open-location); eject presses the hidden
 *    Nautilus row's eject button, so unmounting goes through Nautilus' own code and notifications.
 *
 * Lifetime rules (PLAN.md §4.1):
 *  - SidebarHook is qdata on its NautilusSidebar; RowDeco is qdata on its row. GLib frees them when the
 *    widget finalizes; neither touches global state when freed.
 *  - Async size results find their row through a GWeakRef and check that the row still waits for that
 *    exact query (its GCancellable), so a late result never lands on a recreated row.
 *  - Callbacks on widgets look state up through qdata instead of capturing pointers.
 */
#include "sidebar_hook.h"

#include <gtk/gtk.h>

#ifdef SD_DEBUG_DRIVER
#include <glib-unix.h>
#include <signal.h>
#endif

#include "config.h"
#include "drive_info.h"

#define SIDEBAR_KEY "sd-sidebar-hook"
#define DECO_KEY    "sd-row-deco"
#define OURS_KEY    "sd-our-row"
#define HIDDEN_KEY  "sd-net-hidden"   /* on Nautilus network rows we hid */
#define NET_URI_KEY "sd-net-uri"      /* on our network rows: the share's root URI (owned string) */

#define COMPUTER_URI "computer:///"   /* Nautilus' own Computer row: never touched */
#define OS_URI       "file:///"

/* If a Nautilus window is shown but no sidebar could be hooked within this time, Nautilus changed. */
#define DETECT_TIMEOUT_MS 3000

/* After a sidebar rebuild, re-check the OS row highlight this long after we re-inserted the row. */
#define REBUILD_SETTLE_MS 150

static const char sd_css[] =
  ".sd-drive-info { padding-top: 4px; padding-bottom: 4px; }\n"
  "levelbar.sd-usage-bar trough { min-height: " G_STRINGIFY (SD_BAR_HEIGHT_PX) "px; padding: 0; border: none;"
  "  border-radius: 99px; background-color: color-mix(in srgb, currentColor 15%, transparent); }\n"
  "levelbar.sd-usage-bar block { min-height: " G_STRINGIFY (SD_BAR_HEIGHT_PX) "px; border: none; border-radius: 99px; }\n"
  "levelbar.sd-usage-bar block.filled { background-color: var(--accent-bg-color); }\n"
  "levelbar.sd-usage-bar.sd-warning block.filled { background-color: var(--warning-bg-color); }\n"
  "levelbar.sd-usage-bar.sd-full block.filled { background-color: var(--error-bg-color); }\n"
  "levelbar.sd-usage-bar block.empty { background-color: transparent; }\n"
  /* Our network section sits between two lists instead of inside one, so the list padding and the box
   * spacing add up; these match its gaps to Nautilus' own separators (8 px above, 6 px below, measured on
   * Nautilus 51.1 / Adwaita, PLAN.md §8.8). */
  "list.sd-net-list { padding-top: 0; }\n"
  "separator.sd-net-separator { margin-top: 2px; }\n"
  "@media (prefers-contrast: more) {\n"
  "  levelbar.sd-usage-bar trough { outline: 1px solid currentColor; }\n"
  "}\n";

typedef struct
{
  guint           map_signal;
  gulong          map_hook_id;       /* Emission hook on GtkWidget::map; 0 once removed. */
  GType           sidebar_type;      /* NautilusSidebar, 0 until Nautilus registers it. */
  GType           row_type;          /* NautilusSidebarRow. */
  gboolean        validated;
  gboolean        disabled;
  int             section_mounts;    /* NAUTILUS_SIDEBAR_SECTION_MOUNTS */
  int             place_internal;    /* NAUTILUS_SIDEBAR_ROW_INTERNAL_MOUNT */
  GHashTable     *sidebars;          /* Set of SidebarHook* (borrowed: owned by the sidebars' qdata). */
  GtkCssProvider *css;               /* Owned. */
  GPtrArray      *css_displays;      /* Borrowed GdkDisplay* the provider was added to (displays outlive us). */
  char           *os_name;           /* Owned. */
  guint           detect_timer;      /* DETECT_TIMEOUT_MS watchdog. */
  guint           disable_idle;
  guint           n_hooked_total;
} Hook;

static Hook *sh; /* NULL when not initialised. Main thread only. */

typedef struct
{
  GtkWidget *sidebar;   /* Borrowed: we are its qdata, so we never outlive it. */
  GtkWidget *os_row;    /* Owned (sunk) ref: our OS drive row, kept across rebuilds. NULL until created. */
  guint      apply_id;  /* Pending apply idle, or 0. */
  guint      sync_id;   /* Pending selection-sync idle/timeout, or 0. */
  GWeakRef   slot;      /* The window's active NautilusWindowSlot whose "location" we watch. */
  GtkWidget  *net_sep;      /* Owned (sunk) ref: separator above the network shares, or NULL. */
  GtkWidget  *net_list;     /* Owned (sunk) ref: our GtkListBox of network shares, or NULL. */
  GHashTable *net_rows;     /* Root URI (owned) -> our network row (borrowed: owned by @net_list). */
  GHashTable *remote_uris;  /* Set of root URIs (owned) of native mounts that reported filesystem::remote. */
} SidebarHook;

typedef struct
{
  GtkWidget    *bar;      /* Borrowed: child of the row, lives as long as the row (our owner). */
  GtkWidget    *caption;  /* Borrowed: same. */
  GCancellable *query;    /* Owned: the size query this row waits for, or NULL. */
} RowDeco;

static void disable (const char *reason);
static void schedule_apply (SidebarHook *hook);

/* Name of a Nautilus type/property we depend on. Debug-driver builds can simulate a Nautilus that renamed
 * things ($SD_DEBUG_BREAK=types|props) to test the graceful-failure path; release builds use @real. */
static const char *
internal_name (const char *real,
               const char *kind)
{
#ifdef SD_DEBUG_DRIVER
  if (g_strcmp0 (g_getenv ("SD_DEBUG_BREAK"), kind) == 0)
    return "sd-debug-renamed";
#else
  (void) kind;
#endif
  return real;
}

/* ------------------------------------------------------------------------------------------------ */
/* Small helpers                                                                                     */

static void
collect_rows (GtkWidget *w, GPtrArray *out)
{
  if (G_TYPE_CHECK_INSTANCE_TYPE (w, sh->row_type))
    {
      g_ptr_array_add (out, w);
      return; /* rows don't nest */
    }
  for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL; c = gtk_widget_get_next_sibling (c))
    collect_rows (c, out);
}

static gboolean
is_ours (GtkWidget *row)
{
  return g_object_get_data (G_OBJECT (row), OURS_KEY) != NULL;
}

/* Caller owns the returned string (g_free); may be NULL. */
static char *
row_uri (GtkWidget *row)
{
  char *uri = NULL;
  g_object_get (row, "uri", &uri, NULL);
  return uri;
}

/* Caller owns the returned mount (g_object_unref); NULL for non-mount rows. */
static GMount *
row_mount (GtkWidget *row)
{
  GMount *mount = NULL;
  g_object_get (row, "mount", &mount, NULL);
  return mount;
}

static void
ensure_css (GtkWidget *widget)
{
  GdkDisplay *display = gtk_widget_get_display (widget);
  if (sh->css == NULL)
    {
      sh->css = gtk_css_provider_new ();
      gtk_css_provider_load_from_string (sh->css, sd_css);
    }
  if (!g_ptr_array_find (sh->css_displays, display, NULL))
    {
      gtk_style_context_add_provider_for_display (display, GTK_STYLE_PROVIDER (sh->css),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
      g_ptr_array_add (sh->css_displays, display);
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Row decoration: name, usage bar, caption                                                          */

static void
row_deco_free (gpointer data)
{
  RowDeco *deco = data;
  if (deco->query != NULL)
    g_cancellable_cancel (deco->query);
  g_clear_object (&deco->query);
  g_free (deco);
}

/* Adds the bar + caption under the row's name. @title NULL = keep Nautilus' label (moved into our box);
 * otherwise Nautilus' label is hidden and @title shown instead. Returns the decoration (borrowed: owned by
 * the row's qdata), or NULL if the row doesn't have the structure we expect (it is then left untouched). */
static RowDeco *
decorate (GtkWidget  *row,
          const char *title)
{
  GtkWidget *revealer = gtk_widget_get_first_child (row);
  GtkWidget *box = GTK_IS_REVEALER (revealer) ? gtk_revealer_get_child (GTK_REVEALER (revealer)) : NULL;
  if (!GTK_IS_BOX (box))
    return NULL;
  GtkWidget *label = NULL;
  for (GtkWidget *c = gtk_widget_get_first_child (box); c != NULL; c = gtk_widget_get_next_sibling (c))
    if (GTK_IS_LABEL (c))
      {
        label = c;
        break;
      }
  if (label == NULL)
    return NULL;

  GtkWidget *info = gtk_box_new (GTK_ORIENTATION_VERTICAL, 3);
  gtk_widget_add_css_class (info, "sd-drive-info");
  gtk_widget_set_hexpand (info, TRUE);
  gtk_widget_set_valign (info, GTK_ALIGN_CENTER);

  if (title != NULL)
    {
      GtkWidget *mine = gtk_label_new (title);
      gtk_label_set_xalign (GTK_LABEL (mine), 0.0f);
      gtk_label_set_ellipsize (GTK_LABEL (mine), PANGO_ELLIPSIZE_END);
      gtk_box_append (GTK_BOX (info), mine);
      gtk_widget_set_visible (label, FALSE);
      gtk_box_insert_child_after (GTK_BOX (box), info, label);
    }
  else
    {
      /* Move Nautilus' label into our box at the same position; Nautilus keeps updating it. */
      GtkWidget *prev = gtk_widget_get_prev_sibling (label);
      g_object_ref (label);
      gtk_box_remove (GTK_BOX (box), label);
      gtk_box_append (GTK_BOX (info), label);
      g_object_unref (label);
      gtk_box_insert_child_after (GTK_BOX (box), info, prev);
    }

  RowDeco *deco = g_new0 (RowDeco, 1);
  deco->bar = gtk_level_bar_new_for_interval (0.0, 1.0);
  /* Our own colour states replace GTK's low/high/full offsets (PLAN.md §8.1.1). */
  gtk_level_bar_remove_offset_value (GTK_LEVEL_BAR (deco->bar), GTK_LEVEL_BAR_OFFSET_LOW);
  gtk_level_bar_remove_offset_value (GTK_LEVEL_BAR (deco->bar), GTK_LEVEL_BAR_OFFSET_HIGH);
  gtk_level_bar_remove_offset_value (GTK_LEVEL_BAR (deco->bar), GTK_LEVEL_BAR_OFFSET_FULL);
  gtk_widget_add_css_class (deco->bar, "sd-usage-bar");
  gtk_accessible_update_property (GTK_ACCESSIBLE (deco->bar), GTK_ACCESSIBLE_PROPERTY_LABEL, "Disk usage", -1);
  gtk_widget_set_visible (deco->bar, FALSE);
  gtk_box_append (GTK_BOX (info), deco->bar);

  deco->caption = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (deco->caption), 0.0f);
  gtk_label_set_ellipsize (GTK_LABEL (deco->caption), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (deco->caption, "caption");
  gtk_widget_add_css_class (deco->caption, "dim-label");
  gtk_widget_set_visible (deco->caption, FALSE);
  gtk_box_append (GTK_BOX (info), deco->caption);

  g_object_set_data_full (G_OBJECT (row), DECO_KEY, deco, row_deco_free);
  return deco;
}

static void
show_usage (GtkWidget     *row,
            RowDeco       *deco,
            const SdUsage *usage)
{
  if (usage == NULL || !usage->known)
    {
      /* Network server without size info (S2), or the query failed: name only. */
      gtk_widget_set_visible (deco->bar, FALSE);
      gtk_widget_set_visible (deco->caption, FALSE);
      gtk_accessible_reset_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION);
      return;
    }
  double fraction = sd_fill_fraction (usage->size, usage->free);
  SdFillState state = sd_fill_state (fraction);
  g_autofree char *text = sd_usage_text (usage->size, usage->free);

  gtk_level_bar_set_value (GTK_LEVEL_BAR (deco->bar), MAX (fraction, 0.0));
  if (state == SD_FILL_WARNING)
    gtk_widget_add_css_class (deco->bar, "sd-warning");
  else
    gtk_widget_remove_css_class (deco->bar, "sd-warning");
  if (state == SD_FILL_FULL)
    gtk_widget_add_css_class (deco->bar, "sd-full");
  else
    gtk_widget_remove_css_class (deco->bar, "sd-full");
  gtk_label_set_text (GTK_LABEL (deco->caption), text);
  gtk_widget_set_visible (deco->bar, TRUE);
  gtk_widget_set_visible (deco->caption, TRUE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, text, -1);
}

static void note_remote (GtkWidget *row, GFile *root, gboolean remote);


typedef struct
{
  GWeakRef      row;     /* The row the query is for. */
  GCancellable *query;   /* Owned ref: identifies the query in the row's RowDeco. */
} QueryCtx;

static void
query_ctx_free (QueryCtx *ctx)
{
  g_weak_ref_clear (&ctx->row);
  g_clear_object (&ctx->query);
  g_free (ctx);
}

static void
on_usage (GObject      *source,
          GAsyncResult *result,
          gpointer      user_data)
{
  QueryCtx *ctx = user_data; /* Owned: freed here. */
  g_autoptr (GError) error = NULL;
  g_autoptr (SdUsage) usage = sd_query_usage_finish (G_FILE (source), result, &error);
  g_autoptr (GtkWidget) row = g_weak_ref_get (&ctx->row);
  RowDeco *deco = row != NULL ? g_object_get_data (G_OBJECT (row), DECO_KEY) : NULL;

  /* Stale: the row is gone, was recreated, or started a newer query. */
  if (deco == NULL || deco->query != ctx->query || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      query_ctx_free (ctx);
      return;
    }
  g_clear_object (&deco->query);
  if (error != NULL)
    {
      g_autofree char *uri = g_file_get_uri (G_FILE (source));
      g_debug ("size of %s unavailable: %s", uri, error->message);
    }
  show_usage (row, deco, usage);
  if (usage != NULL)
    note_remote (row, G_FILE (source), usage->remote);
  query_ctx_free (ctx);
}

static void
start_query (GtkWidget *row,
             RowDeco   *deco,
             GFile     *root)
{
  if (deco->query != NULL)
    g_cancellable_cancel (deco->query);
  g_clear_object (&deco->query);
  deco->query = g_cancellable_new ();

  QueryCtx *ctx = g_new0 (QueryCtx, 1); /* Freed in on_usage. */
  g_weak_ref_init (&ctx->row, row);
  ctx->query = g_object_ref (deco->query);
  sd_query_usage_async (root, SD_NET_TIMEOUT_MS, deco->query, on_usage, ctx);
}

/* ------------------------------------------------------------------------------------------------ */
/* OS drive row                                                                                      */

static GtkWidget *
sidebar_of (GtkWidget *w)
{
  return sh != NULL && sh->sidebar_type != 0 ? gtk_widget_get_ancestor (w, sh->sidebar_type) : NULL;
}

/* Opens "/" in the current tab through Nautilus' own "slot.open-location" action (a string URI), which the
 * window resolves for its active slot — the same navigation a sidebar click does. The highlight follows
 * from the location change (sync_selection). */
static void
activate_os_row (GtkWidget *os_row)
{
  if (!gtk_widget_activate_action (os_row, "slot.open-location", "s", OS_URI))
    {
      static gboolean warned;
      if (!warned)
        g_warning ("cannot open %s: Nautilus' slot.open-location action is missing", OS_URI);
      warned = TRUE;
    }
}

/* Claim every press on our row so Nautilus' list/sidebar gestures (activation, context menu, long press)
 * never get it; only a primary click opens "/". */
static void
on_os_pressed (GtkGestureClick *gesture,
               int              n_press,
               double           x,
               double           y,
               gpointer         data)
{
  (void) n_press; (void) x; (void) y; (void) data;
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_os_released (GtkGestureClick *gesture,
                int              n_press,
                double           x,
                double           y,
                gpointer         data)
{
  (void) n_press; (void) x; (void) y; (void) data;
  if (gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture)) != GDK_BUTTON_PRIMARY)
    return;
  activate_os_row (gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture)));
}

static gboolean
on_os_key (GtkEventControllerKey *controller,
           guint                  keyval,
           guint                  keycode,
           GdkModifierType        state,
           gpointer               data)
{
  (void) keycode; (void) data;
  if ((state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SHIFT_MASK)) != 0)
    return FALSE;
  if (keyval != GDK_KEY_Return && keyval != GDK_KEY_KP_Enter && keyval != GDK_KEY_ISO_Enter &&
      keyval != GDK_KEY_space && keyval != GDK_KEY_KP_Space)
    return FALSE;
  activate_os_row (gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller)));
  return TRUE;
}

/* Our drop target takes drops on our row so Nautilus' list drop handler never sees our row, and refuses
 * them (no action): nothing can be dropped on the OS drive row. */
static GdkDragAction
on_os_drop_motion (GtkDropTarget *target, double x, double y, gpointer data)
{
  (void) target; (void) x; (void) y; (void) data;
  return 0;
}

static gboolean
on_os_drop (GtkDropTarget *target, const GValue *value, double x, double y, gpointer data)
{
  (void) target; (void) value; (void) x; (void) y; (void) data;
  return FALSE;
}

/* Caller owns the returned (sunk) row. NULL if Nautilus refused to build it. */
static GtkWidget *
create_os_row (GtkWidget *sidebar)
{
  g_autoptr (GIcon) icon = g_themed_icon_new ("drive-harddisk-system-symbolic");
  GtkWidget *row = g_object_new (sh->row_type,
                                 "sidebar", sidebar,
                                 "label", "",            /* sorts first among the mounts (PLAN.md §8.6) */
                                 "tooltip", sh->os_name,
                                 "uri", OS_URI,
                                 "start-icon", icon,
                                 "section-type", sh->section_mounts,
                                 "place-type", sh->place_internal,
                                 "ejectable", FALSE,
                                 NULL);
  g_object_ref_sink (row);
  g_object_set_data (G_OBJECT (row), OURS_KEY, GINT_TO_POINTER (1));
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, sh->os_name, -1);

  GtkGesture *click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);
  g_signal_connect (click, "pressed", G_CALLBACK (on_os_pressed), NULL);
  g_signal_connect (click, "released", G_CALLBACK (on_os_released), NULL);
  gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (click)); /* row owns it */

  GtkEventController *keys = gtk_event_controller_key_new ();
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_os_key), NULL);
  gtk_widget_add_controller (row, keys);

  GType drop_types[] = { GDK_TYPE_FILE_LIST, sh->row_type };
  GtkDropTarget *drop = gtk_drop_target_new (G_TYPE_INVALID, GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK);
  gtk_drop_target_set_gtypes (drop, drop_types, G_N_ELEMENTS (drop_types));
  g_signal_connect (drop, "enter", G_CALLBACK (on_os_drop_motion), NULL);
  g_signal_connect (drop, "motion", G_CALLBACK (on_os_drop_motion), NULL);
  g_signal_connect (drop, "drop", G_CALLBACK (on_os_drop), NULL);
  gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (drop));

  if (decorate (row, sh->os_name) == NULL)
    {
      g_object_unref (row);
      return NULL;
    }
  return row;
}

/* The window's current location (NautilusWindow:active-slot -> NautilusWindowSlot:location), or NULL.
 * Caller owns the returned string (g_free). */
static char *
current_location (SidebarHook *hook)
{
  g_autoptr (GObject) slot = g_weak_ref_get (&hook->slot);
  if (slot == NULL || g_object_class_find_property (G_OBJECT_GET_CLASS (slot), "location") == NULL)
    return NULL;
  g_autoptr (GFile) location = NULL;
  g_object_get (slot, "location", &location, NULL);
  return location != NULL ? g_file_get_uri (location) : NULL;
}

/* Our row is highlighted exactly while the window shows "/" (PLAN.md §8.7). Nautilus clears the selection
 * on every rebuild, so this runs after every selection change and location change. */
static void sync_net_selection (SidebarHook *hook, const char *uri);

static void
sync_selection (SidebarHook *hook)
{
  g_autofree char *uri = current_location (hook);
  sync_net_selection (hook, uri);
  GtkWidget *list = hook->os_row != NULL ? gtk_widget_get_parent (hook->os_row) : NULL;
  if (!GTK_IS_LIST_BOX (list))
    return;
  gboolean ours = gtk_list_box_get_selected_row (GTK_LIST_BOX (list)) == GTK_LIST_BOX_ROW (hook->os_row);
  if (g_strcmp0 (uri, OS_URI) == 0)
    {
      if (!ours)
        {
          /* After a rebuild Nautilus forgets its own location (NautilusSidebar:location becomes NULL) and
           * then rejects any selection; give it back the window's location first. */
          GParamSpec *ps = g_object_class_find_property (G_OBJECT_GET_CLASS (hook->sidebar), "location");
          g_autoptr (GFile) sidebar_location = NULL;
          if (ps != NULL && (ps->flags & G_PARAM_WRITABLE) && G_IS_PARAM_SPEC_OBJECT (ps))
            {
              g_object_get (hook->sidebar, "location", &sidebar_location, NULL);
              if (sidebar_location == NULL)
                {
                  g_autoptr (GFile) root = g_file_new_for_uri (OS_URI);
                  g_object_set (hook->sidebar, "location", root, NULL);
                }
            }
          if (gtk_list_box_get_selected_row (GTK_LIST_BOX (list)) != GTK_LIST_BOX_ROW (hook->os_row))
            gtk_list_box_select_row (GTK_LIST_BOX (list), GTK_LIST_BOX_ROW (hook->os_row));
        }
    }
  else if (ours)
    gtk_list_box_unselect_row (GTK_LIST_BOX (list), GTK_LIST_BOX_ROW (hook->os_row));
}

static gboolean
sync_idle (gpointer data)
{
  SidebarHook *hook = data; /* Borrowed: sidebar_hook_free removes this idle. */
  hook->sync_id = 0;
  if (sh != NULL && !sh->disabled)
    sync_selection (hook);
  return G_SOURCE_REMOVE;
}

/* "selected-rows-changed" on either list; @sidebar is the connect_object target (lives as long as the lists). */
static void
on_selection_changed (GtkListBox *list,
                      gpointer    sidebar)
{
  (void) list;
  SidebarHook *hook = g_object_get_data (G_OBJECT (sidebar), SIDEBAR_KEY);
  if (hook != NULL && hook->sync_id == 0)
    hook->sync_id = g_idle_add (sync_idle, hook);
}

static void
watch_selection (SidebarHook *hook,
                 GtkWidget   *list)
{
  if (!GTK_IS_LIST_BOX (list) || g_object_get_data (G_OBJECT (list), "sd-watched") != NULL)
    return;
  g_object_set_data (G_OBJECT (list), "sd-watched", GINT_TO_POINTER (1));
  g_signal_connect_object (list, "selected-rows-changed", G_CALLBACK (on_selection_changed), hook->sidebar, 0);
}

/* "notify::location" on the active slot; @sidebar is the connect_object target. */
static void
on_slot_location (GObject    *slot,
                  GParamSpec *pspec,
                  gpointer    sidebar)
{
  (void) slot; (void) pspec;
  on_selection_changed (NULL, sidebar);
}

/* Follows the window's active slot (tabs): watch the location of whichever slot is active. */
static void
watch_slot (SidebarHook *hook)
{
  GtkRoot *window = gtk_widget_get_root (hook->sidebar);
  if (window == NULL || g_object_class_find_property (G_OBJECT_GET_CLASS (window), "active-slot") == NULL)
    return; /* no highlight then; everything else keeps working */
  g_autoptr (GObject) slot = NULL;
  g_object_get (window, "active-slot", &slot, NULL);
  g_autoptr (GObject) old = g_weak_ref_get (&hook->slot);
  if (old == slot)
    return;
  if (old != NULL)
    g_signal_handlers_disconnect_by_func (old, G_CALLBACK (on_slot_location), hook->sidebar);
  g_weak_ref_set (&hook->slot, slot);
  if (slot != NULL)
    g_signal_connect_object (slot, "notify::location", G_CALLBACK (on_slot_location), hook->sidebar, 0);
}

static void
on_active_slot (GObject    *window,
                GParamSpec *pspec,
                gpointer    sidebar)
{
  (void) window; (void) pspec;
  SidebarHook *hook = g_object_get_data (G_OBJECT (sidebar), SIDEBAR_KEY);
  if (hook == NULL)
    return;
  watch_slot (hook);
  sync_selection (hook);
}

static void
unwatch_selection (GtkWidget *sidebar,
                   GtkWidget *list)
{
  if (!GTK_IS_LIST_BOX (list))
    return;
  g_signal_handlers_disconnect_by_func (list, G_CALLBACK (on_selection_changed), sidebar);
  g_object_set_data (G_OBJECT (list), "sd-watched", NULL);
}

/* ------------------------------------------------------------------------------------------------ */
/* Network shares (S6, PLAN.md §8.8)                                                                 */

/* Debug-driver builds can pretend a local drive is a network share ($SD_DEBUG_FAKE_NETWORK = its label) to
 * test the network section without a server. Never set in release builds. */
static gboolean
debug_fake_network (GtkWidget *row)
{
#ifdef SD_DEBUG_DRIVER
  const char *fake = g_getenv ("SD_DEBUG_FAKE_NETWORK");
  if (fake == NULL || *fake == '\0')
    return FALSE;
  g_autofree char *label = NULL;
  g_object_get (row, "label", &label, NULL);
  return g_strcmp0 (label, fake) == 0;
#else
  (void) row;
  return FALSE;
#endif
}

/* A Nautilus mount row is a network share if its location isn't local (smb://, sftp://, …) or its query
 * reported filesystem::remote (NFS/CIFS mounted by the kernel). */
static gboolean
is_network_row (SidebarHook *hook,
                GtkWidget   *row,
                GFile       *root)
{
  g_autofree char *uri = g_file_get_uri (root);
  gboolean remote = g_hash_table_contains (hook->remote_uris, uri);
  return sd_classify (g_file_is_native (root), remote) == SD_DRIVE_NETWORK || debug_fake_network (row);
}

static void
note_remote (GtkWidget *row,
             GFile     *root,
             gboolean   remote)
{
  if (!remote || !g_file_is_native (root) || g_object_get_data (G_OBJECT (row), NET_URI_KEY) != NULL)
    return; /* already known as network (non-native), or one of our network rows */
  GtkWidget *sidebar = sidebar_of (row);
  SidebarHook *hook = sidebar != NULL ? g_object_get_data (G_OBJECT (sidebar), SIDEBAR_KEY) : NULL;
  if (hook == NULL)
    return;
  g_autofree char *uri = g_file_get_uri (root);
  if (g_hash_table_add (hook->remote_uris, g_steal_pointer (&uri)))
    schedule_apply (hook);
}

/* Nautilus' row for the mount at @uri in @sidebar (possibly hidden by us), or NULL. Borrowed. */
static GtkWidget *
find_nautilus_mount_row (GtkWidget  *sidebar,
                         const char *uri)
{
  g_autoptr (GPtrArray) rows = g_ptr_array_new ();
  collect_rows (sidebar, rows);
  for (guint i = 0; i < rows->len; i++)
    {
      GtkWidget *row = g_ptr_array_index (rows, i);
      if (is_ours (row))
        continue;
      g_autoptr (GMount) mount = row_mount (row);
      if (mount == NULL)
        continue;
      g_autoptr (GFile) root = g_mount_get_root (mount);
      g_autofree char *root_uri = g_file_get_uri (root);
      if (g_strcmp0 (root_uri, uri) == 0)
        return row;
    }
  return NULL;
}

static GtkWidget *
find_child (GtkWidget *w,
            GType      type)
{
  if (G_TYPE_CHECK_INSTANCE_TYPE (w, type))
    return w;
  for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL; c = gtk_widget_get_next_sibling (c))
    {
      GtkWidget *r = find_child (c, type);
      if (r != NULL)
        return r;
    }
  return NULL;
}

/* Eject on our row = press the eject button of Nautilus' (hidden) row, so the unmount runs through
 * Nautilus' own code (busy/error dialogs, notifications). @sidebar is the connect_object target. */
static void
on_net_eject (GtkButton *button,
              gpointer   sidebar)
{
  GtkWidget *row = gtk_widget_get_ancestor (GTK_WIDGET (button), GTK_TYPE_LIST_BOX_ROW);
  const char *uri = row != NULL ? g_object_get_data (G_OBJECT (row), NET_URI_KEY) : NULL;
  GtkWidget *nrow = uri != NULL ? find_nautilus_mount_row (sidebar, uri) : NULL;
  GtkWidget *nbutton = nrow != NULL ? find_child (nrow, GTK_TYPE_BUTTON) : NULL;
  if (nbutton == NULL)
    {
      g_warning ("cannot unmount %s: Nautilus' row or its eject button is missing", uri != NULL ? uri : "(?)");
      return;
    }
  g_signal_emit_by_name (nbutton, "clicked");
}

/* @sidebar is the connect_object target. */
static void
on_net_activated (GtkListBox    *list,
                  GtkListBoxRow *row,
                  gpointer       sidebar)
{
  (void) list; (void) sidebar;
  const char *uri = g_object_get_data (G_OBJECT (row), NET_URI_KEY);
  if (uri != NULL && !gtk_widget_activate_action (GTK_WIDGET (row), "slot.open-location", "s", uri))
    g_warning ("cannot open %s: Nautilus' slot.open-location action is missing", uri);
}

static int
net_sort (GtkListBoxRow *a,
          GtkListBoxRow *b,
          gpointer       data)
{
  (void) data;
  const char *ta = g_object_get_data (G_OBJECT (a), "sd-net-title");
  const char *tb = g_object_get_data (G_OBJECT (b), "sd-net-title");
  return g_utf8_collate (ta != NULL ? ta : "", tb != NULL ? tb : "");
}

static void
sync_net_selection (SidebarHook *hook,
                    const char  *uri)
{
  if (hook->net_list == NULL)
    return;
  GtkWidget *row = uri != NULL ? g_hash_table_lookup (hook->net_rows, uri) : NULL;
  GtkListBoxRow *selected = gtk_list_box_get_selected_row (GTK_LIST_BOX (hook->net_list));
  if (row != NULL && selected != GTK_LIST_BOX_ROW (row))
    gtk_list_box_select_row (GTK_LIST_BOX (hook->net_list), GTK_LIST_BOX_ROW (row));
  else if (row == NULL && selected != NULL)
    gtk_list_box_unselect_all (GTK_LIST_BOX (hook->net_list));
}

/* Creates our separator + list once and keeps them right after Nautilus' main list @target. FALSE if the
 * sidebar isn't laid out as expected (the network section is then skipped; everything else works). */
static gboolean
ensure_net_section (SidebarHook *hook,
                    GtkWidget   *target)
{
  GtkWidget *box = gtk_widget_get_parent (target);
  if (!GTK_IS_BOX (box))
    return FALSE;
  if (hook->net_list == NULL)
    {
      /* Separator styled like Nautilus' own section separators (its list row headers). */
      hook->net_sep = g_object_ref_sink (gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
      gtk_widget_add_css_class (hook->net_sep, "sd-net-separator");
      for (GtkWidget *c = gtk_widget_get_first_child (target); c != NULL; c = gtk_widget_get_next_sibling (c))
        {
          GtkWidget *header = GTK_IS_LIST_BOX_ROW (c) ? gtk_list_box_row_get_header (GTK_LIST_BOX_ROW (c)) : NULL;
          if (GTK_IS_SEPARATOR (header))
            {
              gtk_widget_set_margin_top (hook->net_sep, gtk_widget_get_margin_top (header));
              gtk_widget_set_margin_bottom (hook->net_sep, gtk_widget_get_margin_bottom (header));
              gtk_widget_set_margin_start (hook->net_sep, gtk_widget_get_margin_start (header));
              gtk_widget_set_margin_end (hook->net_sep, gtk_widget_get_margin_end (header));
              g_auto (GStrv) classes = gtk_widget_get_css_classes (header);
              for (char **k = classes; k != NULL && *k != NULL; k++)
                gtk_widget_add_css_class (hook->net_sep, *k);
              break;
            }
        }

      hook->net_list = g_object_ref_sink (gtk_list_box_new ());
      g_auto (GStrv) classes = gtk_widget_get_css_classes (target);
      for (char **k = classes; k != NULL && *k != NULL; k++)
        gtk_widget_add_css_class (hook->net_list, *k);
      gtk_widget_add_css_class (hook->net_list, "sd-net-list");
      gtk_list_box_set_selection_mode (GTK_LIST_BOX (hook->net_list), GTK_SELECTION_SINGLE);
      gtk_list_box_set_sort_func (GTK_LIST_BOX (hook->net_list), net_sort, NULL, NULL);
      gtk_accessible_update_property (GTK_ACCESSIBLE (hook->net_list), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      "Network shares", -1);
      g_signal_connect_object (hook->net_list, "row-activated", G_CALLBACK (on_net_activated), hook->sidebar, 0);
      g_signal_connect_object (hook->net_list, "selected-rows-changed", G_CALLBACK (on_selection_changed),
                               hook->sidebar, 0);
      gtk_widget_set_visible (hook->net_sep, FALSE);
      gtk_widget_set_visible (hook->net_list, FALSE);
    }
  if (gtk_widget_get_parent (hook->net_list) != box || gtk_widget_get_prev_sibling (hook->net_sep) != target)
    {
      if (gtk_widget_get_parent (hook->net_sep) != NULL)
        gtk_box_remove (GTK_BOX (gtk_widget_get_parent (hook->net_sep)), hook->net_sep);
      if (gtk_widget_get_parent (hook->net_list) != NULL)
        gtk_box_remove (GTK_BOX (gtk_widget_get_parent (hook->net_list)), hook->net_list);
      gtk_box_insert_child_after (GTK_BOX (box), hook->net_sep, target);
      gtk_box_insert_child_after (GTK_BOX (box), hook->net_list, hook->net_sep);
    }
  return TRUE;
}

static void
copy_margins (GtkWidget *to,
              GtkWidget *from)
{
  if (from == NULL)
    return;
  gtk_widget_set_margin_start (to, gtk_widget_get_margin_start (from));
  gtk_widget_set_margin_end (to, gtk_widget_get_margin_end (from));
  gtk_widget_set_margin_top (to, gtk_widget_get_margin_top (from));
  gtk_widget_set_margin_bottom (to, gtk_widget_get_margin_bottom (from));
}

/* Our row for a network share, built like Nautilus' rows (row > revealer.sidebar-revealer > box) so the
 * sidebar's CSS applies, with spacing/margins and button classes copied from Nautilus' row @nrow. */
static GtkWidget *
create_net_row (SidebarHook *hook,
                GtkWidget   *nrow,
                GFile       *root,
                const char  *uri)
{
  g_autofree char *title = NULL, *tooltip = NULL, *eject_tooltip = NULL;
  g_object_get (nrow, "label", &title, "tooltip", &tooltip, "eject-tooltip", &eject_tooltip, NULL);

  GtkWidget *nrevealer = gtk_widget_get_first_child (nrow);
  GtkWidget *nbox = GTK_IS_REVEALER (nrevealer) ? gtk_revealer_get_child (GTK_REVEALER (nrevealer)) : NULL;
  GtkWidget *nbutton = find_child (nrow, GTK_TYPE_BUTTON);
  GtkWidget *nimage = NULL, *nlabel = NULL;
  for (GtkWidget *c = nbox != NULL ? gtk_widget_get_first_child (nbox) : NULL; c != NULL; c = gtk_widget_get_next_sibling (c))
    {
      if (nimage == NULL && GTK_IS_IMAGE (c))
        nimage = c;
      if (nlabel == NULL && GTK_IS_LABEL (c))
        nlabel = c;
    }
  if (nlabel == NULL && nbox != NULL)
    {
      /* Already decorated by us: Nautilus' label sits inside our info box. */
      GtkWidget *info = find_child (nbox, GTK_TYPE_LABEL);
      nlabel = info;
    }

  GtkWidget *row = gtk_list_box_row_new ();
  g_object_set_data_full (G_OBJECT (row), NET_URI_KEY, g_strdup (uri), g_free);
  g_object_set_data_full (G_OBJECT (row), "sd-net-title", g_strdup (title != NULL ? title : uri), g_free);
  gtk_widget_set_tooltip_text (row, tooltip != NULL ? tooltip : uri);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, title != NULL ? title : uri, -1);

  GtkWidget *revealer = gtk_revealer_new ();
  gtk_widget_add_css_class (revealer, "sidebar-revealer");
  gtk_revealer_set_transition_type (GTK_REVEALER (revealer), GTK_REVEALER_TRANSITION_TYPE_NONE);
  gtk_revealer_set_reveal_child (GTK_REVEALER (revealer), TRUE);
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), revealer);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, GTK_IS_BOX (nbox) ? gtk_box_get_spacing (GTK_BOX (nbox)) : 6);
  if (nbox != NULL)
    {
      gtk_widget_set_margin_top (box, gtk_widget_get_margin_top (nbox));
      gtk_widget_set_margin_bottom (box, gtk_widget_get_margin_bottom (nbox));
      gtk_widget_set_margin_start (box, gtk_widget_get_margin_start (nbox));
      gtk_widget_set_margin_end (box, gtk_widget_get_margin_end (nbox));
    }
  gtk_revealer_set_child (GTK_REVEALER (revealer), box);

  /* Always the network-share icon: every row in this list is a network share. (Reusing the icon of
   * Nautilus' row, or a drive icon, rendered blank in our widget on Nautilus 51.1 — PLAN.md §8.8.) */
  GtkWidget *image = gtk_image_new_from_icon_name ("folder-remote-symbolic");
  if (nimage != NULL)
    {
      gtk_image_set_icon_size (GTK_IMAGE (image), gtk_image_get_icon_size (GTK_IMAGE (nimage)));
      if (gtk_image_get_pixel_size (GTK_IMAGE (nimage)) > 0)
        gtk_image_set_pixel_size (GTK_IMAGE (image), gtk_image_get_pixel_size (GTK_IMAGE (nimage)));
    }
  copy_margins (image, nimage);
  gtk_box_append (GTK_BOX (box), image);
  GtkWidget *label = gtk_label_new (title != NULL ? title : uri);
  copy_margins (label, nlabel);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (box), label);

  GtkWidget *eject = gtk_button_new_from_icon_name ("media-eject-symbolic");
  if (nbutton != NULL)
    {
      g_auto (GStrv) classes = gtk_widget_get_css_classes (nbutton);
      for (char **k = classes; k != NULL && *k != NULL; k++)
        gtk_widget_add_css_class (eject, *k);
    }
  gtk_widget_set_valign (eject, GTK_ALIGN_CENTER);
  copy_margins (eject, nbutton);
  gtk_widget_set_tooltip_text (eject, eject_tooltip != NULL ? eject_tooltip : "Unmount");
  gtk_accessible_update_property (GTK_ACCESSIBLE (eject), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  eject_tooltip != NULL ? eject_tooltip : "Unmount", -1);
  g_signal_connect_object (eject, "clicked", G_CALLBACK (on_net_eject), hook->sidebar, 0);
  gtk_box_append (GTK_BOX (box), eject);

  RowDeco *deco = decorate (row, NULL); /* moves our label into name + bar + caption */
  gtk_list_box_insert (GTK_LIST_BOX (hook->net_list), row, -1);
  g_hash_table_insert (hook->net_rows, g_strdup (uri), row);
  if (deco != NULL)
    start_query (row, deco, root);
  return row;
}

/* Hides Nautilus' network rows and keeps our network list in step with them. */
static void
reconcile_network (SidebarHook *hook,
                   GtkWidget   *target,
                   GPtrArray   *rows)
{
  g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < rows->len; i++)
    {
      GtkWidget *row = g_ptr_array_index (rows, i);
      if (is_ours (row))
        continue;
      g_autoptr (GMount) mount = row_mount (row);
      if (mount == NULL)
        continue;
      g_autoptr (GFile) root = g_mount_get_root (mount);
      if (!is_network_row (hook, row, root))
        continue;
      if (!ensure_net_section (hook, target))
        return;
      g_autofree char *uri = g_file_get_uri (root);
      gtk_widget_set_visible (row, FALSE);
      g_object_set_data (G_OBJECT (row), HIDDEN_KEY, GINT_TO_POINTER (1));
      if (!g_hash_table_contains (hook->net_rows, uri))
        create_net_row (hook, row, root, uri);
      g_hash_table_add (seen, g_steal_pointer (&uri));
    }
  if (hook->net_list == NULL)
    return;

  /* Drop our rows for shares that are gone (unmounted). */
  g_autoptr (GPtrArray) gone = g_ptr_array_new_with_free_func (g_free);
  GHashTableIter it;
  gpointer key;
  g_hash_table_iter_init (&it, hook->net_rows);
  while (g_hash_table_iter_next (&it, &key, NULL))
    if (!g_hash_table_contains (seen, key))
      g_ptr_array_add (gone, g_strdup (key));
  for (guint i = 0; i < gone->len; i++)
    {
      const char *uri = g_ptr_array_index (gone, i);
      GtkWidget *row = g_hash_table_lookup (hook->net_rows, uri);
      g_hash_table_remove (hook->net_rows, uri);
      gtk_list_box_remove (GTK_LIST_BOX (hook->net_list), row);
      g_hash_table_remove (hook->remote_uris, uri);
    }

  gboolean any = g_hash_table_size (hook->net_rows) > 0;
  gtk_widget_set_visible (hook->net_sep, any);
  gtk_widget_set_visible (hook->net_list, any);
}

/* ------------------------------------------------------------------------------------------------ */
/* Per-sidebar apply                                                                                 */

static void
apply (SidebarHook *hook)
{
  g_autoptr (GPtrArray) rows = g_ptr_array_new ();
  collect_rows (hook->sidebar, rows);

  /* Find the list that holds the places, bookmarks and drives (not the Computer row's own list): that's
   * where our OS row goes. */
  GtkWidget *target = NULL;
  for (guint i = 0; i < rows->len; i++)
    {
      GtkWidget *row = g_ptr_array_index (rows, i);
      if (is_ours (row))
        continue;
      g_autofree char *uri = row_uri (row);
      if (g_strcmp0 (uri, COMPUTER_URI) == 0)
        continue;
      if (target == NULL && GTK_IS_LIST_BOX (gtk_widget_get_parent (row)))
        target = gtk_widget_get_parent (row);
    }
  if (target == NULL)
    return; /* not populated yet; the rows' map will bring us back */

  ensure_css (hook->sidebar);

  watch_selection (hook, target);
  GtkRoot *window = gtk_widget_get_root (hook->sidebar);
  if (window != NULL && g_object_get_data (G_OBJECT (window), "sd-watched") == NULL &&
      g_object_class_find_property (G_OBJECT_GET_CLASS (window), "active-slot") != NULL)
    {
      g_object_set_data (G_OBJECT (window), "sd-watched", GINT_TO_POINTER (1));
      g_signal_connect_object (window, "notify::active-slot", G_CALLBACK (on_active_slot), hook->sidebar, 0);
    }
  watch_slot (hook);

  if (hook->os_row == NULL)
    hook->os_row = create_os_row (hook->sidebar);
  if (hook->os_row != NULL && gtk_widget_get_parent (hook->os_row) != target)
    {
      GtkWidget *old = gtk_widget_get_parent (hook->os_row);
      if (GTK_IS_LIST_BOX (old))
        gtk_list_box_remove (GTK_LIST_BOX (old), hook->os_row);
      gtk_list_box_insert (GTK_LIST_BOX (target), hook->os_row, -1);
      gtk_list_box_invalidate_headers (GTK_LIST_BOX (target));
      RowDeco *deco = g_object_get_data (G_OBJECT (hook->os_row), DECO_KEY);
      g_autoptr (GFile) root = g_file_new_for_path ("/");
      if (deco != NULL)
        start_query (hook->os_row, deco, root);
      sync_selection (hook);
      /* Nautilus clears the selection again in later steps of a rebuild: check once more after it settles. */
      g_clear_handle_id (&hook->sync_id, g_source_remove);
      hook->sync_id = g_timeout_add (REBUILD_SETTLE_MS, sync_idle, hook);
    }

  for (guint i = 0; i < rows->len; i++)
    {
      GtkWidget *row = g_ptr_array_index (rows, i);
      if (is_ours (row) || g_object_get_data (G_OBJECT (row), DECO_KEY) != NULL)
        continue;
      g_autoptr (GMount) mount = row_mount (row);
      if (mount == NULL)
        continue;
      g_autoptr (GFile) mount_root = g_mount_get_root (mount);
      if (is_network_row (hook, row, mount_root))
        continue; /* shown as our own row in the network list (reconcile_network) */
      RowDeco *deco = decorate (row, NULL);
      if (deco == NULL)
        {
          g_debug ("drive row without the expected label box; left untouched");
          continue;
        }
      g_autoptr (GFile) root = g_mount_get_root (mount);
      if (g_log_writer_default_would_drop (G_LOG_LEVEL_DEBUG, G_LOG_DOMAIN) == FALSE)
        {
          g_autofree char *label = NULL, *uri = g_file_get_uri (root);
          int section = -1, place = -1;
          g_object_get (row, "label", &label, "section-type", &section, "place-type", &place, NULL);
          g_debug ("drive row '%s' root=%s native=%d section=%d place=%d index=%d", label, uri,
                   g_file_is_native (root), section, place, gtk_list_box_row_get_index (GTK_LIST_BOX_ROW (row)));
        }
      start_query (row, deco, root);
    }

  reconcile_network (hook, target, rows);
  sync_selection (hook);
}

static gboolean
apply_idle (gpointer data)
{
  SidebarHook *hook = data; /* Borrowed: sidebar_hook_free removes this idle. */
  hook->apply_id = 0;
  if (sh != NULL && !sh->disabled)
    apply (hook);
  return G_SOURCE_REMOVE;
}

static void
schedule_apply (SidebarHook *hook)
{
  if (hook->apply_id == 0)
    hook->apply_id = g_idle_add_full (G_PRIORITY_HIGH_IDLE, apply_idle, hook, NULL);
}

static void
sidebar_hook_free (gpointer data)
{
  SidebarHook *hook = data;
  g_clear_handle_id (&hook->apply_id, g_source_remove);
  g_clear_handle_id (&hook->sync_id, g_source_remove);
  if (hook->os_row != NULL)
    {
      GtkWidget *list = gtk_widget_get_parent (hook->os_row);
      if (GTK_IS_LIST_BOX (list))
        gtk_list_box_remove (GTK_LIST_BOX (list), hook->os_row);
      g_clear_object (&hook->os_row);
    }
  for (GtkWidget **w = (GtkWidget *[]) { hook->net_sep, hook->net_list, NULL }; *w != NULL; w++)
    if (GTK_IS_BOX (gtk_widget_get_parent (*w)))
      gtk_box_remove (GTK_BOX (gtk_widget_get_parent (*w)), *w);
  g_clear_object (&hook->net_sep);
  g_clear_object (&hook->net_list);
  g_clear_pointer (&hook->net_rows, g_hash_table_unref);
  g_clear_pointer (&hook->remote_uris, g_hash_table_unref);
  g_weak_ref_clear (&hook->slot);
  if (sh != NULL && sh->sidebars != NULL)
    g_hash_table_remove (sh->sidebars, hook);
  g_free (hook);
}

static SidebarHook *
get_or_create_hook (GtkWidget *sidebar)
{
  SidebarHook *hook = g_object_get_data (G_OBJECT (sidebar), SIDEBAR_KEY);
  if (hook != NULL)
    return hook;
  hook = g_new0 (SidebarHook, 1);
  hook->sidebar = sidebar;
  g_weak_ref_init (&hook->slot, NULL);
  hook->net_rows = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  hook->remote_uris = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_object_set_data_full (G_OBJECT (sidebar), SIDEBAR_KEY, hook, sidebar_hook_free);
  g_hash_table_add (sh->sidebars, hook);
  sh->n_hooked_total++;
  g_clear_handle_id (&sh->detect_timer, g_source_remove);
  return hook;
}

/* Undo our changes on a live sidebar and drop its hook. */
static void
detach (SidebarHook *hook)
{
  GtkWidget *sidebar = hook->sidebar;
  GtkRoot *window = gtk_widget_get_root (sidebar);
  if (window != NULL)
    {
      g_signal_handlers_disconnect_by_func (window, G_CALLBACK (on_active_slot), sidebar);
      g_object_set_data (G_OBJECT (window), "sd-watched", NULL);
    }
  g_autoptr (GObject) slot = g_weak_ref_get (&hook->slot);
  if (slot != NULL)
    g_signal_handlers_disconnect_by_func (slot, G_CALLBACK (on_slot_location), sidebar);
  if (hook->os_row != NULL)
    unwatch_selection (sidebar, gtk_widget_get_parent (hook->os_row));
  g_autoptr (GPtrArray) rows = g_ptr_array_new ();
  collect_rows (sidebar, rows);
  for (guint i = 0; i < rows->len; i++)
    {
      GtkWidget *row = g_ptr_array_index (rows, i);
      if (g_object_get_data (G_OBJECT (row), HIDDEN_KEY) != NULL)
        {
          g_object_set_data (G_OBJECT (row), HIDDEN_KEY, NULL);
          gtk_widget_set_visible (row, TRUE);
        }
    }
  g_object_set_data (G_OBJECT (sidebar), SIDEBAR_KEY, NULL); /* frees @hook, removes our OS row */
}

static void
detach_all (void)
{
  g_autoptr (GPtrArray) all = g_hash_table_get_keys_as_ptr_array (sh->sidebars);
  for (guint i = 0; i < all->len; i++)
    detach (g_ptr_array_index (all, i));
}

/* ------------------------------------------------------------------------------------------------ */
/* Validation, graceful failure                                                                      */

static gboolean
disable_idle (gpointer data)
{
  (void) data;
  sh->disable_idle = 0;
  detach_all ();
  return G_SOURCE_REMOVE;
}

static void
disable (const char *reason)
{
  if (sh == NULL || sh->disabled)
    return;
  g_warning ("sidebar drives disabled: %s (Nautilus internals changed; please update the extension)", reason);
  sh->disabled = TRUE;
  g_clear_handle_id (&sh->detect_timer, g_source_remove);
  sh->disable_idle = g_idle_add (disable_idle, NULL);
}

static int
enum_value (GParamSpec *pspec,
            const char *name)
{
  if (pspec == NULL || !G_IS_PARAM_SPEC_ENUM (pspec))
    return -1;
  GEnumValue *v = g_enum_get_value_by_name (G_PARAM_SPEC_ENUM (pspec)->enum_class, name);
  return v != NULL ? v->value : -1;
}

/* Everything we rely on, checked once before touching any sidebar. */
static gboolean
validate (void)
{
  static const char *const row_props[] = { "sidebar", "label", "tooltip", "uri", "mount", "start-icon",
                                           "section-type", "place-type", "ejectable" };
  g_autoptr (GTypeClass) row_class = g_type_class_ref (sh->row_type);
  for (gsize i = 0; i < G_N_ELEMENTS (row_props); i++)
    if (g_object_class_find_property (G_OBJECT_CLASS (row_class), internal_name (row_props[i], "props")) == NULL)
      {
        g_autofree char *why = g_strdup_printf ("NautilusSidebarRow has no \"%s\" property", row_props[i]);
        disable (why);
        return FALSE;
      }
  if (!g_type_is_a (sh->row_type, GTK_TYPE_LIST_BOX_ROW))
    {
      disable ("NautilusSidebarRow is not a GtkListBoxRow");
      return FALSE;
    }
  sh->section_mounts = enum_value (g_object_class_find_property (G_OBJECT_CLASS (row_class), "section-type"),
                                   "NAUTILUS_SIDEBAR_SECTION_MOUNTS");
  sh->place_internal = enum_value (g_object_class_find_property (G_OBJECT_CLASS (row_class), "place-type"),
                                   "NAUTILUS_SIDEBAR_ROW_INTERNAL_MOUNT");
  if (sh->section_mounts < 0 || sh->place_internal < 0)
    {
      disable ("sidebar section/row type enums changed");
      return FALSE;
    }
  return TRUE;
}

static gboolean
on_detect_timeout (gpointer data)
{
  (void) data;
  sh->detect_timer = 0;
  if (sh->n_hooked_total == 0)
    disable ("a Nautilus window is shown but no NautilusSidebar was found");
  return G_SOURCE_REMOVE;
}

/* Emission hook for GtkWidget::map: sees every widget that becomes visible. */
static gboolean
on_widget_map (GSignalInvocationHint *ihint,
               guint                  n_params,
               const GValue          *params,
               gpointer               data)
{
  (void) ihint; (void) data;
  if (sh == NULL || sh->disabled || n_params < 1)
    {
      if (sh != NULL)
        sh->map_hook_id = 0; /* returning FALSE removes the hook */
      return FALSE;
    }
  GObject *obj = g_value_get_object (&params[0]); /* borrowed */

  if (sh->detect_timer == 0 && sh->n_hooked_total == 0 && GTK_IS_WINDOW (obj) &&
      g_str_equal (G_OBJECT_TYPE_NAME (obj), "NautilusWindow"))
    sh->detect_timer = g_timeout_add (DETECT_TIMEOUT_MS, on_detect_timeout, NULL);

  if (sh->sidebar_type == 0 || sh->row_type == 0)
    {
      sh->sidebar_type = g_type_from_name (internal_name ("NautilusSidebar", "types"));
      sh->row_type = g_type_from_name ("NautilusSidebarRow");
      if (sh->sidebar_type == 0 || sh->row_type == 0)
        return TRUE;
    }
  GtkWidget *sidebar = NULL;
  if (G_TYPE_CHECK_INSTANCE_TYPE (obj, sh->sidebar_type))
    sidebar = GTK_WIDGET (obj);
  else if (G_TYPE_CHECK_INSTANCE_TYPE (obj, sh->row_type))
    sidebar = sidebar_of (GTK_WIDGET (obj));
  if (sidebar == NULL)
    return TRUE;

  if (!sh->validated)
    {
      if (!validate ())
        {
          sh->map_hook_id = 0; /* returning FALSE removes the hook */
          return FALSE;
        }
      sh->validated = TRUE;
    }
  schedule_apply (get_or_create_hook (sidebar));
  return TRUE;
}

/* ------------------------------------------------------------------------------------------------ */
/* Debug driver (only in -Ddebug_driver=true builds; shares $FP_DEBUG_DIR/cmd + SIGUSR2 with folder     */
/* preview's driver, and ignores commands that aren't "sd-…").                                        */

#ifdef SD_DEBUG_DRIVER
static guint debug_signal_id;

/* Nautilus' real Computer row in @sidebar (not ours), or NULL. Borrowed. */
static GtkWidget *
find_real_computer_row (GtkWidget *sidebar)
{
  g_autoptr (GPtrArray) rows = g_ptr_array_new ();
  collect_rows (sidebar, rows);
  for (guint i = 0; i < rows->len; i++)
    {
      GtkWidget *row = g_ptr_array_index (rows, i);
      g_autofree char *uri = is_ours (row) ? NULL : row_uri (row);
      if (g_strcmp0 (uri, COMPUTER_URI) == 0)
        return row;
    }
  return NULL;
}


static void
debug_dump (void)
{
  GHashTableIter it;
  gpointer key;
  g_hash_table_iter_init (&it, sh->sidebars);
  while (g_hash_table_iter_next (&it, &key, NULL))
    {
      SidebarHook *hook = key;
      g_autoptr (GPtrArray) rows = g_ptr_array_new ();
      collect_rows (hook->sidebar, rows);
      GtkWidget *computer = find_real_computer_row (hook->sidebar);
      g_autoptr (GFile) location = NULL;
      g_object_get (hook->sidebar, "location", &location, NULL);
      g_autofree char *loc = location != NULL ? g_file_get_uri (location) : NULL;
      GtkWidget *clist = computer != NULL ? gtk_widget_get_parent (computer) : NULL;
      GtkRoot *root = gtk_widget_get_root (hook->sidebar);
      g_message ("sd dump: window-title='%s'", GTK_IS_WINDOW (root) ? gtk_window_get_title (GTK_WINDOW (root)) : "?");
      g_message ("sd dump: sidebar=%p location=%s rows=%u computer-visible=%d computer-list-visible=%d computer-selected=%d",
                 (void *) hook->sidebar, loc, rows->len, computer != NULL && gtk_widget_get_visible (computer),
                 clist != NULL && gtk_widget_get_visible (clist),
                 GTK_IS_LIST_BOX (clist) && gtk_list_box_get_selected_row (GTK_LIST_BOX (clist)) == GTK_LIST_BOX_ROW (computer));
      for (guint i = 0; i < rows->len; i++)
        {
          GtkWidget *row = g_ptr_array_index (rows, i);
          RowDeco *deco = g_object_get_data (G_OBJECT (row), DECO_KEY);
          if (deco == NULL && !is_ours (row))
            continue;
          g_autofree char *label = NULL;
          g_object_get (row, "label", &label, NULL);
          GtkWidget *list = gtk_widget_get_parent (row);
          gboolean selected = GTK_IS_LIST_BOX (list) &&
                              gtk_list_box_get_selected_row (GTK_LIST_BOX (list)) == GTK_LIST_BOX_ROW (row);
          g_message ("sd dump:   %s index=%d header=%d label='%s' bar=%d value=%.3f warn=%d full=%d caption='%s' selected=%d",
                     is_ours (row) ? "OS-ROW" : "drive ", gtk_list_box_row_get_index (GTK_LIST_BOX_ROW (row)),
                     gtk_list_box_row_get_header (GTK_LIST_BOX_ROW (row)) != NULL, label,
                     deco != NULL && gtk_widget_get_visible (deco->bar),
                     deco != NULL ? gtk_level_bar_get_value (GTK_LEVEL_BAR (deco->bar)) : -1.0,
                     deco != NULL && gtk_widget_has_css_class (deco->bar, "sd-warning"),
                     deco != NULL && gtk_widget_has_css_class (deco->bar, "sd-full"),
                     deco != NULL ? gtk_label_get_text (GTK_LABEL (deco->caption)) : "", selected);
        }
      guint hidden = 0;
      for (guint i = 0; i < rows->len; i++)
        hidden += g_object_get_data (G_OBJECT (g_ptr_array_index (rows, i)), HIDDEN_KEY) != NULL;
      g_message ("sd dump: network: sep-visible=%d list-visible=%d rows=%u hidden-nautilus-rows=%u",
                 hook->net_sep != NULL && gtk_widget_get_visible (hook->net_sep),
                 hook->net_list != NULL && gtk_widget_get_visible (hook->net_list),
                 hook->net_rows != NULL ? g_hash_table_size (hook->net_rows) : 0, hidden);
      if (hook->net_list != NULL)
        for (GtkWidget *c = gtk_widget_get_first_child (hook->net_list); c != NULL; c = gtk_widget_get_next_sibling (c))
          {
            if (!GTK_IS_LIST_BOX_ROW (c))
              continue;
            RowDeco *deco = g_object_get_data (G_OBJECT (c), DECO_KEY);
            g_message ("sd dump:   NET-ROW uri=%s title='%s' bar=%d caption='%s' selected=%d",
                       (const char *) g_object_get_data (G_OBJECT (c), NET_URI_KEY),
                       (const char *) g_object_get_data (G_OBJECT (c), "sd-net-title"),
                       deco != NULL && gtk_widget_get_visible (deco->bar),
                       deco != NULL ? gtk_label_get_text (GTK_LABEL (deco->caption)) : "",
                       gtk_list_box_get_selected_row (GTK_LIST_BOX (hook->net_list)) == GTK_LIST_BOX_ROW (c));
          }
    }
}

static gboolean
on_debug_signal (gpointer data)
{
  (void) data;
  const char *dir = g_getenv ("FP_DEBUG_DIR");
  g_autofree char *path = dir != NULL ? g_build_filename (dir, "cmd", NULL) : NULL;
  g_autofree char *cmd = NULL;
  if (sh == NULL || path == NULL || !g_file_get_contents (path, &cmd, NULL, NULL))
    return G_SOURCE_CONTINUE;
  g_strstrip (cmd);
  if (g_str_equal (cmd, "sd-dump"))
    debug_dump ();
  else if (g_str_equal (cmd, "sd-open-home"))
    {
      g_autofree char *home = g_filename_to_uri (g_get_home_dir (), NULL, NULL);
      GHashTableIter it;
      gpointer key;
      g_hash_table_iter_init (&it, sh->sidebars);
      while (g_hash_table_iter_next (&it, &key, NULL))
        {
          g_autoptr (GPtrArray) rows = g_ptr_array_new ();
          collect_rows (((SidebarHook *) key)->sidebar, rows);
          for (guint i = 0; i < rows->len; i++)
            {
              GtkWidget *row = g_ptr_array_index (rows, i);
              g_autofree char *uri = row_uri (row);
              GtkWidget *list = gtk_widget_get_parent (row);
              if (g_strcmp0 (uri, home) == 0 && GTK_IS_LIST_BOX (list))
                {
                  g_message ("sd: opening Home");
                  gtk_list_box_select_row (GTK_LIST_BOX (list), GTK_LIST_BOX_ROW (row));
                  g_signal_emit_by_name (list, "row-activated", row);
                  break;
                }
            }
        }
    }
  else if (g_str_equal (cmd, "sd-click-net"))
    {
      GHashTableIter it;
      gpointer key;
      g_hash_table_iter_init (&it, sh->sidebars);
      while (g_hash_table_iter_next (&it, &key, NULL))
        {
          SidebarHook *hook = key;
          GtkListBoxRow *row = hook->net_list != NULL ? gtk_list_box_get_row_at_index (GTK_LIST_BOX (hook->net_list), 0) : NULL;
          if (row != NULL)
            {
              g_message ("sd: activating first network row");
              g_signal_emit_by_name (hook->net_list, "row-activated", row);
            }
        }
    }
  else if (g_str_equal (cmd, "sd-click-os"))
    {
      g_autoptr (GPtrArray) all = g_hash_table_get_keys_as_ptr_array (sh->sidebars);
      for (guint i = 0; i < all->len; i++)
        {
          SidebarHook *hook = g_ptr_array_index (all, i);
          if (hook->os_row != NULL && gtk_widget_get_mapped (hook->os_row))
            {
              g_message ("sd: activating OS row");
              activate_os_row (hook->os_row);
            }
        }
    }
  return G_SOURCE_CONTINUE;
}
#endif

/* ------------------------------------------------------------------------------------------------ */

void
sd_sidebar_hook_init (void)
{
  g_return_if_fail (sh == NULL);
  sh = g_new0 (Hook, 1);
  sh->sidebars = g_hash_table_new (g_direct_hash, g_direct_equal);
  sh->css_displays = g_ptr_array_new ();
  sh->os_name = sd_os_name ();

  g_autoptr (GTypeClass) widget_class = g_type_class_ref (GTK_TYPE_WIDGET);
  sh->map_signal = g_signal_lookup ("map", GTK_TYPE_WIDGET);
  if (sh->map_signal == 0)
    {
      disable ("GtkWidget::map not found");
      return;
    }
  sh->map_hook_id = g_signal_add_emission_hook (sh->map_signal, 0, on_widget_map, NULL, NULL);
#ifdef SD_DEBUG_DRIVER
  debug_signal_id = g_unix_signal_add (SIGUSR2, on_debug_signal, NULL);
#endif
}

void
sd_sidebar_hook_shutdown (void)
{
  if (sh == NULL)
    return;
#ifdef SD_DEBUG_DRIVER
  g_clear_handle_id (&debug_signal_id, g_source_remove);
#endif
  if (sh->map_hook_id != 0)
    g_signal_remove_emission_hook (sh->map_signal, sh->map_hook_id);
  sh->map_hook_id = 0;
  g_clear_handle_id (&sh->detect_timer, g_source_remove);
  g_clear_handle_id (&sh->disable_idle, g_source_remove);
  detach_all ();
  if (sh->css != NULL)
    for (guint i = 0; i < sh->css_displays->len; i++)
      gtk_style_context_remove_provider_for_display (g_ptr_array_index (sh->css_displays, i),
                                                     GTK_STYLE_PROVIDER (sh->css));
  g_clear_object (&sh->css);
  g_clear_pointer (&sh->css_displays, g_ptr_array_unref);
  g_clear_pointer (&sh->sidebars, g_hash_table_unref);
  g_clear_pointer (&sh->os_name, g_free);
  g_clear_pointer (&sh, g_free);
}
