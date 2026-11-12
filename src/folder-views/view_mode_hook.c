/* Folder views: per-tab hook (PLAN.md §10, findings §10.7). See view_mode_hook.h.
 *
 * Nautilus internals used (all looked up by name, no private headers):
 *   - NautilusWindowSlot: one per tab; the child of an AdwTabPage. Properties "location" (GFile),
 *     "icon-name" (notified on every view switch), "search-global". Action "slot.files-view-mode" (u).
 *   - NautilusFilesView inside the slot: actions "view.zoom-in" / "view.zoom-out".
 *   - NautilusListBase inside the files view (NautilusGridView / NautilusListView): property "icon-size".
 * If any of these is missing, the feature logs one warning and switches itself off (PLAN.md §10.3.5). */
#include "view_mode_hook.h"

#include <adwaita.h>
#include <gtk/gtk.h>

#include "config.h"
#include "fv_state.h"
#include "nu_settings.h"

#ifdef FV_DEBUG_DRIVER
#include <glib-unix.h>
#include <signal.h>
#endif

#define SLOT_KEY "nu-folder-views-slot"

typedef struct
{
  GWeakRef      slot;          /* The NautilusWindowSlot (weak: the slot owns this struct via qdata) */
  gulong        location_id;   /* notify::location on the slot */
  gulong        icon_name_id;  /* notify::icon-name on the slot */
  gulong        loading_id;    /* notify::loading on the slot */
  GWeakRef      list_base;     /* Inner view whose icon size we watch (weak; Nautilus replaces it) */
  gulong        icon_size_id;  /* notify::icon-size on @list_base, or 0 */
  char         *uri;           /* Owned; URI of the location shown, NULL before the first one */
  FvLocationKind kind;         /* Of @uri */
  gboolean      known;         /* @value is final (not waiting for a metadata read) */
  NuFolderView  value;         /* The folder's view: saved, or our default */
  gboolean      applying;      /* Our own view/zoom change is running: its notifications aren't the user */
  guint         user_idle;     /* Idle that classifies a view switch as the user's, or 0 */
  GCancellable *read_cancel;   /* Owned; the pending metadata read, or NULL */
} SlotHook;

typedef struct
{
  GHashTable   *hooks;          /* SlotHook* (owned by their slot's qdata) → itself; a set */
  FvCache      *cache;          /* Owned */
  GCancellable *cancel;         /* Owned; cancels metadata writes on shutdown */
  guint         attach_hook_id; /* Emission hook on AdwTabView::page-attached */
  guint         attach_signal;
  guint         restore_id;     /* Timeout restoring Files' global keys, or 0 */
  guint         disable_idle;   /* Idle detaching everything after disable(), or 0 */
  guint         types_watch;    /* Timeout checking that Nautilus' view types appeared, or 0 */
  gboolean      disabled;
  GSettings    *n_pref;         /* Owned; org.gnome.nautilus.preferences, NULL if not installed */
  GSettings    *n_icon;         /* Owned; org.gnome.nautilus.icon-view, or NULL */
  GSettings    *n_list;         /* Owned; org.gnome.nautilus.list-view, or NULL */
  GType         slot_type, files_view_type, list_base_type, grid_type, list_type;
} Fv;

static Fv *fv; /* Created by init, freed by shutdown (main thread). */

static void slot_location_changed (SlotHook *hook, GtkWidget *slot);
static gboolean types_ready (void);

/* ------------------------------------------------------------------------------------------------ */
/* Helpers                                                                                          */

/* First descendant of @w (or @w itself) of type @t, at most @depth levels down. Borrowed. Stops at the
 * match, so it never walks into a view's cells. */
static GtkWidget *
find_descendant (GtkWidget *w,
                 GType      t,
                 int        depth)
{
  if (G_TYPE_CHECK_INSTANCE_TYPE (w, t))
    return w;
  if (depth <= 0)
    return NULL;
  for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL; c = gtk_widget_get_next_sibling (c))
    {
      GtkWidget *r = find_descendant (c, t, depth - 1);
      if (r != NULL)
        return r;
    }
  return NULL;
}

/* The slot's NautilusFilesView, or NULL. Borrowed. */
static GtkWidget *
files_view_of (GtkWidget *slot)
{
  return fv->list_type != 0 ? find_descendant (slot, fv->files_view_type, 8) : NULL;
}

/* The slot's current inner view (grid or list), or NULL (e.g. Files' Network view). Borrowed. */
static GtkWidget *
list_base_of (GtkWidget *slot)
{
  GtkWidget *files_view = files_view_of (slot);
  return files_view != NULL ? find_descendant (files_view, fv->list_base_type, 4) : NULL;
}

/* What the slot shows right now. FALSE if it isn't a grid or list view. */
static gboolean
shown_view (GtkWidget  *slot,
            NuViewType *view,
            int        *zoom)
{
  GtkWidget *lb = list_base_of (slot);
  if (lb == NULL)
    return FALSE;
  if (G_TYPE_CHECK_INSTANCE_TYPE (lb, fv->grid_type))
    *view = NU_VIEW_GRID;
  else if (G_TYPE_CHECK_INSTANCE_TYPE (lb, fv->list_type))
    *view = NU_VIEW_LIST;
  else
    return FALSE;
  guint size = 0;
  g_object_get (lb, "icon-size", &size, NULL);
  *zoom = fv_zoom_for_icon_size (*view, size);
  return *zoom >= 0;
}

