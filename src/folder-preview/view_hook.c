/* All unsupported Nautilus-internals code lives here (PLAN.md §2). See view_hook.h and PLAN.md §6.4.
 *
 * Lifetime rules (PLAN.md §4.1, view_hook):
 *  - An ImageHook is stored as qdata on its NautilusImage and freed by GLib when the widget finalizes
 *    (or when we detach). No other code stores raw widget pointers.
 *  - Handlers on the image get a static marker as data (so they can be removed by data) and look the
 *    hook up through qdata; the cell's "notify::item" handler is connected with g_signal_connect_object
 *    so GLib disconnects it when the image dies.
 *  - Async results find their hook through a GWeakRef to the image plus a generation number that changes
 *    whenever the cell shows a different folder, so a late result can never land on a recycled cell.
 */
#ifdef FP_DEBUG_DRIVER
#define _GNU_SOURCE /* pthread_kill, backtrace: debug-only stall detector */
#endif
#include "view_hook.h"

#include <nautilus-extension.h>
#include <string.h>

#include "config.h"
#include "folder_paintable.h"
#include "icon_art.h"

#ifdef FP_DEBUG_DRIVER
#include <glib-unix.h>
#include <execinfo.h> /* backtrace (glibc), debug-only stall detector */
#include <fcntl.h>
#include <malloc.h> /* mallinfo2 (glibc), debug statistics only */
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

/* Phase 6 measurements (debug-driver builds only): how long each of our main-thread entry points runs
 * (any > 16 ms would be a stall attributable to the extension), and how long a folder icon waits for its
 * first preview. */
typedef enum { T_MAP, T_SYNC, T_FRAME_READY, T_HOVER_TICK, T_FOLDER_CHANGED, T_COUNT } TimingId;
static const char *const timing_names[T_COUNT] = { "map-hook", "sync", "frame-ready", "hover-tick", "folder-changed" };
typedef struct { guint64 count; gint64 total_us, max_us; guint over_16ms; } Timing;
static Timing timings[T_COUNT];
static GArray *latencies; /* gint64 µs from "cell shows folder" to "first preview drawn"; owned */

static void
timing_add (TimingId id, gint64 us)
{
  Timing *t = &timings[id];
  t->count++;
  t->total_us += us;
  t->max_us = MAX (t->max_us, us);
  if (us > 16000)
    t->over_16ms++;
}
#define TIME_BEGIN() gint64 fp_t0_ = g_get_monotonic_time ()
#define TIME_END(id) timing_add ((id), g_get_monotonic_time () - fp_t0_)
#else
#define TIME_BEGIN() (void) 0
#define TIME_END(id) (void) 0
#endif

#define HOOK_KEY   "fp-view-hook"
#define IGNORE_KEY "fp-view-ignore"

/* If a Nautilus grid view is shown but no icon could be hooked within this time, the widget structure
 * changed: warn once and disable (PLAN.md §6.5 Phase 5 "graceful failure"). */
#define DETECT_TIMEOUT_MS 3000

typedef struct _ImageHook ImageHook;

/* Name of a Nautilus type/property we depend on. Debug-driver builds can simulate a Nautilus that renamed
 * things ($FP_DEBUG_BREAK=types|props) to test the graceful-failure path; release builds use @real. */
static const char *
internal_name (const char *real, const char *kind)
{
#ifdef FP_DEBUG_DRIVER
  if (g_strcmp0 (g_getenv ("FP_DEBUG_BREAK"), kind) == 0)
    return "fp-debug-renamed";
#else
  (void) kind;
#endif
  return real;
}

typedef struct
{
  FpCache    *cache;          /* Owned ref. */
  guint       map_signal;
  gulong      map_hook_id;    /* Emission hook on GtkWidget::map; 0 once removed. */
  GType       image_type;     /* NautilusImage, 0 until Nautilus registers it. */
  GType       grid_cell_type; /* NautilusGridCell. */
  GType       view_cell_type; /* NautilusViewCell (has the "item" property). */
  gboolean    validated;
  gboolean    disabled;
  gboolean    off;            /* Switched off in the settings (PLAN.md §9): no hooks, map events ignored. */
  GHashTable *hooks;          /* Set of ImageHook* (borrowed: owned by the images' qdata). */
  guint       hover_timer;    /* The single hover timer in the process (PLAN.md §6.1.3). */
  ImageHook  *hover;          /* Borrowed; cleared by hook_free / hover_stop. */
  guint       detect_timer;   /* DETECT_TIMEOUT_MS watchdog, see on_detect_timeout. */
  guint       disable_idle;
  guint       n_attached_total;
  /* Look (PLAN.md §11), set by fp_view_hook_set_look. */
  FpStyle     style;          /* Same as the cache's style. */
  FpStackPosition position;   /* Stacked: where the stack goes (placement only: frames don't change). */
  int         offset;         /* Stacked: vertical fine-tuning, px at 64 × 64. */
  gboolean    cycle;          /* Hover cycling on. */
  FpIconArt  *back_art;       /* Owned or NULL: custom folder SVG (stacked layout only). */
  FpIconArt  *overlay_art;    /* Owned or NULL: front overlay SVG (stacked layout only). */
  char       *back_path;      /* Owned or NULL: path @back_art was loaded from (or failed to load from). */
  char       *overlay_path;   /* Owned or NULL: same for @overlay_art. */
  GHashTable *theme_bodies;   /* Owned: theme icon file URI (owned) → graphene_rect_t* (owned): its opaque body. */
} ViewHook;

static ViewHook *vh; /* NULL when not initialised. Main thread only. */

/* Data marker for handlers connected on images, so they can be disconnected by data. */
static int handler_marker;

struct _ImageHook
{
  GtkWidget         *image;          /* Borrowed: we are its qdata, so we never outlive it. */
  char              *uri;            /* Owned: folder currently shown by the cell, NULL if not previewable. */
  GFile             *location;       /* Owned: same folder, to restore Nautilus' "source". */
  guint64            generation;     /* Bumped whenever @uri changes. */
  GCancellable      *request;        /* Owned: in-flight frame-0 request, or NULL. */
  guint              request_px;
  GCancellable      *hover_request;  /* Owned: in-flight hover-frame request, or NULL. */
  guint              hover_target;   /* Frame the hover request is for. */
  FpFolderPaintable *paintable;      /* Owned: what we install as the image's fallback. */
  GdkPaintable      *icon;           /* Owned: Nautilus' own folder icon (last non-ours fallback). */
  guint              n_frames;       /* Frames available (hint from the cache); <= 1 means no hover. */
  guint              shown_frame;
  int                size;
  gboolean           applying;       /* We are setting the image's properties: ignore our own notifies. */
  guint              sync_id;        /* Pending sync idle, or 0. */
  GFileMonitor      *monitor;        /* Owned: watches @location while the icon is on screen (D15). */
  guint              changed_id;     /* Debounce timeout after a monitor event, or 0. */
  GtkEventController *motion;        /* Borrowed: owned by the image. */
  gint64             shown_since_us; /* When the cell started showing @uri (measurements). */
  gint64             first_frame_us; /* When the first preview for @uri was drawn, 0 = not yet. */
  gboolean           known_empty;    /* The cache says @uri has nothing to preview. */
};

static void sync_hook (ImageHook *hook);
static void disable (const char *reason);

/* ------------------------------------------------------------------------------------------------ */
/* Small helpers                                                                                    */

static ImageHook *
get_hook (GtkWidget *image)
{
  if (vh == NULL || vh->disabled)
    return NULL;
  return g_object_get_data (G_OBJECT (image), HOOK_KEY);
}

static void
cancel_and_clear (GCancellable **c)
{
  if (*c != NULL)
    g_cancellable_cancel (*c);
  g_clear_object (c);
}

static void
hover_stop (void)
{
  if (vh == NULL)
    return;
  g_clear_handle_id (&vh->hover_timer, g_source_remove);
  if (vh->hover != NULL)
    cancel_and_clear (&vh->hover->hover_request);
  vh->hover = NULL;
}

