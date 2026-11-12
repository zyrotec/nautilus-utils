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
#include <stdio.h>
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
  GWeakRef      cover;         /* Our frozen picture over the view during a switch (§10.9), or empty */
  AdwAnimation *fade;          /* Owned; the cover's fade-out, or NULL */
  guint         cover_cap;     /* Timeout revealing the folder if loading takes too long, or 0 */
  gint64        cover_since;   /* Monotonic time the cover went up (debug log) */
  gboolean      cap_passed;    /* @cover_cap fired while the switch was still pending */
  gboolean      switch_pending;/* A switch to @target waits for the click's frame to be drawn (§10.10) */
  NuFolderView  target;        /* What the pending switch shows */
  GWeakRef      paint_clock;   /* Frame clock @paint_id is connected to */
  gulong        paint_id;      /* after-paint handler waiting for that frame, or 0 */
  guint         switch_idle;   /* Idle running the pending switch, or 0 */
  guint         switch_fallback; /* Timeout running it if no frame comes, or 0 */
  GCancellable *prefetch_cancel; /* Owned; the read-ahead of this folder's subfolders, or NULL */
  gboolean      prefetched;    /* Read-ahead started for the current location */
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

/* ------------------------------------------------------------------------------------------------ */
/* Smooth transitions (PLAN.md §10.9)                                                               */
/*                                                                                                  */
/* At a folder change the old folder's files are still on screen and the new ones arrive 0.2–1 s later   */
/* (§10.7). Switching the view type or zoom right away re-lays out the old files and blocks the main loop */
/* for 30–90 ms. Instead, a static picture of the old view goes on top first (an extra child of the       */
/* files view's GtkOverlay, below Nautilus' floating loading bar), the switch happens underneath it, and   */
/* it fades out when the slot stops loading (or after FV_COVER_MAX_MS), then is removed.                    */

/* The files view's GtkOverlay (its only child), or NULL. Borrowed. */
static GtkWidget *
view_overlay_of (GtkWidget *slot)
{
  GtkWidget *files_view = files_view_of (slot);
  GtkWidget *child = files_view != NULL && ADW_IS_BIN (files_view) ? adw_bin_get_child (ADW_BIN (files_view)) : NULL;
  return child != NULL && GTK_IS_OVERLAY (child) ? child : NULL;
}

/* Takes the cover out of the overlay and drops the animation/timer. */
static void
cover_remove (SlotHook *hook)
{
  g_clear_handle_id (&hook->cover_cap, g_source_remove);
  if (hook->fade != NULL)
    adw_animation_reset (hook->fade);
  g_clear_object (&hook->fade);
  g_autoptr (GtkWidget) cover = g_weak_ref_get (&hook->cover);
  GtkWidget *overlay = cover != NULL ? gtk_widget_get_parent (cover) : NULL;
  if (overlay != NULL && GTK_IS_OVERLAY (overlay))
    gtk_overlay_remove_overlay (GTK_OVERLAY (overlay), cover);
  g_weak_ref_set (&hook->cover, NULL);
}

static void
on_fade_done (AdwAnimation *animation,
              gpointer      data)
{
  (void) animation;
  SlotHook *hook = data; /* alive: hook_free resets and drops the animation first */
  g_debug ("transition: done after %.0f ms in %s", (g_get_monotonic_time () - hook->cover_since) / 1000.0,
           hook->uri);
  cover_remove (hook);
}

/* Starts the fade-out (once). */
static void
cover_fade (SlotHook *hook)
{
  g_clear_handle_id (&hook->cover_cap, g_source_remove);
  g_autoptr (GtkWidget) cover = g_weak_ref_get (&hook->cover);
  if (cover == NULL || hook->fade != NULL)
    return;
  g_debug ("transition: fading after %.0f ms in %s", (g_get_monotonic_time () - hook->cover_since) / 1000.0,
           hook->uri);
  AdwAnimationTarget *target = adw_property_animation_target_new (G_OBJECT (cover), "opacity"); /* consumed */
  hook->fade = adw_timed_animation_new (cover, 1.0, 0.0, FV_FADE_MS, target);
  adw_timed_animation_set_easing (ADW_TIMED_ANIMATION (hook->fade), ADW_EASE_OUT_CUBIC);
  g_signal_connect (hook->fade, "done", G_CALLBACK (on_fade_done), hook);
  adw_animation_play (hook->fade); /* instant when GTK animations are off */
}

