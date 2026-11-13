/* Shared settings for all features (PLAN.md §9.2.3, §9.3): typed snapshot of the GSettings schema
 * io.github.zyrotec.NautilusUtils plus one coalesced "changed" notification per batch of key changes
 * (the settings dialog applies many keys at once). If the schema isn't installed, one warning is logged
 * and the built-in defaults (= behaviour before settings existed) are used.
 * GLib/GIO only (PLAN.md §4.2); main thread only. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define NU_SETTINGS_SCHEMA_ID "io.github.zyrotec.NautilusUtils"

typedef enum
{
  NU_BAR_COLOUR_ACCENT   = 0,
  NU_BAR_COLOUR_CUSTOM   = 1,
  NU_BAR_COLOUR_GRADIENT = 2,
} NuBarColourMode;

typedef struct
{
  guint8 r, g, b;
} NuRgb;

/* Folder preview v2 (PLAN.md §11). Values equal the schema enums. */
typedef enum
{
  NU_PREVIEW_GRID    = 0,
  NU_PREVIEW_STACKED = 1,
} NuPreviewLayout;

typedef enum
{
  NU_DEPTH_NONE         = 0,
  NU_DEPTH_BLUR         = 1,
  NU_DEPTH_TRANSPARENCY = 2,
  NU_DEPTH_DARKEN       = 3,
} NuDepthEffect;

typedef enum
{
  NU_STACK_TOP    = 0,
  NU_STACK_CENTRE = 1,
  NU_STACK_BOTTOM = 2,
} NuStackPosition;

typedef enum
{
  NU_STACK_STEP_ONE   = 0,
  NU_STACK_STEP_THREE = 1,
} NuStackStep;

/* Folder views (PLAN.md §10). Values equal Nautilus' own view ids and zoom enums. */
typedef enum
{
  NU_VIEW_LIST = 1,
  NU_VIEW_GRID = 2,
} NuViewType;

#define NU_GRID_ZOOM_MAX 4 /* small, small-plus, medium, large, extra-large */
#define NU_LIST_ZOOM_MAX 2 /* small, medium, large */

/* One folder's view: its view type plus one icon size (zoom level) per view type. */
typedef struct
{
  NuViewType view;
  guint8     grid_zoom; /* 0 .. NU_GRID_ZOOM_MAX */
  guint8     list_zoom; /* 0 .. NU_LIST_ZOOM_MAX */
} NuFolderView;

/* Which groups of settings changed in one batch (bit flags for listeners). */
typedef enum
{
  NU_CHANGED_FOLDER_PREVIEW = 1 << 0, /* folder-preview-enabled */
  NU_CHANGED_SIDEBAR_DRIVES = 1 << 1, /* sidebar-drives-enabled */
  NU_CHANGED_BAR_COLOUR     = 1 << 2, /* bar-colour-mode, bar-colour, bar-gradient-* */
  NU_CHANGED_ROW_CONTENT    = 1 << 3, /* show-usage-bars, show-size-labels */
  NU_CHANGED_EJECT          = 1 << 4, /* eject-slide */
  NU_CHANGED_FOLDER_VIEWS   = 1 << 5, /* folder-views-enabled */
  NU_CHANGED_FV_GENERATION  = 1 << 6, /* folder-views-generation (Reset All) */
  NU_CHANGED_FV_DEFAULT     = 1 << 7, /* folder-views-default-*, folder-views-defaults-captured */
  NU_CHANGED_PREVIEW_STYLE  = 1 << 8, /* folder-preview-layout, -stack-step, -border*, -shadow, -rotation,
                                       * -keep-aspect, -depth-effect, -corner-radius, -fade, -view-*-radius,
                                       * -stack-padding, -border-width */
  NU_CHANGED_PREVIEW_ICONS  = 1 << 9, /* folder-preview-cycle, -folder-icon, -overlay-icon, -stack-position,
                                       * -stack-offset (nothing that changes the pictures themselves) */
} NuChanged;