/* Returns a new reference to the NautilusFileInfo shown by @image's cell, or NULL. */
static NautilusFileInfo *
cell_file (GtkWidget *image)
{
  GtkWidget *cell = gtk_widget_get_ancestor (image, vh->view_cell_type); /* borrowed */
  if (cell == NULL)
    return NULL;
  g_autoptr (GObject) item = NULL;
  g_object_get (cell, "item", &item, NULL);
  if (item == NULL)
    return NULL;
  GParamSpec *pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (item), "file"); /* borrowed */
  if (pspec == NULL || !g_type_is_a (pspec->value_type, G_TYPE_OBJECT))
    {
      disable ("NautilusViewItem has no object property 'file'");
      return NULL;
    }
  g_autoptr (GObject) file = NULL;
  g_object_get (item, "file", &file, NULL);
  if (file == NULL || !NAUTILUS_IS_FILE_INFO (file))
    return NULL;
  return NAUTILUS_FILE_INFO (g_steal_pointer (&file));
}

/* ------------------------------------------------------------------------------------------------ */
/* Which icons get previews, and the decor (PLAN.md §11)                                            */

/* New string: URI of the file a theme icon was loaded from, or NULL if @icon isn't a theme/file icon. */
static char *
icon_file_uri (GdkPaintable *icon)
{
  if (icon == NULL || !GTK_IS_ICON_PAINTABLE (icon))
    return NULL;
  g_autoptr (GFile) file = gtk_icon_paintable_get_file (GTK_ICON_PAINTABLE (icon)); /* transfer full */
  return file != NULL ? g_file_get_uri (file) : NULL;
}

/* Only the theme's plain folder icon gets previews: never folder-pictures, folder-download, …, nor a custom
 * icon (§11.1). Nautilus looks the plain folder up as "inode-directory" first, then "folder" (Adwaita and
 * Hatter ship both: mimetypes/inode-directory.svg, places/folder.svg), so both names count (V6). */
static gboolean
is_plain_folder_icon (GdkPaintable *icon)
{
  if (icon == NULL || !GTK_IS_ICON_PAINTABLE (icon))
    return FALSE;
  g_autoptr (GFile) file = gtk_icon_paintable_get_file (GTK_ICON_PAINTABLE (icon)); /* transfer full */
  g_autofree char *base = file != NULL ? g_file_get_basename (file) : NULL;
  if (base == NULL)
    return FALSE;
  char *dot = strchr (base, '.');
  if (dot != NULL)
    *dot = '\0';
  return g_str_equal (base, "folder") || g_str_equal (base, "inode-directory");
}

/* Opaque body of the theme folder icon @icon, as fractions of the icon (measured once per icon file). */
static void
theme_body (GdkPaintable *icon, graphene_rect_t *out)
{
  graphene_rect_init (out, 0, 0, 1, 1); /* if it can't be measured: the whole icon */
  g_autofree char *uri = icon_file_uri (icon);
  if (uri == NULL)
    return;
  graphene_rect_t *known = g_hash_table_lookup (vh->theme_bodies, uri); /* borrowed */
  if (known == NULL)
    {
      g_autoptr (GdkTexture) probe = fp_render_paintable (icon, 256, 256);
      known = g_new (graphene_rect_t, 1);
      if (probe == NULL || !fp_texture_opaque_bounds (probe, known))
        graphene_rect_init (known, 0, 0, 1, 1);
      g_hash_table_insert (vh->theme_bodies, g_steal_pointer (&uri), known); /* table owns both */
    }
  *out = *known;
}

/* Where the frame goes for @hook's icon, without textures (cheap after the first call). Also returns the
 * folder body and whether the custom back SVG is used. */
static void
hook_geometry (ImageHook *hook, FpDecor *decor, graphene_rect_t *body, gboolean *custom_back)
{
  *custom_back = FALSE;
  if (vh->style.layout != FP_LAYOUT_STACKED)
    {
      fp_decor_init_grid (decor);
      graphene_rect_init (body, 0, 0, 1, 1);
      return;
    }
  *custom_back = vh->back_art != NULL && fp_icon_art_get_body (vh->back_art, body);
  if (!*custom_back)
    theme_body (hook->icon, body);
  fp_decor_init_stacked (decor, body, vh->position, vh->offset);
}

/* The full decor for @hook at its current size and scale; *decor gets owned texture refs (release with
 * decor_clear). */
static void
hook_decor (ImageHook *hook, FpDecor *decor)
{
  graphene_rect_t body;
  gboolean custom_back;
  hook_geometry (hook, decor, &body, &custom_back);
  if (decor->layout != FP_LAYOUT_STACKED || hook->size <= 0)
    return;
  int px = hook->size * gtk_widget_get_scale_factor (hook->image);
  if (custom_back)
    decor->back = fp_icon_art_render (vh->back_art, px, px);
  /* Same size and place as the folder icon (V4): the SVG itself positions the overlay. */
  if (vh->overlay_art != NULL)
    decor->overlay = fp_icon_art_render (vh->overlay_art, px, px);
}

static void
decor_clear (FpDecor *decor)
{
  g_clear_object (&decor->back);
  g_clear_object (&decor->overlay);
}

/* ------------------------------------------------------------------------------------------------ */
/* Installing / removing our paintable                                                              */

/* Shows @frame (may be NULL = icon only) through our paintable, installing it if needed. */
static void
install (ImageHook *hook, GdkTexture *frame)
{
#ifdef FP_DEBUG_DRIVER
  /* Phase 6 experiment: run the whole pipeline but never draw frames ($FP_DEBUG_NODRAW). */
  if (g_getenv ("FP_DEBUG_NODRAW") != NULL)
    frame = NULL;
#endif
  if (frame != NULL && hook->first_frame_us == 0)
    {
      hook->first_frame_us = g_get_monotonic_time ();
#ifdef FP_DEBUG_DRIVER
      gint64 latency = hook->first_frame_us - hook->shown_since_us;
      if (latencies != NULL && hook->shown_since_us > 0)
        g_array_append_val (latencies, latency);
#endif
    }
  if (hook->paintable == NULL)
    hook->paintable = fp_folder_paintable_new (hook->icon, hook->size);
  fp_folder_paintable_set_icon (hook->paintable, hook->icon);
  fp_folder_paintable_set_size (hook->paintable, hook->size);
  FpDecor decor;
  hook_decor (hook, &decor);
  fp_folder_paintable_set_decor (hook->paintable, &decor); /* takes its own refs */
  decor_clear (&decor);
  fp_folder_paintable_set_frame (hook->paintable, frame, hook->uri);

  g_autoptr (GdkPaintable) fallback = NULL;
  g_autoptr (GFile) source = NULL;
  g_object_get (hook->image, "fallback", &fallback, "source", &source, NULL);
  if (fallback == GDK_PAINTABLE (hook->paintable) && source == NULL)
    {
      /* Already installed: only our paintable's contents changed. NautilusImage isn't guaranteed to
       * listen to the paintable's invalidate-contents, so ask for a redraw explicitly. */
      gtk_widget_queue_draw (hook->image);
      return;
    }
  hook->applying = TRUE;
  g_object_set (hook->image, "source", NULL, "fallback", hook->paintable, NULL);
  hook->applying = FALSE;
}

/* Gives the image back to Nautilus if our paintable is installed. */
static void
uninstall (ImageHook *hook)
{
  if (hook->paintable != NULL)
    fp_folder_paintable_set_frame (hook->paintable, NULL, NULL);
  g_autoptr (GdkPaintable) fallback = NULL;
  g_object_get (hook->image, "fallback", &fallback, NULL);
  if (fallback == NULL || !FP_IS_FOLDER_PAINTABLE (fallback))
    return;
  hook->applying = TRUE;
  g_object_set (hook->image, "fallback", hook->icon, "source", hook->location, NULL);
  hook->applying = FALSE;
}

/* ------------------------------------------------------------------------------------------------ */
/* Requests                                                                                         */

typedef struct
{
  GWeakRef      image;       /* Weak: the cell may be destroyed while the request runs. */
  guint64       generation;
  guint         frame;
  guint         px;
  GCancellable *cancellable; /* Owned ref: identifies which request of the hook this is. */
} ReqCtx;

static void
req_ctx_free (ReqCtx *ctx)
{
  g_weak_ref_clear (&ctx->image);
  g_clear_object (&ctx->cancellable);
  g_free (ctx);
}

static void show_hover_frame (ImageHook *hook, guint frame);