/* Shows the new folder: removes the cover at once (FV_FADE_MS 0) or fades it out. */
static void
cover_reveal (SlotHook *hook)
{
  g_clear_handle_id (&hook->cover_cap, g_source_remove);
  if (FV_FADE_MS > 0)
    {
      cover_fade (hook);
      return;
    }
  g_autoptr (GtkWidget) cover = g_weak_ref_get (&hook->cover);
  if (cover == NULL)
    return;
  g_debug ("transition: done after %.0f ms in %s", (g_get_monotonic_time () - hook->cover_since) / 1000.0,
           hook->uri);
  cover_remove (hook);
}

static gboolean
cover_cap_timeout (gpointer data)
{
  SlotHook *hook = data; /* alive: hook_free removes this timeout */
  hook->cover_cap = 0;
  if (hook->switch_pending)
    hook->cap_passed = TRUE; /* reveal right after the switch */
  else
    cover_reveal (hook);
  return G_SOURCE_REMOVE;
}

/* Puts a frozen picture of the current view over it (or keeps the one already there, at full opacity). */
static void
cover_begin (SlotHook  *hook,
             GtkWidget *slot)
{
  g_autoptr (GtkWidget) existing = g_weak_ref_get (&hook->cover);
  if (existing != NULL)
    {
      if (hook->fade != NULL)
        adw_animation_reset (hook->fade); /* back to opacity 1 */
      g_clear_object (&hook->fade);
      gtk_widget_set_opacity (existing, 1.0);
    }
  else
    {
      GtkWidget *overlay = view_overlay_of (slot);
      GtkWidget *view = overlay != NULL ? gtk_overlay_get_child (GTK_OVERLAY (overlay)) : NULL;
      if (view == NULL || !gtk_widget_get_mapped (view))
        return; /* background tab or nothing shown: nothing to smooth */
      g_autoptr (GdkPaintable) live = gtk_widget_paintable_new (view);
      g_autoptr (GdkPaintable) frozen = gdk_paintable_get_current_image (live); /* static copy */
      GtkWidget *picture = gtk_picture_new_for_paintable (frozen);
      gtk_picture_set_content_fit (GTK_PICTURE (picture), GTK_CONTENT_FIT_FILL);
      gtk_picture_set_can_shrink (GTK_PICTURE (picture), TRUE);
      gtk_widget_set_hexpand (picture, TRUE);
      gtk_widget_set_vexpand (picture, TRUE);
      /* The "view" style paints the view background behind the picture, so nothing shows through. */
      GtkWidget *cover = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      gtk_widget_add_css_class (cover, "view");
      gtk_box_append (GTK_BOX (cover), picture);
      gtk_widget_set_can_target (cover, FALSE);
      gtk_widget_set_can_focus (cover, FALSE);
      gtk_accessible_update_state (GTK_ACCESSIBLE (cover), GTK_ACCESSIBLE_STATE_HIDDEN, TRUE, -1);
      gtk_overlay_add_overlay (GTK_OVERLAY (overlay), cover); /* the overlay owns it */
      gtk_widget_insert_after (cover, overlay, view); /* below Nautilus' floating loading bar */
      g_weak_ref_set (&hook->cover, cover);
    }
  hook->cover_since = g_get_monotonic_time ();
  hook->cap_passed = FALSE;
  g_clear_handle_id (&hook->cover_cap, g_source_remove);
  hook->cover_cap = g_timeout_add (FV_COVER_MAX_MS, cover_cap_timeout, hook);
}

static gboolean
slot_is_loading (GtkWidget *slot)
{
  gboolean loading = FALSE;
  if (g_object_class_find_property (G_OBJECT_GET_CLASS (slot), "loading") != NULL)
    g_object_get (slot, "loading", &loading, NULL);
  return loading;
}

/* Switches @slot to @hook->target with Nautilus' own actions (our notifications are ignored while @applying
 * is set), then reveals the folder if its files are already in. */
static void
do_switch (SlotHook  *hook,
           GtkWidget *slot)
{
  const NuFolderView *v = &hook->target;
  NuViewType view;
  int zoom;
  if (fv->disabled || !shown_view (slot, &view, &zoom))
    {
      cover_reveal (hook);
      return;
    }
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
  g_debug ("applied view %d zoom %d to %s (was view %d zoom %d) in %.1f ms, %.0f ms after the click", v->view,
           target, hook->uri, was, was_zoom, (g_get_monotonic_time () - start) / 1000.0,
           (start - hook->cover_since) / 1000.0);
  if (!slot_is_loading (slot) || hook->cap_passed)
    cover_reveal (hook); /* files already in (or loading too long): show the folder now */
}

