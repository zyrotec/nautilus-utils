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

/* Which groups of settings changed in one batch (bit flags for listeners). */
typedef enum
{
  NU_CHANGED_FOLDER_PREVIEW = 1 << 0, /* folder-preview-enabled */
  NU_CHANGED_SIDEBAR_DRIVES = 1 << 1, /* sidebar-drives-enabled */
  NU_CHANGED_BAR_COLOUR     = 1 << 2, /* bar-colour-mode, bar-colour, bar-gradient-* */
  NU_CHANGED_ROW_CONTENT    = 1 << 3, /* show-usage-bars, show-size-labels */
  NU_CHANGED_EJECT          = 1 << 4, /* eject-slide */
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

/* The built-in defaults (static, never freed). */
const NuConfig *nu_settings_defaults (void);

G_END_DECLS