/* Main thread: a cache request finished (the cache itself is already updated). Consumes @user_data. */
static void
on_frame_ready_impl (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) source;
  ReqCtx *ctx = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (FpFrame) frame = fp_cache_get_frame_finish (res, &error);
  if (error != NULL && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_debug ("frame request failed: %s", error->message);

  g_autoptr (GtkWidget) image = g_weak_ref_get (&ctx->image); /* new strong ref or NULL */
  ImageHook *hook = image != NULL ? get_hook (image) : NULL;
  if (hook != NULL && hook->generation == ctx->generation)
    {
      if (hook->request == ctx->cancellable)
        g_clear_object (&hook->request);
      if (hook->hover_request == ctx->cancellable)
        g_clear_object (&hook->hover_request);
      if (frame != NULL)
        {
          if (ctx->frame == 0)
            sync_hook (hook); /* the cache now has it: sync shows it */
          else if (vh->hover == hook && hook->hover_target == ctx->frame)
            show_hover_frame (hook, ctx->frame);
        }
    }
  req_ctx_free (ctx);
}

static void
on_frame_ready (GObject *source, GAsyncResult *res, gpointer user_data)
{
  TIME_BEGIN ();
  on_frame_ready_impl (source, res, user_data);
  TIME_END (T_FRAME_READY);
}

/* Starts an async request for @frame at @px; *slot receives the new cancellable (owned by the hook). */
static void
request (ImageHook *hook, guint frame, guint px, int priority, GCancellable **slot)
{
  cancel_and_clear (slot);
  *slot = g_cancellable_new ();
  ReqCtx *ctx = g_new0 (ReqCtx, 1);
  g_weak_ref_init (&ctx->image, hook->image);
  ctx->generation = hook->generation;
  ctx->frame = frame;
  ctx->px = px;
  ctx->cancellable = g_object_ref (*slot);
  fp_cache_get_frame_async (vh->cache, hook->location, frame, px, priority, *slot, on_frame_ready, ctx);
}

/* ------------------------------------------------------------------------------------------------ */
/* Sync: make the icon match the cell's current folder, size and cache state                        */

/* ---- Watching on-screen folders for changes (D15) ---- */

#define CHANGE_DEBOUNCE_MS 600

static void
monitor_stop (ImageHook *hook)
{
  g_clear_handle_id (&hook->changed_id, g_source_remove);
  if (hook->monitor != NULL)
    {
      g_signal_handlers_disconnect_by_data (hook->monitor, hook);
      g_file_monitor_cancel (hook->monitor);
      g_clear_object (&hook->monitor);
    }
}

static gboolean
on_change_settled (gpointer data)
{
  ImageHook *hook = data;
  hook->changed_id = 0;
  if (hook->uri != NULL)
    {
      g_autofree char *uri = g_strdup (hook->uri); /* folder_changed may resync (not free) this hook */
      fp_view_hook_folder_changed (uri);
    }
  return G_SOURCE_REMOVE;
}

/* Something inside the folder changed: refresh its preview once things settle. */
static void
on_folder_changed (GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, gpointer data)
{
  (void) monitor;
  (void) file;
  (void) other;
  (void) event;
  ImageHook *hook = data;
  g_clear_handle_id (&hook->changed_id, g_source_remove);
  hook->changed_id = g_timeout_add (CHANGE_DEBOUNCE_MS, on_change_settled, hook);
}

/* Starts watching hook->location if the icon is on screen and not already watched. */
static void
monitor_start (ImageHook *hook)
{
  if (hook->monitor != NULL || hook->location == NULL || !gtk_widget_get_mapped (hook->image))
    return;
  g_autoptr (GError) error = NULL;
  hook->monitor = g_file_monitor_directory (hook->location, G_FILE_MONITOR_WATCH_MOVES, NULL, &error);
  if (hook->monitor == NULL)
    {
      g_debug ("cannot watch %s: %s", hook->uri, error->message);
      return;
    }
  g_signal_connect (hook->monitor, "changed", G_CALLBACK (on_folder_changed), hook);
}

/* Frame width in device pixels for @hook's icon at its current size, layout and scale. */
static guint
current_px (ImageHook *hook)
{
  FpDecor decor;
  graphene_rect_t body;
  gboolean custom_back;
  hook_geometry (hook, &decor, &body, &custom_back);
  return fp_decor_frame_px (&decor, hook->size, gtk_widget_get_scale_factor (hook->image));
}

static void
reset_for_new_folder (ImageHook *hook)
{
  monitor_stop (hook);
  cancel_and_clear (&hook->request);
  if (vh->hover == hook)
    hover_stop ();
  cancel_and_clear (&hook->hover_request);
  hook->generation++;
  hook->n_frames = 0;
  hook->shown_frame = 0;
  /* Never let a recycled cell show the previous folder's preview, not even for one frame. */
  if (hook->paintable != NULL)
    fp_folder_paintable_set_frame (hook->paintable, NULL, NULL);
}

static void
sync_hook (ImageHook *hook)
{
  GtkWidget *image = hook->image;
  g_autoptr (NautilusFileInfo) file = cell_file (image);
  /* cell_file() may have disabled the hook (Nautilus internals changed). */
  if (vh == NULL || vh->disabled)
    return;

  g_autoptr (GFile) location = NULL;
  g_autofree char *uri = NULL;
  if (file != NULL && nautilus_file_info_is_directory (file))
    {
      location = nautilus_file_info_get_location (file);
      if (location != NULL && g_file_is_native (location))
        uri = g_file_get_uri (location);
    }

  if (g_strcmp0 (uri, hook->uri) != 0)
    {
      reset_for_new_folder (hook);
      g_free (hook->uri);
      hook->uri = g_steal_pointer (&uri);
      hook->shown_since_us = g_get_monotonic_time ();
      hook->first_frame_us = 0;
      hook->known_empty = FALSE;
      g_set_object (&hook->location, location);
    }
  if (hook->uri == NULL)
    {
      uninstall (hook);
      return;
    }

  /* Remember Nautilus' own folder icon: whatever fallback it set that isn't ours. */
  g_autoptr (GdkPaintable) fallback = NULL;
  int size = 0;
  g_object_get (image, "fallback", &fallback, "size", &size, NULL);
  if (fallback != NULL && !FP_IS_FOLDER_PAINTABLE (fallback))
    g_set_object (&hook->icon, fallback);
  hook->size = size;

  if (!is_plain_folder_icon (hook->icon))
    {
      uninstall (hook); /* folder-pictures, folder-download, custom icons, …: no preview (§11.1) */
      monitor_stop (hook);
      return;
    }
  guint px = current_px (hook);
  if (px < 8)
    {
      uninstall (hook);
      return;
    }
  monitor_start (hook);

  guint want = vh->hover == hook ? hook->shown_frame : 0;
  g_autoptr (GdkTexture) texture = NULL;
  guint hint = 0;
  if (fp_cache_peek_frame (vh->cache, hook->uri, want, px, &texture, &hint))
    {
      if (want == 0)
        hook->n_frames = hint;
      if (texture == NULL && want == 0)
        {
          hook->known_empty = TRUE;
          uninstall (hook); /* nothing to preview: plain folder icon */
          return;
        }
      install (hook, texture);
      return;
    }

  /* Not cached at this size yet. Keep showing this folder's current frame (scaled) if we have one — this
   * also re-applies our paintable after Nautilus reset it — and ask for the right size. */
  if (hook->paintable != NULL && fp_folder_paintable_get_frame (hook->paintable) != NULL &&
      g_strcmp0 (fp_folder_paintable_get_frame_uri (hook->paintable), hook->uri) == 0)
    install (hook, fp_folder_paintable_get_frame (hook->paintable));
  else
    {
      /* A new cell (e.g. Nautilus rebuilt the view after a zoom): show this folder's frame from another
       * cached size, scaled, instead of a plain icon until the right size is built. */
      g_autoptr (GdkTexture) other = fp_cache_peek_frame_any_size (vh->cache, hook->uri, want);
      if (other != NULL)
        install (hook, other);
    }

  if (gtk_widget_get_mapped (image) && !(hook->request != NULL && hook->request_px == px))
    {
      hook->request_px = px;
      request (hook, 0, px, G_PRIORITY_DEFAULT, &hook->request);
    }
}

static gboolean
sync_idle (gpointer data)
{
  ImageHook *hook = data;
  hook->sync_id = 0;
  TIME_BEGIN ();
  if (vh != NULL && !vh->disabled)
    sync_hook (hook);
  TIME_END (T_SYNC);
  return G_SOURCE_REMOVE;
}