static void
cancel_pending_switch (SlotHook *hook)
{
  hook->switch_pending = FALSE;
  g_autoptr (GdkFrameClock) clock = g_weak_ref_get (&hook->paint_clock);
  if (clock != NULL && hook->paint_id != 0)
    g_signal_handler_disconnect (clock, hook->paint_id);
  hook->paint_id = 0;
  g_weak_ref_set (&hook->paint_clock, NULL);
  g_clear_handle_id (&hook->switch_idle, g_source_remove);
  g_clear_handle_id (&hook->switch_fallback, g_source_remove);
}

static gboolean
switch_idle (gpointer data)
{
  SlotHook *hook = data; /* alive: hook_free cancels this idle */
  hook->switch_idle = 0;
  gboolean pending = hook->switch_pending;
  cancel_pending_switch (hook);
  g_autoptr (GtkWidget) slot = g_weak_ref_get (&hook->slot);
  if (pending && slot != NULL)
    do_switch (hook, slot);
  return G_SOURCE_REMOVE;
}

/* The frame showing the click (new path, loading state, our still picture) has been drawn: switch next. */
static void
on_after_paint (GdkFrameClock *clock,
                gpointer       data)
{
  SlotHook *hook = data; /* alive: hook_free disconnects this handler */
  g_signal_handler_disconnect (clock, hook->paint_id);
  hook->paint_id = 0;
  g_weak_ref_set (&hook->paint_clock, NULL);
  g_clear_handle_id (&hook->switch_fallback, g_source_remove);
  if (hook->switch_idle == 0)
    hook->switch_idle = g_idle_add_full (G_PRIORITY_DEFAULT, switch_idle, hook, NULL);
}

static gboolean
switch_fallback (gpointer data)
{
  SlotHook *hook = data; /* alive: hook_free removes this timeout */
  hook->switch_fallback = 0;
  return switch_idle (hook);
}

/* Shows @v in @slot. Visible tabs: the old view is frozen under a still picture now, and the switch runs right
 * after that frame is drawn, so the click gets its feedback at once and the switch's cost (Files' own view
 * rebuild) happens behind an unchanged picture (§10.10). Background tabs switch right away. */