static gboolean
folder_view_equal (const NuFolderView *a, const NuFolderView *b)
{
  return a->view == b->view && a->grid_zoom == b->grid_zoom && a->list_zoom == b->list_zoom;
}

static void detach_all (void);

static gboolean
disable_idle (gpointer data)
{
  (void) data;
  fv->disable_idle = 0;
  detach_all ();
  return G_SOURCE_REMOVE;
}

/* Turns the feature off for this session. Detaching happens in an idle, so a hook that triggered this from
 * inside its own handler is never freed under its feet. */
static void
disable (const char *reason)
{
  if (fv == NULL || fv->disabled)
    return;
  g_warning ("folder views disabled: %s (Nautilus internals changed; please update the extension)", reason);
  fv->disabled = TRUE;
  fv->disable_idle = g_idle_add (disable_idle, NULL);
}

/* ------------------------------------------------------------------------------------------------ */
/* Files' global keys (PLAN.md §10.1.1)                                                             */

/* Opens a GSettings for @id if that schema is installed. Caller owns the result; NULL otherwise. */
static GSettings *
settings_if_installed (const char *id)
{
  GSettingsSchemaSource *src = g_settings_schema_source_get_default (); /* borrowed */
  g_autoptr (GSettingsSchema) schema = src != NULL ? g_settings_schema_source_lookup (src, id, TRUE) : NULL;
  return schema != NULL ? g_settings_new_full (schema, NULL, NULL) : NULL;
}

static void
set_enum_if_different (GSettings  *s,
                       const char *key,
                       int         value)
{
  if (s != NULL && g_settings_get_enum (s, key) != value)
    g_settings_set_enum (s, key, value);
}

/* Puts Files' keys back to our default, so a tab or window opened next starts from it and Files without
 * this feature behaves as before. */
static void
restore_global_keys (void)
{
  const NuFolderView *d = &nu_settings_get ()->folder_views_default;
  set_enum_if_different (fv->n_pref, "default-folder-viewer", d->view);
  set_enum_if_different (fv->n_icon, "default-zoom-level", d->grid_zoom);
  set_enum_if_different (fv->n_list, "default-zoom-level", d->list_zoom);
}

static gboolean
restore_timeout (gpointer data)
{
  (void) data;
  fv->restore_id = 0;
  restore_global_keys ();
  return G_SOURCE_REMOVE;
}

/* Restores the keys after the last of a burst of changes (D1). */
static void
schedule_restore (void)
{
  g_clear_handle_id (&fv->restore_id, g_source_remove);
  fv->restore_id = g_timeout_add (FV_RESTORE_KEYS_DELAY_MS, restore_timeout, NULL);
}

/* Copies Files' current default into ours, the first time the feature ever runs. */
static void
capture_default (void)
{
  NuFolderView files = nu_settings_defaults ()->folder_views_default;
  if (fv->n_pref != NULL)
    files.view = (NuViewType) g_settings_get_enum (fv->n_pref, "default-folder-viewer");
  if (fv->n_icon != NULL)
    files.grid_zoom = (guint8) CLAMP (g_settings_get_enum (fv->n_icon, "default-zoom-level"), 0, 255);
  if (fv->n_list != NULL)
    files.list_zoom = (guint8) CLAMP (g_settings_get_enum (fv->n_list, "default-zoom-level"), 0, 255);
  if (nu_settings_capture_folder_view_default (&files))
    g_debug ("captured Files' default: view %d, grid zoom %u, list zoom %u", files.view, files.grid_zoom,
             files.list_zoom);
}

/* ------------------------------------------------------------------------------------------------ */
/* Applying a folder's view                                                                         */

static void on_icon_size (GObject *list_base, GParamSpec *pspec, gpointer data);

/* Watches the slot's current inner view for zoom changes (Nautilus replaces it on every view switch). */
static void
rebind_list_base (SlotHook  *hook,
                  GtkWidget *slot)
{
  GtkWidget *lb = list_base_of (slot);
  g_autoptr (GObject) old = g_weak_ref_get (&hook->list_base);
  if (old == (GObject *) lb)
    return;
  if (old != NULL && hook->icon_size_id != 0)
    g_signal_handler_disconnect (old, hook->icon_size_id);
  hook->icon_size_id = 0;
  g_weak_ref_set (&hook->list_base, lb);
  if (lb != NULL)
    hook->icon_size_id = g_signal_connect (lb, "notify::icon-size", G_CALLBACK (on_icon_size), hook);
}