/* Coalesces bursts of notifications (Nautilus sets several properties per rebind) into one sync that runs
 * before the next redraw. */
static void
schedule_sync (ImageHook *hook)
{
  if (hook->sync_id == 0)
    hook->sync_id = g_idle_add_full (G_PRIORITY_HIGH_IDLE + 10, sync_idle, hook, NULL);
}

/* ------------------------------------------------------------------------------------------------ */
/* Hover (PLAN.md §6.1.3)                                                                                */

/* Shows hover frame @frame if cached, else requests it (shown on arrival if still wanted). */
static void
show_hover_frame (ImageHook *hook, guint frame)
{
  guint px = current_px (hook);
  g_autoptr (GdkTexture) texture = NULL;
  guint hint = 0;
  if (!fp_cache_peek_frame (vh->cache, hook->uri, frame, px, &texture, &hint))
    {
      /* Shown when it arrives (on_frame_ready), if it is still the wanted frame then. A prefetch of the
       * same frame may already be running: then just mark it as wanted. */
      gboolean same_in_flight = hook->hover_request != NULL && hook->hover_target == G_MAXUINT && frame == 1;
      hook->hover_target = frame;
      if (!same_in_flight)
        request (hook, frame, px, G_PRIORITY_DEFAULT - 10, &hook->hover_request);
      return;
    }
  if (texture == NULL && frame != 0)
    {
      /* Fewer frames than the hint promised (some thumbnails failed): wrap around from here on. */
      hook->n_frames = frame;
      show_hover_frame (hook, 0);
      return;
    }
  hook->shown_frame = frame;
  install (hook, texture);
}

static gboolean
hover_tick_impl (gpointer data)
{
  (void) data;
  ImageHook *hook = vh != NULL ? vh->hover : NULL;
  if (hook == NULL || hook->n_frames <= 1)
    {
      if (vh != NULL)
        vh->hover_timer = 0;
      hover_stop ();
      return G_SOURCE_REMOVE;
    }
  guint next = hook->shown_frame + 1;
  if (next >= hook->n_frames)
    next = 0;
  show_hover_frame (hook, next);
  return G_SOURCE_CONTINUE;
}

static gboolean
hover_tick (gpointer data)
{
  TIME_BEGIN ();
  gboolean again = hover_tick_impl (data);
  TIME_END (T_HOVER_TICK);
  return again;
}

static void
hover_start (ImageHook *hook)
{
  if (!vh->cycle || hook->uri == NULL || hook->n_frames <= 1 || hook->paintable == NULL ||
      fp_folder_paintable_get_frame (hook->paintable) == NULL)
    return;
  hover_stop ();
  vh->hover = hook;
  hook->shown_frame = 0;
  vh->hover_timer = g_timeout_add (HOVER_FRAME_MS, hover_tick, NULL);
  /* Prefetch the first hover frame so it is ready when the timer fires. hover_target stays "none": the
   * prefetch only fills the cache; frame 1 is shown by the timer, after HOVER_FRAME_MS. */
  hook->hover_target = G_MAXUINT;
  guint px = current_px (hook);
  g_autoptr (GdkTexture) unused = NULL;
  if (!fp_cache_peek_frame (vh->cache, hook->uri, 1, px, &unused, NULL))
    request (hook, 1, px, G_PRIORITY_DEFAULT - 10, &hook->hover_request);
}

static void
hover_end (ImageHook *hook)
{
  if (vh->hover != hook)
    return;
  hover_stop ();
  hook->shown_frame = 0;
  sync_hook (hook); /* back to frame 0 immediately */
}

/* ------------------------------------------------------------------------------------------------ */
/* Signal handlers (data = &handler_marker or the image)                                            */

static void
on_image_notify (GObject *image, GParamSpec *pspec, gpointer data)
{
  (void) pspec;
  (void) data;
  ImageHook *hook = get_hook (GTK_WIDGET (image));
  if (hook != NULL && !hook->applying)
    schedule_sync (hook);
}

static void
on_cell_item (GObject *cell, GParamSpec *pspec, gpointer image)
{
  (void) cell;
  (void) pspec;
  ImageHook *hook = get_hook (image);
  if (hook != NULL)
    schedule_sync (hook);
}

static void
on_image_map (GtkWidget *image, gpointer data)
{
  (void) data;
  ImageHook *hook = get_hook (image);
  if (hook == NULL)
    return;
  /* Latency is measured from when the folder becomes visible (cells are bound off-screen in advance). */
  if (hook->first_frame_us == 0)
    hook->shown_since_us = g_get_monotonic_time ();
  schedule_sync (hook);
}

/* Scrolled out of view: drop the request (PLAN.md §6.2 rule 6) and any hover. */
static void
on_image_unmap (GtkWidget *image, gpointer data)
{
  (void) data;
  ImageHook *hook = get_hook (image);
  if (hook == NULL)
    return;
  cancel_and_clear (&hook->request);
  monitor_stop (hook);
  if (vh->hover == hook)
    hover_end (hook);
  /* GTK keeps hundreds of off-screen cells alive; don't let each pin its frame texture (the cache keeps
   * what's worth keeping, within its budget). When the icon is shown again, sync takes the frame from the
   * cache — the same idle runs before the next redraw — or from disk within a few ms. */
  if (hook->paintable != NULL)
    fp_folder_paintable_set_frame (hook->paintable, NULL, NULL);
}

static void
on_enter (GtkEventControllerMotion *motion, double x, double y, gpointer data)
{
  (void) x;
  (void) y;
  (void) data;
  ImageHook *hook = get_hook (gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (motion)));
  if (hook != NULL)
    hover_start (hook);
}

static void
on_leave (GtkEventControllerMotion *motion, gpointer data)
{
  (void) data;
  ImageHook *hook = get_hook (gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (motion)));
  if (hook != NULL)
    hover_end (hook);
}

/* ------------------------------------------------------------------------------------------------ */
/* Attach / detach                                                                                  */

/* qdata destroy notify: runs when the image finalizes, or when we detach. Must not touch the widget in the
 * finalize case, so it only releases our own state. */
static void
hook_free (gpointer data)
{
  ImageHook *hook = data;
  g_clear_handle_id (&hook->sync_id, g_source_remove);
  monitor_stop (hook);
  cancel_and_clear (&hook->request);
  cancel_and_clear (&hook->hover_request);
  if (vh != NULL)
    {
      if (vh->hover == hook)
        hover_stop ();
      g_hash_table_remove (vh->hooks, hook);
    }
  g_clear_object (&hook->paintable);
  g_clear_object (&hook->icon);
  g_clear_object (&hook->location);
  g_clear_pointer (&hook->uri, g_free);
  g_free (hook);
}

/* Checks once that NautilusImage / NautilusViewCell still have what Phase 0 found. */
static gboolean
validate (GtkWidget *image)
{
  /* We write source/fallback and only read size/item. */
  struct { GType owner; const char *name; GType type; GParamFlags need; } props[] = {
    { G_OBJECT_TYPE (image), internal_name ("source", "props"), G_TYPE_FILE, G_PARAM_READWRITE },
    { G_OBJECT_TYPE (image), "fallback", GDK_TYPE_PAINTABLE, G_PARAM_READWRITE },
    { G_OBJECT_TYPE (image), "size", G_TYPE_INT, G_PARAM_READABLE },
    { vh->view_cell_type, "item", G_TYPE_OBJECT, G_PARAM_READABLE },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (props); i++)
    {
      g_autoptr (GTypeClass) klass = g_type_class_ref (props[i].owner);
      GParamSpec *pspec = g_object_class_find_property (G_OBJECT_CLASS (klass), props[i].name);
      if (pspec == NULL || !g_type_is_a (pspec->value_type, props[i].type) ||
          (pspec->flags & props[i].need) != props[i].need)
        {
          g_autofree char *reason = g_strdup_printf ("%s has no read/write %s property '%s'",
                                                     g_type_name (props[i].owner), g_type_name (props[i].type),
                                                     props[i].name);
          disable (reason);
          return FALSE;
        }
    }
  return TRUE;
}