static void
apply (SlotHook           *hook,
       GtkWidget          *slot,
       const NuFolderView *v)
{
  cancel_pending_switch (hook);
  if (fv->disabled)
    return;
  NuViewType view;
  int zoom;
  if (!shown_view (slot, &view, &zoom))
    return; /* Network view or not built yet: nothing to switch */
  if (view == v->view && zoom == fv_folder_view_zoom (v))
    return; /* already right: nothing changes on screen */
  hook->target = *v;
  GdkFrameClock *clock = gtk_widget_get_mapped (slot) ? gtk_widget_get_frame_clock (slot) : NULL; /* borrowed */
  if (clock == NULL)
    {
      do_switch (hook, slot);
      return;
    }
  cover_begin (hook, slot);
  hook->switch_pending = TRUE;
  g_weak_ref_set (&hook->paint_clock, clock);
  hook->paint_id = g_signal_connect (clock, "after-paint", G_CALLBACK (on_after_paint), hook);
  hook->switch_fallback = g_timeout_add (FV_SWITCH_FALLBACK_MS, switch_fallback, hook);
  gtk_widget_queue_draw (slot);
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
/* Read-ahead (PLAN.md §10.10)                                                                       */
/*                                                                                                  */
/* Once a local folder has loaded, the saved views of its subfolders, siblings and parent are read in the */
/* background (GIO's worker threads, low priority) into the cache, so that opening one of them knows its  */
/* view at the click. Only entries the cache doesn't know yet are stored, so a value the user just saved  */
/* is never replaced by an older one from disk.                                                     */

typedef struct
{
  GFileEnumerator *enumerator; /* Owned ref, or NULL before it is open */
  GCancellable    *cancel;     /* Owned ref; cancelled when the tab moves on */
  guint            seen;       /* Entries enumerated so far (capped at FV_PREFETCH_MAX) */
} PrefetchCtx;

static void
prefetch_ctx_free (PrefetchCtx *ctx)
{
  g_clear_object (&ctx->enumerator);
  g_clear_object (&ctx->cancel);
  g_free (ctx);
}

/* Stores what @info says about @file's saved view, unless the cache already knows that folder. */
static void
prefetch_store (GFile     *file,
                GFileInfo *info)
{
  g_autofree char *uri = g_file_get_uri (file);
  NuFolderView saved;
  if (fv_cache_lookup (fv->cache, uri, &saved) != FV_CACHE_MISS)
    return;
  const char *text = g_file_info_get_attribute_string (info, FV_METADATA_ATTR);
  gboolean has = fv_decode (text, nu_settings_get ()->folder_views_generation, &saved);
  fv_cache_store (fv->cache, uri, has ? &saved : NULL);
}

static void on_prefetch_batch (GObject *source, GAsyncResult *res, gpointer data);

static void
on_prefetch_closed (GObject *source, GAsyncResult *res, gpointer data)
{
  g_file_enumerator_close_finish (G_FILE_ENUMERATOR (source), res, NULL);
  prefetch_ctx_free (data);
}

static void
prefetch_finish (PrefetchCtx *ctx)
{
  /* Closing doesn't need the cancellable: an enumerator must be closed even after a cancel. */
  g_file_enumerator_close_async (ctx->enumerator, G_PRIORITY_LOW, NULL, on_prefetch_closed, ctx);
}

static void
on_prefetch_batch (GObject      *source,
                   GAsyncResult *res,
                   gpointer      data)
{
  PrefetchCtx *ctx = data; /* owned by this chain of callbacks */
  g_autoptr (GError) error = NULL;
  GList *infos = g_file_enumerator_next_files_finish (G_FILE_ENUMERATOR (source), res, &error); /* owned */
  if (infos == NULL || fv == NULL || g_cancellable_is_cancelled (ctx->cancel))
    {
      g_list_free_full (infos, g_object_unref);
      prefetch_finish (ctx);
      return;
    }
  for (GList *l = infos; l != NULL; l = l->next)
    {
      GFileInfo *info = l->data;
      ctx->seen++;
      if (g_file_info_get_file_type (info) != G_FILE_TYPE_DIRECTORY)
        continue;
      g_autoptr (GFile) child = g_file_enumerator_get_child (ctx->enumerator, info);
      prefetch_store (child, info);
    }
  g_list_free_full (infos, g_object_unref);
  if (ctx->seen >= FV_PREFETCH_MAX)
    {
      prefetch_finish (ctx);
      return;
    }
  g_file_enumerator_next_files_async (ctx->enumerator, FV_PREFETCH_BATCH, G_PRIORITY_LOW, ctx->cancel,
                                      on_prefetch_batch, ctx);
}

static void
on_prefetch_open (GObject      *source,
                  GAsyncResult *res,
                  gpointer      data)
{
  PrefetchCtx *ctx = data; /* owned by this chain of callbacks */
  ctx->enumerator = g_file_enumerate_children_finish (G_FILE (source), res, NULL);
  if (ctx->enumerator == NULL || fv == NULL || g_cancellable_is_cancelled (ctx->cancel))
    {
      if (ctx->enumerator != NULL)
        prefetch_finish (ctx);
      else
        prefetch_ctx_free (ctx);
      return;
    }
  g_file_enumerator_next_files_async (ctx->enumerator, FV_PREFETCH_BATCH, G_PRIORITY_LOW, ctx->cancel,
                                      on_prefetch_batch, ctx);
}

static void
on_prefetch_parent (GObject      *source,
                    GAsyncResult *res,
                    gpointer      data)
{
  (void) data;
  g_autoptr (GFileInfo) info = g_file_query_info_finish (G_FILE (source), res, NULL);
  if (info != NULL && fv != NULL)
    prefetch_store (G_FILE (source), info);
}

static void
cancel_prefetch (SlotHook *hook)
{
  if (hook->prefetch_cancel != NULL)
    g_cancellable_cancel (hook->prefetch_cancel);
  g_clear_object (&hook->prefetch_cancel);
}

/* Reads the saved views of @dir's subfolders into the cache, in the background. */
static void
prefetch_children (GFile        *dir,
                   GCancellable *cancel)
{
  PrefetchCtx *ctx = g_new0 (PrefetchCtx, 1); /* freed at the end of its callback chain */
  ctx->cancel = g_object_ref (cancel);
  g_file_enumerate_children_async (dir, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE "," FV_METADATA_ATTR,
                                   G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_LOW, ctx->cancel,
                                   on_prefetch_open, ctx);
}

/* Starts the read-ahead for the tab's current folder: its subfolders, its siblings and its parent (once per
 * location; local folders only, so a network share is never enumerated a second time). */
static void
start_prefetch (SlotHook *hook)
{
  if (hook->prefetched || hook->kind != FV_LOCATION_SAVE || hook->uri == NULL)
    return;
  hook->prefetched = TRUE;
  g_autoptr (GFile) dir = g_file_new_for_uri (hook->uri);
  if (!g_file_is_native (dir))
    return;
  cancel_prefetch (hook);
  hook->prefetch_cancel = g_cancellable_new ();
  prefetch_children (dir, hook->prefetch_cancel);

  g_autoptr (GFile) parent = g_file_get_parent (dir);
  if (parent != NULL)
    {
      prefetch_children (parent, hook->prefetch_cancel); /* siblings */
      g_autofree char *parent_uri = g_file_get_uri (parent);
      NuFolderView unused;
      if (fv_cache_lookup (fv->cache, parent_uri, &unused) == FV_CACHE_MISS)
        g_file_query_info_async (parent, FV_METADATA_ATTR, G_FILE_QUERY_INFO_NONE, G_PRIORITY_LOW,
                                 hook->prefetch_cancel, on_prefetch_parent, NULL);
    }
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
  cancel_prefetch (hook);
  hook->prefetched = FALSE;
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
      g_debug ("view of %s not known yet: reading it", hook->uri);
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
  if (!slot_is_loading (GTK_WIDGET (slot)))
    {
      if (!hook->switch_pending)
        cover_reveal (hook); /* the new folder's files are in: show it */
      start_prefetch (hook);
    }
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
  cancel_prefetch (hook);
  cancel_pending_switch (hook);
  g_weak_ref_clear (&hook->paint_clock);
  cover_remove (hook);
  g_weak_ref_clear (&hook->cover);
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
  g_weak_ref_init (&hook->cover, NULL);
  g_weak_ref_init (&hook->paint_clock, NULL);
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
 *   fv-reset                    Reset All, exactly as the settings dialog's button
 *   fv-expect <view> <zoom>     check what the selected tab shows (view 1 = list, 2 = grid)               */

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
      gboolean checked = hook != NULL && hook->known && hook->kind != FV_LOCATION_IGNORE && shown &&
                         !hook->switch_pending;
      gboolean ok = !checked || (view == hook->value.view && zoom == fv_folder_view_zoom (&hook->value));
      g_autoptr (GtkWidget) cover = hook != NULL ? g_weak_ref_get (&hook->cover) : NULL;
      g_message ("fv dump: tab=%d selected=%d hooked=%d uri=%s kind=%d known=%d shown=%d/%d expect=%d/%d "
                 "cover=%d pending=%d checked=%d ok=%d",
                 i, page == adw_tab_view_get_selected_page (tv), hook != NULL, hook != NULL ? hook->uri : "-",
                 hook != NULL ? (int) hook->kind : -1, hook != NULL && hook->known, view, zoom,
                 hook != NULL ? (int) hook->value.view : 0, hook != NULL ? fv_folder_view_zoom (&hook->value) : -1,
                 cover != NULL, hook != NULL && hook->switch_pending, checked, ok);
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
  else if (g_str_has_prefix (cmd, "fv-expect "))
    {
      GtkWidget *slot = debug_slot (-1);
      int want_view = 0, want_zoom = -1;
      if (sscanf (cmd + 10, "%d %d", &want_view, &want_zoom) != 2)
        g_message ("fv expect: bad arguments: %s", cmd + 10);
      else
        {
          NuViewType view = 0;
          int zoom = -1;
          gboolean shown = slot != NULL && shown_view (slot, &view, &zoom);
          SlotHook *hook = slot != NULL ? g_object_get_data (G_OBJECT (slot), SLOT_KEY) : NULL;
          g_message ("fv expect: ok=%d want=%d/%d shown=%d/%d uri=%s", shown && (int) view == want_view && zoom == want_zoom,
                     want_view, want_zoom, view, zoom, hook != NULL ? hook->uri : "-");
        }
    }
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