/* Shows @v in @slot with Nautilus' own actions; our notifications are ignored while @applying is set. */
static void
apply (SlotHook           *hook,
       GtkWidget          *slot,
       const NuFolderView *v)
{
  if (fv->disabled)
    return;
  NuViewType view;
  int zoom;
  if (!shown_view (slot, &view, &zoom))
    return; /* Network view or not built yet: nothing to switch */

  gint64 start = g_get_monotonic_time ();
  NuViewType was = view;
  int was_zoom = zoom;
  hook->applying = TRUE;
  if (view != v->view)
    {
      if (!gtk_widget_activate_action (slot, "slot.files-view-mode", "u", (guint32) v->view))
        disable ("the slot.files-view-mode action is missing");
    }
  int target = fv_folder_view_zoom (v);
  GtkWidget *files_view = files_view_of (slot);
  for (int step = 0; step < FV_MAX_ZOOM_STEPS && !fv->disabled && files_view != NULL; step++)
    {
      if (!shown_view (slot, &view, &zoom) || view != v->view || zoom == target)
        break;
      if (!gtk_widget_activate_action (files_view, zoom < target ? "view.zoom-in" : "view.zoom-out", NULL))
        disable ("the view.zoom-in/zoom-out actions are missing");
    }
  hook->applying = FALSE;
  rebind_list_base (hook, slot);
  schedule_restore ();
  g_debug ("applied view %d zoom %d to %s (was view %d zoom %d) in %.1f ms", v->view, target, hook->uri, was, was_zoom,
           (g_get_monotonic_time () - start) / 1000.0);
}

/* ------------------------------------------------------------------------------------------------ */
/* Reading and saving metadata (PLAN.md §10.1.2)                                                     */

typedef struct
{
  GWeakRef      slot;   /* The slot the read was for */
  char         *uri;    /* Owned; the location read */
  GCancellable *cancel; /* Owned ref; identifies the read (the hook drops its ref when it gives up) */
} ReadCtx;

static void
read_ctx_free (ReadCtx *ctx)
{
  g_weak_ref_clear (&ctx->slot);
  g_free (ctx->uri);
  g_clear_object (&ctx->cancel);
  g_free (ctx);
}

static void
on_read_done (GObject      *source,
              GAsyncResult *res,
              gpointer      data)
{
  ReadCtx *ctx = data; /* owned here */
  g_autoptr (GError) error = NULL;
  g_autoptr (GFileInfo) info = g_file_query_info_finish (G_FILE (source), res, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) || fv == NULL)
    {
      read_ctx_free (ctx);
      return;
    }
  NuFolderView saved;
  const char *text = info != NULL ? g_file_info_get_attribute_string (info, FV_METADATA_ATTR) : NULL;
  gboolean has = fv_decode (text, nu_settings_get ()->folder_views_generation, &saved);
  fv_cache_store (fv->cache, ctx->uri, has ? &saved : NULL);

  g_autoptr (GtkWidget) slot = g_weak_ref_get (&ctx->slot);
  SlotHook *hook = slot != NULL ? g_object_get_data (G_OBJECT (slot), SLOT_KEY) : NULL;
  if (hook != NULL && hook->read_cancel == ctx->cancel && g_strcmp0 (hook->uri, ctx->uri) == 0)
    {
      g_clear_object (&hook->read_cancel);
      hook->value = has ? saved : nu_settings_get ()->folder_views_default;
      hook->known = TRUE;
      apply (hook, slot, &hook->value);
    }
  read_ctx_free (ctx);
}

static void
start_read (SlotHook  *hook,
            GtkWidget *slot)
{
  hook->read_cancel = g_cancellable_new ();
  ReadCtx *ctx = g_new0 (ReadCtx, 1); /* freed by on_read_done */
  g_weak_ref_init (&ctx->slot, slot);
  ctx->uri = g_strdup (hook->uri);
  ctx->cancel = g_object_ref (hook->read_cancel);
  g_autoptr (GFile) file = g_file_new_for_uri (hook->uri);
  g_file_query_info_async (file, FV_METADATA_ATTR, G_FILE_QUERY_INFO_NONE, G_PRIORITY_HIGH, ctx->cancel,
                           on_read_done, ctx);
}

static void
cancel_read (SlotHook *hook)
{
  if (hook->read_cancel != NULL)
    g_cancellable_cancel (hook->read_cancel);
  g_clear_object (&hook->read_cancel);
}