static void
attach (GtkWidget *image, GtkWidget *cell)
{
  ImageHook *hook = g_new0 (ImageHook, 1);
  hook->image = image;
  g_object_set_data_full (G_OBJECT (image), HOOK_KEY, hook, hook_free); /* image owns @hook */
  g_hash_table_add (vh->hooks, hook);
  vh->n_attached_total++;

  g_signal_connect (image, "notify::source", G_CALLBACK (on_image_notify), &handler_marker);
  g_signal_connect (image, "notify::fallback", G_CALLBACK (on_image_notify), &handler_marker);
  g_signal_connect (image, "notify::size", G_CALLBACK (on_image_notify), &handler_marker);
  g_signal_connect (image, "map", G_CALLBACK (on_image_map), &handler_marker);
  g_signal_connect (image, "unmap", G_CALLBACK (on_image_unmap), &handler_marker);
  /* Disconnected by GLib when the image is finalized. */
  g_signal_connect_object (cell, "notify::item", G_CALLBACK (on_cell_item), image, G_CONNECT_DEFAULT);

  GtkEventController *motion = gtk_event_controller_motion_new ();
  g_signal_connect (motion, "enter", G_CALLBACK (on_enter), &handler_marker);
  g_signal_connect (motion, "leave", G_CALLBACK (on_leave), &handler_marker);
  gtk_widget_add_controller (image, motion); /* image owns @motion */
  hook->motion = motion;

  schedule_sync (hook);
}

/* Restores the icon and removes everything we added to @hook's image. Frees @hook. */
static void
detach (ImageHook *hook)
{
  GtkWidget *image = hook->image;
  uninstall (hook);
  g_signal_handlers_disconnect_by_data (image, &handler_marker);
  GtkWidget *cell = vh->view_cell_type != 0 ? gtk_widget_get_ancestor (image, vh->view_cell_type) : NULL;
  if (cell != NULL)
    g_signal_handlers_disconnect_by_data (cell, image);
  if (hook->motion != NULL)
    gtk_widget_remove_controller (image, hook->motion);
  g_object_set_data (G_OBJECT (image), HOOK_KEY, NULL); /* runs hook_free */
}

static void
detach_all (void)
{
  hover_stop ();
  g_autoptr (GPtrArray) all = g_ptr_array_new ();
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init (&iter, vh->hooks);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    g_ptr_array_add (all, key);
  for (guint i = 0; i < all->len; i++)
    detach (g_ptr_array_index (all, i));
}

static gboolean
disable_idle (gpointer data)
{
  (void) data;
  vh->disable_idle = 0;
  detach_all ();
  return G_SOURCE_REMOVE;
}

/* Turns the hook off. The detaching happens in an idle, so a hook that triggered this from inside one of
 * its own callbacks is never freed under its feet. */
static void
disable (const char *reason)
{
  if (vh == NULL || vh->disabled)
    return;
  g_warning ("folder previews disabled: %s (Nautilus internals changed; please update the extension)", reason);
  vh->disabled = TRUE;
  g_clear_handle_id (&vh->detect_timer, g_source_remove);
  vh->disable_idle = g_idle_add (disable_idle, NULL);
}

static gboolean
on_detect_timeout (gpointer data)
{
  (void) data;
  vh->detect_timer = 0;
  if (vh->n_attached_total == 0)
    disable ("a Nautilus grid view is shown but no NautilusImage inside a NautilusGridCell was found");
  return G_SOURCE_REMOVE;
}

/* Called for a newly mapped NautilusImage. */
static void
maybe_attach (GtkWidget *image)
{
  if (vh->grid_cell_type == 0)
    vh->grid_cell_type = g_type_from_name ("NautilusGridCell");
  if (vh->view_cell_type == 0)
    vh->view_cell_type = g_type_from_name ("NautilusViewCell");
  GtkWidget *cell = vh->grid_cell_type != 0 ? gtk_widget_get_ancestor (image, vh->grid_cell_type) : NULL;
  if (cell == NULL || vh->view_cell_type == 0 || !g_type_is_a (vh->grid_cell_type, vh->view_cell_type))
    {
      /* List view, sidebar, … — not ours (grid view only in v1). Remember so we don't look again. */
      g_object_set_data (G_OBJECT (image), IGNORE_KEY, GINT_TO_POINTER (1));
      return;
    }
  if (!vh->validated)
    {
      if (!validate (image))
        return;
      vh->validated = TRUE;
    }
  attach (image, cell);
}

/* Emission hook for GtkWidget::map: sees every widget that becomes visible, in every window and tab. */
static gboolean
on_widget_map_impl (GSignalInvocationHint *ihint, guint n_params, const GValue *params, gpointer data)
{
  (void) ihint;
  (void) data;
  if (vh == NULL || vh->disabled || n_params < 1)
    {
      if (vh != NULL)
        vh->map_hook_id = 0; /* returning FALSE removes the hook */
      return FALSE;
    }
  if (vh->off)
    return TRUE; /* keep the hook: switching back on must not need a restart */
  GObject *obj = g_value_get_object (&params[0]); /* borrowed */

  /* Watchdog: the first time a Nautilus grid view appears, expect icons to be hooked soon. */
  if (vh->detect_timer == 0 && vh->n_attached_total == 0 && GTK_IS_GRID_VIEW (obj))
    {
      GtkWidget *parent = gtk_widget_get_parent (GTK_WIDGET (obj));
      for (; parent != NULL; parent = gtk_widget_get_parent (parent))
        if (g_str_has_prefix (G_OBJECT_TYPE_NAME (parent), "Nautilus"))
          {
            vh->detect_timer = g_timeout_add (DETECT_TIMEOUT_MS, on_detect_timeout, NULL);
            break;
          }
    }

  if (vh->image_type == 0)
    {
      vh->image_type = g_type_from_name (internal_name ("NautilusImage", "types"));
      if (vh->image_type == 0)
        return TRUE;
    }
  if (!G_TYPE_CHECK_INSTANCE_TYPE (obj, vh->image_type))
    return TRUE;
  if (g_object_get_data (obj, HOOK_KEY) != NULL || g_object_get_data (obj, IGNORE_KEY) != NULL)
    return TRUE;
  maybe_attach (GTK_WIDGET (obj));
  return TRUE;
}

static gboolean
on_widget_map (GSignalInvocationHint *ihint, guint n_params, const GValue *params, gpointer data)
{
  TIME_BEGIN ();
  gboolean keep = on_widget_map_impl (ihint, n_params, params, data);
  TIME_END (T_MAP);
  return keep;
}

/* ------------------------------------------------------------------------------------------------ */
/* Debug driver (only in -Ddebug_driver=true builds, used by tools/smoke-nautilus.sh)                */

#ifdef FP_DEBUG_DRIVER
static guint debug_signal_id;

/* The debug driver only ever acts on icons under $FP_DEBUG_ROOT (the test tree), so it never scrolls,
 * zooms or hovers windows the user is working in. */
static gboolean
under_debug_root (ImageHook *hook)
{
  const char *root = g_getenv ("FP_DEBUG_ROOT");
  g_autofree char *root_uri = root != NULL ? g_filename_to_uri (root, NULL, NULL) : NULL;
  return root_uri != NULL && hook->uri != NULL && g_str_has_prefix (hook->uri, root_uri);
}

static GtkWidget *
first_mapped_grid (void)
{
  GHashTableIter it;
  gpointer k;
  g_hash_table_iter_init (&it, vh->hooks);
  while (g_hash_table_iter_next (&it, &k, NULL))
    {
      ImageHook *h = k;
      if (gtk_widget_get_mapped (h->image) && under_debug_root (h))
        return gtk_widget_get_ancestor (h->image, GTK_TYPE_GRID_VIEW);
    }
  /* No hooked icons (baseline runs with the hook disabled, or a folder without subfolders): fall back to
   * the first mapped grid view in any window. Only used by isolated test instances (FP_DEBUG_ROOT set),
   * which have no other windows. */
  if (g_getenv ("FP_DEBUG_ROOT") == NULL)
    return NULL;
  GListModel *tops = gtk_window_get_toplevels (); /* borrowed */
  for (guint i = 0; i < g_list_model_get_n_items (tops); i++)
    {
      g_autoptr (GtkWidget) win = g_list_model_get_item (tops, i);
      g_autoptr (GPtrArray) stack = g_ptr_array_new (); /* borrowed widget pointers, DFS */
      g_ptr_array_add (stack, win);
      while (stack->len > 0)
        {
          GtkWidget *w = g_ptr_array_steal_index (stack, stack->len - 1);
          if (GTK_IS_GRID_VIEW (w) && gtk_widget_get_mapped (w))
            return w;
          for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL; c = gtk_widget_get_next_sibling (c))
            g_ptr_array_add (stack, c);
        }
    }
  return NULL;
}