typedef struct
{
  gboolean        folder_preview_enabled;
  gboolean        sidebar_drives_enabled;
  NuBarColourMode bar_colour_mode;
  NuRgb           bar_colour;
  NuRgb           gradient_start;
  NuRgb           gradient_end;
  gboolean        show_usage_bars;
  gboolean        show_size_labels;
  gboolean        eject_slide;
  gboolean        folder_views_enabled;
  guint           folder_views_generation;
  NuFolderView    folder_views_default;
  gboolean        folder_views_defaults_captured;
  NuPreviewLayout preview_layout;
  gboolean        preview_cycle;
  NuStackStep     preview_stack_step;
  gboolean        preview_border;
  NuRgb           preview_border_colour;
  double          preview_border_width;    /* px at 64 × 64, 0.5 … 6 */
  gboolean        preview_shadow;
  gboolean        preview_rotation;
  gboolean        preview_keep_aspect;
  NuDepthEffect   preview_depth_effect;
  int             preview_corner_radius;   /* px at 64 × 64 */
  int             preview_fade;            /* % of the stack height, 0 = off */
  int             preview_view_top_radius; /* px at 64 × 64 */
  int             preview_view_bottom_radius;
  int             preview_stack_padding;   /* px at 64 × 64 */
  NuStackPosition preview_stack_position;
  int             preview_stack_offset;    /* px at 64 × 64, positive = down */
  const char     *preview_folder_icon;  /* Never NULL ("" = theme icon); owned by the service, valid until
                                         * the next change notification or shutdown. */
  const char     *preview_overlay_icon; /* Never NULL ("" = none); same ownership. */
} NuConfig;

typedef void (*NuSettingsListener) (NuChanged changed, gpointer user_data);

/* Starts the settings service with the installed schema and the default backend (dconf).
 * Idempotent; pair with nu_settings_shutdown(). */
void nu_settings_init (void);

/* Same, with an explicit schema id and backend (NULL = default) — for tests. An unknown @schema_id behaves
 * like a missing schema (defaults + one warning). */
void nu_settings_init_full (const char       *schema_id,
                            GSettingsBackend *backend);

/* Disconnects everything, drops all listeners and pending notifications. Safe without init. */
void nu_settings_shutdown (void);

/* Current values. Borrowed pointer, valid until the next change notification or shutdown; without init
 * (or without schema) it points at the defaults. */
const NuConfig *nu_settings_get (void);

/* TRUE if the schema was found (settings can be changed and are persisted). */
gboolean nu_settings_available (void);

/* A new GSettings for the same schema/backend in delay-apply mode (for the settings dialog: changes stay
 * pending until g_settings_apply). Caller owns the returned ref; NULL if the schema isn't available. */
GSettings *nu_settings_new_delayed (void);

/* Calls @listener once per batch of changes (coalesced in an idle), with the changed groups.
 * Returns an id > 0 for nu_settings_remove_listener(). */
guint nu_settings_add_listener (NuSettingsListener listener,
                                gpointer           user_data);

/* Removes a listener; unknown or 0 ids are ignored. */
void nu_settings_remove_listener (guint id);

/* Parses "#rrggbb" (case-insensitive). Returns FALSE and leaves @out untouched on anything else. */
gboolean nu_rgb_parse (const char *text,
                       NuRgb      *out);

/* "#rrggbb" (lower case). Caller owns the returned string (g_free). */
char *nu_rgb_to_string (NuRgb rgb);

/* TRUE if @v holds a known view type and in-range zoom levels. */
gboolean nu_folder_view_valid (const NuFolderView *v);

/* One-time copy of Files' default into folder-views-default-* (PLAN.md §10.1.1): writes @files_default and
 * sets folder-views-defaults-captured, unless that is already set. An invalid @files_default is replaced by
 * the built-in default. Returns TRUE if it wrote; FALSE if already captured or without schema. */
gboolean nu_settings_capture_folder_view_default (const NuFolderView *files_default);

/* "Reset All Folder Views" (PLAN.md §10.1.3): increments folder-views-generation right away (not a pending
 * dialog setting). Returns FALSE without schema. */
gboolean nu_settings_reset_folder_views (void);

/* The built-in defaults (static, never freed). */
const NuConfig *nu_settings_defaults (void);

G_END_DECLS