static void
on_write_done (GObject      *source,
               GAsyncResult *res,
               gpointer      data)
{
  (void) data;
  g_autoptr (GError) error = NULL;
  if (!g_file_set_attributes_finish (G_FILE (source), res, NULL, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_autofree char *uri = g_file_get_uri (G_FILE (source));
      g_debug ("could not save the view of %s: %s", uri, error->message);
    }
}

/* Saves @hook->value for its folder, unless it's already the saved value. */
static void
save (SlotHook *hook)
{
  schedule_restore ();
  if (hook->kind != FV_LOCATION_SAVE || hook->uri == NULL)
    return;
  NuFolderView cached;
  if (fv_cache_lookup (fv->cache, hook->uri, &cached) == FV_CACHE_SAVED && folder_view_equal (&cached, &hook->value))
    return;
  fv_cache_store (fv->cache, hook->uri, &hook->value);
  g_autofree char *text = fv_encode (&hook->value, nu_settings_get ()->folder_views_generation);
  g_autoptr (GFileInfo) info = g_file_info_new ();
  g_file_info_set_attribute_string (info, FV_METADATA_ATTR, text);
  g_autoptr (GFile) file = g_file_new_for_uri (hook->uri);
  g_file_set_attributes_async (file, info, G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, fv->cancel, on_write_done,
                               NULL);
  g_debug ("saved %s = %s", hook->uri, text);
}

/* ------------------------------------------------------------------------------------------------ */
/* User changes                                                                                     */

/* The user changed the view type or icon size: take what the slot shows now as the folder's view. */
static void
take_user_change (SlotHook  *hook,
                  GtkWidget *slot)
{
  NuViewType view;
  int zoom;
  if (!shown_view (slot, &view, &zoom))
    return;
  if (!hook->known)
    {
      /* Changed before the folder's saved view arrived (a few ms): the user wins. */
      cancel_read (hook);
      hook->value = nu_settings_get ()->folder_views_default;
      hook->known = TRUE;
    }
  hook->value.view = view;
  if (view == NU_VIEW_GRID)
    hook->value.grid_zoom = (guint8) zoom;
  else
    hook->value.list_zoom = (guint8) zoom;
  save (hook);
}

static gboolean
user_idle (gpointer data)
{
  SlotHook *hook = data; /* alive: hook_free removes this idle */
  hook->user_idle = 0;
  g_autoptr (GtkWidget) slot = g_weak_ref_get (&hook->slot);
  if (slot != NULL && !fv->disabled)
    {
      g_debug ("view type changed by the user in %s", hook->uri);
      take_user_change (hook, slot);
    }
  return G_SOURCE_REMOVE;
}

/* Every view switch notifies icon-name. Ours are skipped; one followed by a location change in the same
 * main-loop iteration is navigation (on_location removes the idle); otherwise it's the user (§10.7). */
static void
on_icon_name (GObject    *slot,
              GParamSpec *pspec,
              gpointer    data)
{
  (void) pspec;
  SlotHook *hook = data;
  if (hook->applying || fv->disabled)
    return;
  rebind_list_base (hook, GTK_WIDGET (slot));
  if (hook->user_idle == 0)
    hook->user_idle = g_idle_add (user_idle, hook);
}

static void
on_icon_size (GObject    *list_base,
              GParamSpec *pspec,
              gpointer    data)
{
  (void) list_base;
  (void) pspec;
  SlotHook *hook = data;
  if (hook->applying || fv->disabled || hook->user_idle != 0)
    return; /* ours, or part of a view switch that the idle will take */
  g_autoptr (GtkWidget) slot = g_weak_ref_get (&hook->slot);
  if (slot != NULL)
    {
      g_debug ("icon size changed by the user in %s", hook->uri);
      take_user_change (hook, slot);
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Navigation                                                                                       */

static void
slot_location_changed (SlotHook  *hook,
                       GtkWidget *slot)
{
  g_clear_handle_id (&hook->user_idle, g_source_remove); /* that switch was navigation */
  cancel_read (hook);
  g_clear_pointer (&hook->uri, g_free);
  hook->known = FALSE;

  g_autoptr (GFile) location = NULL;
  g_object_get (slot, "location", &location, NULL);
  if (location == NULL || !types_ready ())
    return;
  hook->uri = g_file_get_uri (location);
  hook->kind = fv_location_kind (hook->uri);
  if (g_str_has_prefix (hook->uri, "x-nautilus-search:"))
    {
      /* A search inside a folder keeps that folder's view; Files' global search uses our default. */
      gboolean global = FALSE;
      if (g_object_class_find_property (G_OBJECT_GET_CLASS (slot), "search-global") != NULL)
        g_object_get (slot, "search-global", &global, NULL);
      if (!global)
        hook->kind = FV_LOCATION_IGNORE;
    }
  rebind_list_base (hook, slot);

  switch (hook->kind)
    {
    case FV_LOCATION_IGNORE:
      return;
    case FV_LOCATION_DEFAULT_ONLY:
      hook->value = nu_settings_get ()->folder_views_default;
      hook->known = TRUE;
      apply (hook, slot, &hook->value);
      return;
    case FV_LOCATION_SAVE:
      break;
    }

  NuFolderView saved;
  switch (fv_cache_lookup (fv->cache, hook->uri, &saved))
    {
    case FV_CACHE_SAVED:
      hook->value = saved;
      hook->known = TRUE;
      apply (hook, slot, &hook->value);
      break;
    case FV_CACHE_NONE:
      hook->value = nu_settings_get ()->folder_views_default;
      hook->known = TRUE;
      apply (hook, slot, &hook->value);
      break;
    case FV_CACHE_MISS:
      start_read (hook, slot); /* applies when it arrives (0.3–18 ms, §10.7) */
      break;
    }
}

/* A tab whose first location arrived before Nautilus' view types existed (D3) — Nautilus doesn't notify the
 * same location again — is caught up when its view starts loading (the view exists by then). */
static void
on_loading (GObject    *slot,
            GParamSpec *pspec,
            gpointer    data)
{
  (void) pspec;
  SlotHook *hook = data;
  if (!fv->disabled && hook->uri == NULL)
    slot_location_changed (hook, GTK_WIDGET (slot));
}

static void
on_location (GObject    *slot,
             GParamSpec *pspec,
             gpointer    data)
{
  (void) pspec;
  if (!fv->disabled)
    slot_location_changed (data, GTK_WIDGET (slot));
}

/* ------------------------------------------------------------------------------------------------ */
/* Attaching to tabs                                                                                */

/* qdata destroy notify: the slot is being finalized, or we detached. */
static void
hook_free (gpointer data)
{
  SlotHook *hook = data;
  if (fv != NULL)
    g_hash_table_remove (fv->hooks, hook);
  g_clear_handle_id (&hook->user_idle, g_source_remove);
  cancel_read (hook);
  g_autoptr (GObject) lb = g_weak_ref_get (&hook->list_base);
  if (lb != NULL && hook->icon_size_id != 0)
    g_signal_handler_disconnect (lb, hook->icon_size_id);
  g_weak_ref_clear (&hook->list_base);
  g_weak_ref_clear (&hook->slot);
  g_free (hook->uri);
  g_free (hook);
}

static gboolean
lookup_types (void)
{
  fv->files_view_type = g_type_from_name ("NautilusFilesView");
  fv->list_base_type = g_type_from_name ("NautilusListBase");
  fv->grid_type = g_type_from_name ("NautilusGridView");
  fv->list_type = g_type_from_name ("NautilusListView");
  if (fv->files_view_type == 0 || fv->list_base_type == 0 || fv->grid_type == 0 || fv->list_type == 0)
    {
      fv->list_type = 0; /* "not ready" marker */
      return FALSE;
    }
  return TRUE;
}

static gboolean
types_watchdog (gpointer data)
{
  (void) data;
  fv->types_watch = 0;
  if (!lookup_types ())
    disable ("NautilusFilesView / NautilusListBase / NautilusGridView / NautilusListView not found");
  return G_SOURCE_REMOVE;
}

/* Looks up Nautilus' view types. They are registered when the first view is built; a tab can report a
 * location before that (D3), so a miss isn't an error yet: that tab's real location change follows. If the
 * types are still missing FV_TYPES_TIMEOUT_MS later, Nautilus changed → the feature switches itself off. */
static gboolean
types_ready (void)
{
  if (fv->list_type != 0 || lookup_types ())
    return TRUE;
  if (fv->types_watch == 0 && !fv->disabled)
    fv->types_watch = g_timeout_add (FV_TYPES_TIMEOUT_MS, types_watchdog, NULL);
  return FALSE;
}

static void
attach_slot (GtkWidget *slot)
{
  if (fv->disabled || g_object_get_data (G_OBJECT (slot), SLOT_KEY) != NULL)
    return;
  GObjectClass *klass = G_OBJECT_GET_CLASS (slot);
  GParamSpec *loc = g_object_class_find_property (klass, "location");
  if (loc == NULL || !g_type_is_a (loc->value_type, G_TYPE_FILE) ||
      g_object_class_find_property (klass, "icon-name") == NULL)
    {
      disable ("NautilusWindowSlot has no location / icon-name property");
      return;
    }
  SlotHook *hook = g_new0 (SlotHook, 1); /* owned by the slot's qdata, freed by hook_free */
  g_weak_ref_init (&hook->slot, slot);
  g_weak_ref_init (&hook->list_base, NULL);
  hook->kind = FV_LOCATION_IGNORE;
  g_object_set_data_full (G_OBJECT (slot), SLOT_KEY, hook, hook_free);
  g_hash_table_add (fv->hooks, hook);
  hook->location_id = g_signal_connect (slot, "notify::location", G_CALLBACK (on_location), hook);
  hook->icon_name_id = g_signal_connect (slot, "notify::icon-name", G_CALLBACK (on_icon_name), hook);
  if (g_object_class_find_property (klass, "loading") != NULL)
    hook->loading_id = g_signal_connect (slot, "notify::loading", G_CALLBACK (on_loading), hook);
  /* A slot that already shows a folder (feature switched on later): apply now. */
  slot_location_changed (hook, slot);
}

static void
detach_slot (SlotHook *hook)
{
  g_autoptr (GtkWidget) slot = g_weak_ref_get (&hook->slot);
  if (slot == NULL)
    return;
  g_clear_signal_handler (&hook->location_id, slot);
  g_clear_signal_handler (&hook->icon_name_id, slot);
  g_clear_signal_handler (&hook->loading_id, slot);
  g_object_set_data (G_OBJECT (slot), SLOT_KEY, NULL); /* frees @hook */
}

static void
detach_all (void)
{
  g_autoptr (GList) hooks = g_hash_table_get_keys (fv->hooks); /* list owned, hooks borrowed */
  for (GList *l = hooks; l != NULL; l = l->next)
    detach_slot (l->data);
}

/* Emission hook for AdwTabView::page-attached: every new tab of every window, before it loads its folder
 * (Nautilus adds the page first, §10.7). */
static gboolean
on_page_attached (GSignalInvocationHint *ihint,
                  guint                  n_params,
                  const GValue          *params,
                  gpointer               data)
{
  (void) ihint;
  (void) data;
  if (fv == NULL || fv->disabled)
    {
      if (fv != NULL)
        fv->attach_hook_id = 0; /* returning FALSE removes the hook */
      return FALSE;
    }
  if (n_params < 2 || !G_VALUE_HOLDS (&params[1], ADW_TYPE_TAB_PAGE))
    return TRUE;
  AdwTabPage *page = g_value_get_object (&params[1]); /* borrowed */
  GtkWidget *child = page != NULL ? adw_tab_page_get_child (page) : NULL;
  if (fv->slot_type == 0)
    fv->slot_type = g_type_from_name ("NautilusWindowSlot");
  if (child != NULL && fv->slot_type != 0 && G_TYPE_CHECK_INSTANCE_TYPE (child, fv->slot_type))
    attach_slot (child);
  return TRUE;
}

/* Attaches to the tabs of windows that already exist. */
static void
attach_existing (void)
{
  if (fv->slot_type == 0)
    fv->slot_type = g_type_from_name ("NautilusWindowSlot");
  GType window_type = g_type_from_name ("NautilusWindow");
  if (fv->slot_type == 0 || window_type == 0)
    return; /* no window yet: the emission hook catches the first one */
  GListModel *toplevels = gtk_window_get_toplevels (); /* borrowed */
  for (guint i = 0; i < g_list_model_get_n_items (toplevels); i++)
    {
      g_autoptr (GtkWidget) window = g_list_model_get_item (toplevels, i);
      if (!G_TYPE_CHECK_INSTANCE_TYPE (window, window_type))
        continue;
      GtkWidget *tab_view = find_descendant (window, ADW_TYPE_TAB_VIEW, 12);
      for (int p = 0; tab_view != NULL && p < adw_tab_view_get_n_pages (ADW_TAB_VIEW (tab_view)); p++)
        {
          GtkWidget *child = adw_tab_page_get_child (adw_tab_view_get_nth_page (ADW_TAB_VIEW (tab_view), p));
          if (G_TYPE_CHECK_INSTANCE_TYPE (child, fv->slot_type))
            attach_slot (child);
        }
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Debug driver (only in -Ddebug_driver=true builds, used by tools/smoke-nautilus.sh). Shares
 * $FP_DEBUG_DIR/cmd + SIGUSR2 with the other features and ignores commands that aren't "fv-…":
 *   fv-open <path>              open <path> in the selected tab of the first window
 *   fv-tab-open <n> <path>      open <path> in tab <n> (may be a background tab)
 *   fv-new-tab                  open a new tab (Files' win.new-tab)
 *   fv-view <1|2>               switch the selected tab to list (1) / grid (2) like the user would
 *   fv-zoom <in|out|standard>   zoom the selected tab like the user would
 *   fv-dump                     per tab: folder, what it shows, what it should show (feature off: tabs
 *                               still hooked, must be 0, and whether Files' keys are at our default)
 *   fv-keys                     Files' global keys and our default
 *   fv-reset                    Reset All, exactly as the settings dialog's button                        */

#ifdef FV_DEBUG_DRIVER
static guint debug_signal_id;

/* The first NautilusWindow's tab view, or NULL. Borrowed. */
static AdwTabView *
debug_tab_view (void)
{
  GType window_type = g_type_from_name ("NautilusWindow");
  GListModel *toplevels = gtk_window_get_toplevels (); /* borrowed */
  for (guint i = 0; window_type != 0 && i < g_list_model_get_n_items (toplevels); i++)
    {
      g_autoptr (GtkWidget) window = g_list_model_get_item (toplevels, i);
      if (G_TYPE_CHECK_INSTANCE_TYPE (window, window_type))
        {
          GtkWidget *tv = find_descendant (window, ADW_TYPE_TAB_VIEW, 12);
          return tv != NULL ? ADW_TAB_VIEW (tv) : NULL;
        }
    }
  return NULL;
}

static GtkWidget *
debug_slot (int index)
{
  AdwTabView *tv = debug_tab_view ();
  if (tv == NULL)
    return NULL;
  AdwTabPage *page = index < 0 ? adw_tab_view_get_selected_page (tv)
                     : index < adw_tab_view_get_n_pages (tv) ? adw_tab_view_get_nth_page (tv, index) : NULL;
  return page != NULL ? adw_tab_page_get_child (page) : NULL;
}

static void
debug_open (int index, const char *path)
{
  GtkWidget *slot = debug_slot (index);
  g_autoptr (GFile) file = g_file_new_for_commandline_arg (path);
  g_autofree char *uri = g_file_get_uri (file);
  gboolean ok = slot != NULL && gtk_widget_activate_action (slot, "slot.open-location", "s", uri);
  g_message ("fv: open %s in tab %d: %s", uri, index, ok ? "ok" : "FAILED");
}

static void
debug_dump (void)
{
  AdwTabView *tv = debug_tab_view ();
  int n = tv != NULL ? adw_tab_view_get_n_pages (tv) : 0;
  for (int i = 0; i < n; i++)
    {
      AdwTabPage *page = adw_tab_view_get_nth_page (tv, i);
      GtkWidget *slot = adw_tab_page_get_child (page);
      SlotHook *hook = g_object_get_data (G_OBJECT (slot), SLOT_KEY);
      NuViewType view = 0;
      int zoom = -1;
      gboolean shown = shown_view (slot, &view, &zoom);
      gboolean checked = hook != NULL && hook->known && hook->kind != FV_LOCATION_IGNORE && shown;
      gboolean ok = !checked || (view == hook->value.view && zoom == fv_folder_view_zoom (&hook->value));
      g_message ("fv dump: tab=%d selected=%d hooked=%d uri=%s kind=%d known=%d shown=%d/%d expect=%d/%d "
                 "checked=%d ok=%d",
                 i, page == adw_tab_view_get_selected_page (tv), hook != NULL, hook != NULL ? hook->uri : "-",
                 hook != NULL ? (int) hook->kind : -1, hook != NULL && hook->known, view, zoom,
                 hook != NULL ? (int) hook->value.view : 0, hook != NULL ? fv_folder_view_zoom (&hook->value) : -1,
                 checked, ok);
    }
  g_message ("fv dump: tabs=%d hooks=%u cache=%u disabled=%d", n, g_hash_table_size (fv->hooks),
             fv_cache_size (fv->cache), fv->disabled);
}

/* Feature off: count tabs that still carry our hook (must be 0) and show Files' keys (must be our
 * default again). */
static void
debug_dump_off (void)
{
  AdwTabView *tv = debug_tab_view ();
  int n = tv != NULL ? adw_tab_view_get_n_pages (tv) : 0, left = 0;
  for (int i = 0; i < n; i++)
    if (g_object_get_data (G_OBJECT (adw_tab_page_get_child (adw_tab_view_get_nth_page (tv, i))), SLOT_KEY) != NULL)
      left++;
  g_autoptr (GSettings) pref = settings_if_installed ("org.gnome.nautilus.preferences");
  g_autoptr (GSettings) icon = settings_if_installed ("org.gnome.nautilus.icon-view");
  g_autoptr (GSettings) list = settings_if_installed ("org.gnome.nautilus.list-view");
  const NuFolderView *d = &nu_settings_get ()->folder_views_default;
  gboolean keys_ok = (pref == NULL || g_settings_get_enum (pref, "default-folder-viewer") == (int) d->view) &&
                     (icon == NULL || g_settings_get_enum (icon, "default-zoom-level") == d->grid_zoom) &&
                     (list == NULL || g_settings_get_enum (list, "default-zoom-level") == d->list_zoom);
  g_message ("fv dump: off tabs=%d leftover=%d keys-at-default=%d", n, left, keys_ok);
}

/* The selected tab's NautilusFilesView, looked up by name (works with the feature off). Borrowed. */
static GtkWidget *
debug_files_view (GtkWidget *slot)
{
  GType t = g_type_from_name ("NautilusFilesView");
  return slot != NULL && t != 0 ? find_descendant (slot, t, 8) : NULL;
}

static gboolean
on_debug_signal (gpointer data)
{
  (void) data;
  const char *dir = g_getenv ("FP_DEBUG_DIR");
  g_autofree char *path = dir != NULL ? g_build_filename (dir, "cmd", NULL) : NULL;
  g_autofree char *cmd = NULL;
  if (path == NULL || !g_file_get_contents (path, &cmd, NULL, NULL))
    return G_SOURCE_CONTINUE;
  g_strstrip (cmd);
  if (!g_str_has_prefix (cmd, "fv-"))
    return G_SOURCE_CONTINUE;
  if (fv == NULL && (g_str_equal (cmd, "fv-dump") || g_str_equal (cmd, "fv-keys")))
    {
      debug_dump_off ();
      return G_SOURCE_CONTINUE;
    }
  if (g_str_equal (cmd, "fv-reset"))
    g_message ("fv: reset %s", nu_settings_reset_folder_views () ? "ok" : "FAILED");
  else if (g_str_has_prefix (cmd, "fv-open "))
    debug_open (-1, cmd + 8);
  else if (g_str_has_prefix (cmd, "fv-tab-open "))
    {
      g_auto (GStrv) parts = g_strsplit (cmd, " ", 3);
      if (g_strv_length (parts) == 3)
        debug_open ((int) g_ascii_strtoll (parts[1], NULL, 10), parts[2]);
    }
  else if (g_str_equal (cmd, "fv-new-tab"))
    {
      GtkWidget *slot = debug_slot (-1);
      GtkRoot *root = slot != NULL ? gtk_widget_get_root (slot) : NULL;
      g_message ("fv: new tab %s", root != NULL && gtk_widget_activate_action (GTK_WIDGET (root), "win.new-tab", NULL)
                                     ? "ok" : "FAILED");
    }
  else if (g_str_has_prefix (cmd, "fv-view "))
    {
      GtkWidget *slot = debug_slot (-1);
      guint32 v = (guint32) g_ascii_strtoull (cmd + 8, NULL, 10);
      g_message ("fv: user view %u %s", v,
                 slot != NULL && gtk_widget_activate_action (slot, "slot.files-view-mode", "u", v) ? "ok" : "FAILED");
    }
  else if (g_str_has_prefix (cmd, "fv-zoom "))
    {
      GtkWidget *slot = debug_slot (-1);
      GtkWidget *files_view = debug_files_view (slot);
      g_autofree char *action = g_strdup_printf ("view.zoom-%s", cmd + 8);
      g_message ("fv: user %s %s", action,
                 files_view != NULL && gtk_widget_activate_action (files_view, action, NULL) ? "ok" : "FAILED");
    }
  else if (g_str_equal (cmd, "fv-dump"))
    debug_dump (); /* fv != NULL here */
  else if (g_str_equal (cmd, "fv-keys"))
    {
      const NuFolderView *d = &nu_settings_get ()->folder_views_default;
      g_message ("fv keys: files view=%d grid=%d list=%d; ours view=%d grid=%u list=%u captured=%d",
                 fv->n_pref != NULL ? g_settings_get_enum (fv->n_pref, "default-folder-viewer") : -1,
                 fv->n_icon != NULL ? g_settings_get_enum (fv->n_icon, "default-zoom-level") : -1,
                 fv->n_list != NULL ? g_settings_get_enum (fv->n_list, "default-zoom-level") : -1, d->view,
                 d->grid_zoom, d->list_zoom, nu_settings_get ()->folder_views_defaults_captured);
    }
  return G_SOURCE_CONTINUE;
}
#endif

void
fv_view_mode_hook_debug_init (void)
{
#ifdef FV_DEBUG_DRIVER
  if (debug_signal_id == 0)
    debug_signal_id = g_unix_signal_add (SIGUSR2, on_debug_signal, NULL);
#endif
}

void
fv_view_mode_hook_debug_shutdown (void)
{
#ifdef FV_DEBUG_DRIVER
  g_clear_handle_id (&debug_signal_id, g_source_remove);
#endif
}

/* ------------------------------------------------------------------------------------------------ */
/* Public API                                                                                       */

void
fv_view_mode_hook_init (void)
{
  g_return_if_fail (fv == NULL);
  fv = g_new0 (Fv, 1);
  fv->hooks = g_hash_table_new (g_direct_hash, g_direct_equal);
  fv->cache = fv_cache_new (FV_CACHE_MAX);
  fv->cancel = g_cancellable_new ();
  fv->n_pref = settings_if_installed ("org.gnome.nautilus.preferences");
  fv->n_icon = settings_if_installed ("org.gnome.nautilus.icon-view");
  fv->n_list = settings_if_installed ("org.gnome.nautilus.list-view");
  capture_default ();

  g_autoptr (GTypeClass) tab_view_class = g_type_class_ref (ADW_TYPE_TAB_VIEW); /* make sure the signal exists */
  fv->attach_signal = g_signal_lookup ("page-attached", ADW_TYPE_TAB_VIEW);
  if (fv->attach_signal == 0)
    {
      disable ("AdwTabView::page-attached not found");
      return;
    }
  fv->attach_hook_id = g_signal_add_emission_hook (fv->attach_signal, 0, on_page_attached, NULL, NULL);
  attach_existing ();
}

void
fv_view_mode_hook_refresh (gboolean forget_saved)
{
  if (fv == NULL || fv->disabled)
    return;
  if (forget_saved)
    fv_cache_clear (fv->cache);
  g_autoptr (GList) hooks = g_hash_table_get_keys (fv->hooks); /* list owned, hooks borrowed */
  for (GList *l = hooks; l != NULL; l = l->next)
    {
      SlotHook *hook = l->data;
      g_autoptr (GtkWidget) slot = g_weak_ref_get (&hook->slot);
      if (slot != NULL)
        slot_location_changed (hook, slot); /* never frees a hook */
    }
}

void
fv_view_mode_hook_shutdown (void)
{
  if (fv == NULL)
    return;
  if (fv->attach_hook_id != 0)
    g_signal_remove_emission_hook (fv->attach_signal, fv->attach_hook_id);
  fv->attach_hook_id = 0;
  g_clear_handle_id (&fv->disable_idle, g_source_remove);
  g_clear_handle_id (&fv->types_watch, g_source_remove);
  detach_all ();
  g_cancellable_cancel (fv->cancel);
  g_clear_handle_id (&fv->restore_id, g_source_remove);
  restore_global_keys ();
  g_clear_object (&fv->n_pref);
  g_clear_object (&fv->n_icon);
  g_clear_object (&fv->n_list);
  g_clear_object (&fv->cancel);
  g_clear_pointer (&fv->cache, fv_cache_free);
  g_clear_pointer (&fv->hooks, g_hash_table_unref);
  g_clear_pointer (&fv, g_free);
}