/* Checks every mapped hooked icon: an installed frame must belong to the folder the cell shows now. */
static void
debug_verify (void)
{
  guint ok = 0, wrong = 0, plain = 0, unmapped = 0;
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init (&iter, vh->hooks);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      ImageHook *hook = key;
      if (!under_debug_root (hook) && hook->uri != NULL)
        continue;
      if (!gtk_widget_get_mapped (hook->image))
        {
          unmapped++;
          continue;
        }
      g_autoptr (GdkPaintable) fallback = NULL;
      g_object_get (hook->image, "fallback", &fallback, NULL);
      if (fallback == NULL || !FP_IS_FOLDER_PAINTABLE (fallback) ||
          fp_folder_paintable_get_frame (FP_FOLDER_PAINTABLE (fallback)) == NULL)
        {
          plain++;
          continue;
        }
      g_autoptr (NautilusFileInfo) file = cell_file (hook->image);
      g_autofree char *uri = file != NULL ? nautilus_file_info_get_uri (file) : NULL;
      const char *frame_uri = fp_folder_paintable_get_frame_uri (FP_FOLDER_PAINTABLE (fallback));
      if (g_strcmp0 (uri, frame_uri) == 0)
        ok++;
      else
        {
          wrong++;
          g_message ("debug verify: WRONG cell=%s frame=%s", uri, frame_uri);
        }
    }
  g_message ("debug verify: ok=%u wrong=%u plain=%u unmapped=%u hooks=%u cache-folders=%u cache-bytes=%zu",
             ok, wrong, plain, unmapped, g_hash_table_size (vh->hooks), fp_cache_get_n_folders (vh->cache),
             fp_cache_get_n_bytes (vh->cache));
}

static void
debug_save_png (void)
{
  const char *dir = g_getenv ("FP_DEBUG_DIR");
  GtkWidget *grid = first_mapped_grid ();
  if (dir == NULL || grid == NULL)
    return;
  GtkRoot *root = gtk_widget_get_root (grid);
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (root)); /* borrowed */
  GdkToplevelState state = GDK_IS_TOPLEVEL (surface) ? gdk_toplevel_get_state (GDK_TOPLEVEL (surface)) : 0;
  g_message ("debug: window state suspended=%d minimized=%d focused=%d",
             (state & GDK_TOPLEVEL_STATE_SUSPENDED) != 0, (state & GDK_TOPLEVEL_STATE_MINIMIZED) != 0,
             (state & GDK_TOPLEVEL_STATE_FOCUSED) != 0);
  GskRenderer *renderer = gtk_native_get_renderer (GTK_NATIVE (root)); /* borrowed */
  g_autoptr (GdkPaintable) wp = gtk_widget_paintable_new (GTK_WIDGET (root));
  GtkSnapshot *snap = gtk_snapshot_new ();
  gdk_paintable_snapshot (wp, snap, gtk_widget_get_width (GTK_WIDGET (root)), gtk_widget_get_height (GTK_WIDGET (root)));
  g_autoptr (GskRenderNode) node = gtk_snapshot_free_to_node (snap);
  if (node == NULL || renderer == NULL)
    {
      g_message ("debug: nothing rendered (window not drawn?)");
      return;
    }
  g_autoptr (GdkTexture) tex = gsk_renderer_render_texture (renderer, node, NULL);
  g_autofree char *path = g_build_filename (dir, "window.png", NULL);
  gdk_texture_save_to_png (tex, path);
  g_message ("debug: saved %s", path);
}

/* Starts hovering the first mapped icon that has hover frames (simulates pointer enter). */
static void
debug_hover (gboolean start)
{
  if (!start)
    {
      if (vh->hover != NULL)
        hover_end (vh->hover);
      return;
    }
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init (&iter, vh->hooks);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      ImageHook *hook = key;
      if (gtk_widget_get_mapped (hook->image) && hook->n_frames > 1 && under_debug_root (hook))
        {
          hover_start (hook);
          g_message ("debug: hovering %s (%u frames)", hook->uri, hook->n_frames);
          return;
        }
    }
  g_message ("debug: nothing to hover");
}

static guint probe_id;
static gint64 probe_expected_us;
static guint probe_ticks, probe_late16, probe_late50;
static gint64 probe_max_late_us;
static gint64 probe_last_tick_us; /* written by the main thread, read (racily) by the stall watchdog */

/* 16 ms main-loop probe (PLAN.md §6.5 Phases 2/6): how late does a 16 ms timer fire while Nautilus works? */
static gboolean
probe_tick (gpointer data)
{
  (void) data;
  gint64 now = g_get_monotonic_time ();
  gint64 late = MAX (0, now - probe_expected_us);
  probe_ticks++;
  probe_max_late_us = MAX (probe_max_late_us, late);
  if (late > 16000)
    probe_late16++;
  if (late > 50000)
    probe_late50++;
  probe_expected_us = now + 16000;
  probe_last_tick_us = now; /* read by the stall watchdog; a racy read only shifts one sample */
  return G_SOURCE_CONTINUE;
}

/* ---- Stall detector ($FP_DEBUG_STALLS=<file>): when the probe hasn't ticked for > 60 ms, a watchdog
 * thread signals the main thread, whose handler appends its backtrace to <file>. Debug builds only. ---- */
static pthread_t main_thread_id;
static int stall_fd = -1;
static gint stall_running;
static GThread *stall_thread;

static void
on_stall_signal (int sig)
{
  (void) sig;
  void *frames[48];
  int n = backtrace (frames, G_N_ELEMENTS (frames));
  static const char sep[] = "---- stall\n";
  if (write (stall_fd, sep, sizeof sep - 1) < 0)
    return;
  backtrace_symbols_fd (frames, n, stall_fd);
}

static gpointer
stall_watchdog (gpointer data)
{
  (void) data;
  gint64 reported_for = 0;
  const char *ms = g_getenv ("FP_DEBUG_STALL_MS");
  gint64 threshold_us = (ms != NULL ? g_ascii_strtoll (ms, NULL, 10) : 60) * 1000;
  while (g_atomic_int_get (&stall_running))
    {
      g_usleep (3 * 1000);
      gint64 last = probe_last_tick_us; /* racy read is fine for a debug sampler */
      gint64 now = g_get_monotonic_time ();
      if (last != 0 && now - last > threshold_us && reported_for != last)
        {
          reported_for = last; /* one sample per stall */
          pthread_kill (main_thread_id, SIGURG);
        }
    }
  return NULL;
}

static void
stall_detector_start (const char *path)
{
  stall_fd = open (path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (stall_fd < 0)
    return;
  void *warm[4];
  backtrace (warm, 4); /* first call may allocate: do it outside the signal handler */
  main_thread_id = pthread_self ();
  struct sigaction sa = { 0 };
  sa.sa_handler = on_stall_signal;
  sa.sa_flags = SA_RESTART;
  sigaction (SIGURG, &sa, NULL);
  g_atomic_int_set (&stall_running, 1);
  stall_thread = g_thread_new ("fp-stall-watchdog", stall_watchdog, NULL);
}

static void
stall_detector_stop (void)
{
  if (stall_thread == NULL)
    return;
  g_atomic_int_set (&stall_running, 0);
  g_thread_join (g_steal_pointer (&stall_thread));
  signal (SIGURG, SIG_DFL);
  close (stall_fd);
  stall_fd = -1;
}

static int
cmp_i64 (gconstpointer a, gconstpointer b)
{
  gint64 x = *(const gint64 *) a, y = *(const gint64 *) b;
  return x < y ? -1 : x > y;
}

static void
debug_latency (void)
{
  guint mapped = 0, ready = 0, pending = 0, empty = 0;
  GHashTableIter it;
  gpointer k;
  g_hash_table_iter_init (&it, vh->hooks);
  while (g_hash_table_iter_next (&it, &k, NULL))
    {
      ImageHook *h = k;
      if (h->uri == NULL || !under_debug_root (h) || !gtk_widget_get_mapped (h->image))
        continue;
      mapped++;
      if (h->known_empty)
        empty++;
      else if (h->first_frame_us != 0)
        ready++;
      else
        pending++;
    }
  g_autoptr (GArray) sorted = g_array_copy (latencies);
  g_array_sort (sorted, cmp_i64);
  guint n = sorted->len;
  double p50 = n ? g_array_index (sorted, gint64, n / 2) / 1000.0 : 0;
  double p95 = n ? g_array_index (sorted, gint64, MIN (n - 1, n * 95 / 100)) / 1000.0 : 0;
  double max = n ? g_array_index (sorted, gint64, n - 1) / 1000.0 : 0;
  g_message ("debug latency: mapped=%u ready=%u pending=%u empty=%u samples=%u p50=%.1fms p95=%.1fms max=%.1fms",
             mapped, ready, pending, empty, n, p50, p95, max);
}

static long
rss_kb (void)
{
  g_autofree char *status = NULL;
  if (!g_file_get_contents ("/proc/self/status", &status, NULL, NULL))
    return -1;
  const char *line = strstr (status, "VmRSS:");
  return line != NULL ? strtol (line + 6, NULL, 10) : -1;
}

static void
debug_stats (void)
{
  g_message ("debug stats: rss=%ld hooks=%u cache-folders=%u cache-bytes=%zu probe-ticks=%u late16=%u "
             "late50=%u max-late=%.1fms",
             rss_kb (), g_hash_table_size (vh->hooks), fp_cache_get_n_folders (vh->cache),
             fp_cache_get_n_bytes (vh->cache), probe_ticks, probe_late16, probe_late50, probe_max_late_us / 1000.0);
  struct mallinfo2 mi = mallinfo2 ();
  g_message ("debug malloc: heap-in-use=%zu KB heap-free-held=%zu KB mmapped=%zu KB",
             mi.uordblks / 1024, mi.fordblks / 1024, mi.hblkhd / 1024);
  for (guint i = 0; i < T_COUNT; i++)
    g_message ("debug timing: %s count=%" G_GUINT64_FORMAT " max=%.2fms avg=%.3fms over16=%u", timing_names[i],
               timings[i].count, timings[i].max_us / 1000.0,
               timings[i].count ? (double) timings[i].total_us / (double) timings[i].count / 1000.0 : 0.0,
               timings[i].over_16ms);
}

static void
debug_stats_reset (void)
{
  memset (timings, 0, sizeof timings);
  g_array_set_size (latencies, 0);
  probe_ticks = probe_late16 = probe_late50 = 0;
  probe_max_late_us = 0;
  probe_expected_us = g_get_monotonic_time () + 16000;
}

/* Counts icons that currently show our paintable, in every window (must be 0 while switched off). */
static guint
count_installed (GtkWidget *w)
{
  guint n = 0;
  if (vh->image_type != 0 && G_TYPE_CHECK_INSTANCE_TYPE (w, vh->image_type))
    {
      g_autoptr (GdkPaintable) fallback = NULL;
      g_object_get (w, "fallback", &fallback, NULL);
      return fallback != NULL && FP_IS_FOLDER_PAINTABLE (fallback) ? 1 : 0;
    }
  for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL; c = gtk_widget_get_next_sibling (c))
    n += count_installed (c);
  return n;
}

static void
debug_state (void)
{
  guint installed = 0;
  GListModel *toplevels = gtk_window_get_toplevels (); /* borrowed */
  for (guint i = 0; i < g_list_model_get_n_items (toplevels); i++)
    {
      g_autoptr (GtkWidget) window = g_list_model_get_item (toplevels, i);
      installed += count_installed (window);
    }
  g_message ("fp state: off=%d hooks=%u installed=%u layout=%d cycle=%d back=%d overlay=%d", vh->off,
             g_hash_table_size (vh->hooks), installed, vh->style.layout, vh->cycle, vh->back_art != NULL,
             vh->overlay_art != NULL);
}

static gboolean
on_debug_signal (gpointer data)
{
  (void) data;
  const char *dir = g_getenv ("FP_DEBUG_DIR");
  g_autofree char *path = dir != NULL ? g_build_filename (dir, "cmd", NULL) : NULL;
  g_autofree char *cmd = NULL;
  if (vh == NULL || path == NULL || !g_file_get_contents (path, &cmd, NULL, NULL))
    return G_SOURCE_CONTINUE;
  g_strstrip (cmd);
  GtkWidget *grid = first_mapped_grid ();
  g_message ("debug cmd: %s", cmd);
  if (g_str_equal (cmd, "fp-state"))
    {
      debug_state ();
      return G_SOURCE_CONTINUE;
    }
  if (grid == NULL && (g_str_has_prefix (cmd, "scroll ") || g_str_has_prefix (cmd, "action ")))
    g_message ("debug cmd: no grid");
  if (g_str_has_prefix (cmd, "scroll ") && grid != NULL)
    {
      GtkAdjustment *adj = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (grid)); /* borrowed */
      double f = g_ascii_strtod (cmd + 7, NULL);
      gtk_adjustment_set_value (adj, gtk_adjustment_get_lower (adj) +
                                     f * (gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj)));
    }
  else if (g_str_has_prefix (cmd, "action ") && grid != NULL)
    gtk_widget_activate_action (grid, cmd + 7, NULL);
  else if (g_str_equal (cmd, "verify"))
    debug_verify ();
  else if (g_str_equal (cmd, "png"))
    debug_save_png ();
  else if (g_str_equal (cmd, "hover"))
    debug_hover (TRUE);
  else if (g_str_equal (cmd, "leave"))
    debug_hover (FALSE);
  else if (g_str_equal (cmd, "latency"))
    debug_latency ();
  else if (g_str_equal (cmd, "stats"))
    debug_stats ();
  else if (g_str_equal (cmd, "stats-reset"))
    debug_stats_reset ();
  else if (g_str_equal (cmd, "dump"))
    {
      GHashTableIter it;
      gpointer k;
      g_hash_table_iter_init (&it, vh->hooks);
      while (g_hash_table_iter_next (&it, &k, NULL))
        {
          ImageHook *h = k;
          if (!gtk_widget_get_mapped (h->image))
            continue;
          GtkWidget *cell = gtk_widget_get_ancestor (h->image, vh->view_cell_type);
          GtkWidget *hgrid = gtk_widget_get_ancestor (h->image, GTK_TYPE_GRID_VIEW);
          g_autoptr (NautilusFileInfo) f = cell_file (h->image);
          g_autofree char *name = f != NULL ? nautilus_file_info_get_name (f) : NULL;
          g_autofree char *icon_uri = icon_file_uri (h->icon);
          g_message ("debug dump: img=%p parent=%s cell=%p grid=%p %dx%d size=%d file=%s frame=%p n_frames=%u watched=%d uri=%s icon=%s:%s plain=%d",
                     (void *) h->image, G_OBJECT_TYPE_NAME (gtk_widget_get_parent (h->image)), (void *) cell,
                     (void *) hgrid, gtk_widget_get_width (h->image), gtk_widget_get_height (h->image), h->size,
                     name, h->paintable != NULL ? (void *) fp_folder_paintable_get_frame (h->paintable) : NULL,
                     h->n_frames, h->monitor != NULL, h->uri != NULL ? strrchr (h->uri, '/') : "(null)",
                     h->icon != NULL ? G_OBJECT_TYPE_NAME (h->icon) : "none", icon_uri != NULL ? icon_uri : "-",
                     is_plain_folder_icon (h->icon));
        }
    }
  else if (g_str_equal (cmd, "state") && vh->hover != NULL)
    {
      ImageHook *h = vh->hover;
      g_autoptr (GdkTexture) f0 = NULL;
      g_autoptr (GdkTexture) f1 = NULL;
      guint px = current_px (h);
      fp_cache_peek_frame (vh->cache, h->uri, 0, px, &f0, NULL);
      fp_cache_peek_frame (vh->cache, h->uri, 1, px, &f1, NULL);
      GdkTexture *shown = h->paintable != NULL ? fp_folder_paintable_get_frame (h->paintable) : NULL;
      g_message ("debug state: hover shown_frame=%u n_frames=%u paintable-frame=%s",
                 h->shown_frame, h->n_frames,
                 shown == NULL ? "none" : shown == f0 ? "frame0" : shown == f1 ? "frame1" : "other");
    }
  return G_SOURCE_CONTINUE;
}
#endif

/* ------------------------------------------------------------------------------------------------ */
/* Public API                                                                                       */

void
fp_view_hook_init (FpCache *cache)
{
  g_return_if_fail (cache != NULL);
  g_return_if_fail (vh == NULL);

  vh = g_new0 (ViewHook, 1);
  vh->cache = fp_cache_ref (cache);
  vh->hooks = g_hash_table_new (g_direct_hash, g_direct_equal);
  vh->style = *fp_cache_get_style (cache);
  vh->position = FP_STACK_CENTRE;
  vh->cycle = TRUE;
  vh->theme_bodies = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  /* Make sure GtkWidget's signals exist, then watch every map. */
  g_autoptr (GTypeClass) widget_class = g_type_class_ref (GTK_TYPE_WIDGET);
  vh->map_signal = g_signal_lookup ("map", GTK_TYPE_WIDGET);
  if (vh->map_signal == 0)
    {
      disable ("GtkWidget::map not found");
      return;
    }
  vh->map_hook_id = g_signal_add_emission_hook (vh->map_signal, 0, on_widget_map, NULL, NULL);

#ifdef FP_DEBUG_DRIVER
  debug_signal_id = g_unix_signal_add (SIGUSR2, on_debug_signal, NULL);
  latencies = g_array_new (FALSE, FALSE, sizeof (gint64));
  if (g_getenv ("FP_DEBUG_PROBE") != NULL)
    {
      probe_expected_us = g_get_monotonic_time () + 16000;
      probe_id = g_timeout_add (16, probe_tick, NULL);
      if (g_getenv ("FP_DEBUG_STALLS") != NULL)
        stall_detector_start (g_getenv ("FP_DEBUG_STALLS"));
    }
  g_message ("debug driver enabled (SIGUSR2 + $FP_DEBUG_DIR/cmd)");
#endif
}

void
fp_view_hook_shutdown (void)
{
  if (vh == NULL)
    return;
#ifdef FP_DEBUG_DRIVER
  g_clear_handle_id (&debug_signal_id, g_source_remove);
  stall_detector_stop ();
  g_clear_handle_id (&probe_id, g_source_remove);
  g_clear_pointer (&latencies, g_array_unref);
#endif
  if (vh->map_hook_id != 0)
    g_signal_remove_emission_hook (vh->map_signal, vh->map_hook_id);
  vh->map_hook_id = 0;
  g_clear_handle_id (&vh->detect_timer, g_source_remove);
  g_clear_handle_id (&vh->disable_idle, g_source_remove);
  detach_all ();
  g_clear_pointer (&vh->hooks, g_hash_table_unref);
  g_clear_pointer (&vh->cache, fp_cache_unref);
  g_clear_pointer (&vh->back_art, fp_icon_art_free);
  g_clear_pointer (&vh->overlay_art, fp_icon_art_free);
  g_clear_pointer (&vh->back_path, g_free);
  g_clear_pointer (&vh->overlay_path, g_free);
  g_clear_pointer (&vh->theme_bodies, g_hash_table_unref);
  g_clear_pointer (&vh, g_free);
}

/* Hooks every mapped NautilusImage below @w that isn't hooked or known to be irrelevant yet (used when the
 * feature is switched on while windows are open: their icons won't be mapped again). */
static void
attach_mapped (GtkWidget *w)
{
  if (!gtk_widget_get_mapped (w))
    return;
  if (G_TYPE_CHECK_INSTANCE_TYPE (w, vh->image_type))
    {
      if (g_object_get_data (G_OBJECT (w), HOOK_KEY) == NULL && g_object_get_data (G_OBJECT (w), IGNORE_KEY) == NULL)
        maybe_attach (w);
      return;
    }
  for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL && vh != NULL && !vh->disabled;
       c = gtk_widget_get_next_sibling (c))
    attach_mapped (c);
}

void
fp_view_hook_set_enabled (gboolean enabled)
{
  if (vh == NULL || vh->disabled || vh->off == !enabled)
    return;
  vh->off = !enabled;
  if (vh->off)
    {
      g_clear_handle_id (&vh->detect_timer, g_source_remove);
      detach_all (); /* gives every icon back to Nautilus */
      g_debug ("folder previews switched off");
      return;
    }
  g_debug ("folder previews switched on");
  if (vh->image_type == 0)
    vh->image_type = g_type_from_name (internal_name ("NautilusImage", "types"));
  if (vh->image_type == 0)
    return; /* no Nautilus icon was ever created: the map hook will find them */
  GListModel *toplevels = gtk_window_get_toplevels (); /* borrowed */
  for (guint i = 0; i < g_list_model_get_n_items (toplevels) && vh != NULL && !vh->disabled; i++)
    {
      g_autoptr (GtkWidget) window = g_list_model_get_item (toplevels, i);
      attach_mapped (window);
    }
}

/* (Re)loads *art from @path if the path changed ("" or NULL = none). Returns TRUE if *art changed. */
static gboolean
reload_art (FpIconArt **art, char **loaded_path, const char *path)
{
  if (path == NULL)
    path = "";
  if (*loaded_path != NULL && g_str_equal (*loaded_path, path))
    return FALSE;
  gboolean had = *art != NULL;
  g_clear_pointer (art, fp_icon_art_free);
  g_free (*loaded_path);
  *loaded_path = g_strdup (path);
  if (*path != '\0')
    {
      g_autoptr (GError) error = NULL;
      *art = fp_icon_art_new_from_file (path, &error);
      if (*art == NULL)
        g_debug ("custom folder icon not used: %s", error->message); /* = not set (§11.2 rule 3) */
    }
  return had || *art != NULL;
}

void
fp_view_hook_set_look (const FpStyle   *style,
                       FpStackPosition  position,
                       int              offset,
                       gboolean         cycle,
                       const char      *folder_icon,
                       const char      *overlay_icon)
{
  g_return_if_fail (style != NULL);
  if (vh == NULL || vh->disabled)
    return;
  gboolean style_changed = fp_cache_set_style (vh->cache, style); /* forgets frames of the old look */
  gboolean layout_changed = vh->style.layout != style->layout;
  vh->style = *style;
  gboolean cycle_changed = vh->cycle != cycle;
  vh->cycle = cycle;
  gboolean placement_changed = vh->position != position || vh->offset != offset;
  vh->position = position;
  vh->offset = offset;
  gboolean icons_changed = reload_art (&vh->back_art, &vh->back_path, folder_icon);
  icons_changed |= reload_art (&vh->overlay_art, &vh->overlay_path, overlay_icon);
  if (!style_changed && !layout_changed && !cycle_changed && !icons_changed && !placement_changed)
    return;
  g_debug ("look: %s, cycle %d, folder icon %s, overlay %s", style->layout == FP_LAYOUT_STACKED ? "stacked" : "grid",
           cycle, vh->back_art != NULL ? "custom" : "theme", vh->overlay_art != NULL ? "set" : "none");

  hover_stop ();
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init (&iter, vh->hooks);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      ImageHook *hook = key;
      if (style_changed)
        {
          /* Frames of the old look are gone: start over like a new folder (keeps uri/location). */
          cancel_and_clear (&hook->request);
          cancel_and_clear (&hook->hover_request);
          hook->generation++;
          hook->n_frames = 0;
          hook->shown_frame = 0;
          hook->known_empty = FALSE;
          if (hook->paintable != NULL)
            fp_folder_paintable_set_frame (hook->paintable, NULL, NULL);
        }
      schedule_sync (hook);
    }
}

gboolean
fp_view_hook_get_enabled (void)
{
  return vh != NULL && !vh->disabled && !vh->off;
}

static void folder_changed_impl (const char *uri);

void
fp_view_hook_folder_changed (const char *uri)
{
  TIME_BEGIN ();
  folder_changed_impl (uri);
  TIME_END (T_FOLDER_CHANGED);
}

static void
folder_changed_impl (const char *uri)
{
  if (vh == NULL || vh->disabled || vh->off || uri == NULL)
    return;
  /* Nautilus calls this for every folder when a directory loads; only act if we had cached something. */
  if (!fp_cache_invalidate (vh->cache, uri))
    return;
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init (&iter, vh->hooks);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      ImageHook *hook = key;
      if (g_strcmp0 (hook->uri, uri) == 0)
        schedule_sync (hook);
    }
}
