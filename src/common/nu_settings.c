/* Shared settings service (PLAN.md §9.2.3, §9.3). See nu_settings.h. */
#include "nu_settings.h"

#include <string.h>

/* Must match data/io.github.zyrotec.NautilusUtils.gschema.xml (checked by tests/common/test-settings.c). */
static const NuConfig defaults = {
  .folder_preview_enabled = TRUE,
  .sidebar_drives_enabled = TRUE,
  .bar_colour_mode = NU_BAR_COLOUR_ACCENT,
  .bar_colour = { 0x35, 0x84, 0xe4 },
  .gradient_start = { 0x35, 0x84, 0xe4 },
  .gradient_end = { 0x91, 0x41, 0xac },
  .show_usage_bars = TRUE,
  .show_size_labels = TRUE,
  .eject_slide = FALSE,
  .folder_views_enabled = TRUE,
  .folder_views_generation = 0,
  .folder_views_default = { NU_VIEW_GRID, 2 /* medium */, 1 /* medium */ },
  .folder_views_defaults_captured = FALSE,
  .preview_layout = NU_PREVIEW_GRID,
  .preview_cycle = TRUE,
  .preview_stack_step = NU_STACK_STEP_ONE,
  .preview_border = TRUE,
  .preview_border_colour = { 0xff, 0xff, 0xff },
  .preview_shadow = TRUE,
  .preview_rotation = TRUE,
  .preview_keep_aspect = TRUE,
  .preview_depth_effect = NU_DEPTH_BLUR,
  .preview_corner_radius = 2,
  .preview_fade = 0,
  .preview_view_top_radius = 0,
  .preview_view_bottom_radius = 0,
  .preview_stack_padding = 8,
  .preview_stack_position = NU_STACK_CENTRE,
  .preview_stack_offset = 0,
  .preview_folder_icon = "",
  .preview_overlay_icon = "",
};

/* Key name → group reported to listeners. */
static const struct
{
  const char *key;
  NuChanged   group;
} key_groups[] = {
  { "folder-preview-enabled", NU_CHANGED_FOLDER_PREVIEW },
  { "sidebar-drives-enabled", NU_CHANGED_SIDEBAR_DRIVES },
  { "bar-colour-mode", NU_CHANGED_BAR_COLOUR },
  { "bar-colour", NU_CHANGED_BAR_COLOUR },
  { "bar-gradient-start", NU_CHANGED_BAR_COLOUR },
  { "bar-gradient-end", NU_CHANGED_BAR_COLOUR },
  { "show-usage-bars", NU_CHANGED_ROW_CONTENT },
  { "show-size-labels", NU_CHANGED_ROW_CONTENT },
  { "eject-slide", NU_CHANGED_EJECT },
  { "folder-views-enabled", NU_CHANGED_FOLDER_VIEWS },
  { "folder-views-generation", NU_CHANGED_FV_GENERATION },
  { "folder-views-default-view", NU_CHANGED_FV_DEFAULT },
  { "folder-views-default-grid-zoom", NU_CHANGED_FV_DEFAULT },
  { "folder-views-default-list-zoom", NU_CHANGED_FV_DEFAULT },
  { "folder-views-defaults-captured", NU_CHANGED_FV_DEFAULT },
  { "folder-preview-layout", NU_CHANGED_PREVIEW_STYLE },
  { "folder-preview-stack-step", NU_CHANGED_PREVIEW_STYLE },
  { "folder-preview-border", NU_CHANGED_PREVIEW_STYLE },
  { "folder-preview-border-colour", NU_CHANGED_PREVIEW_STYLE },
  { "folder-preview-shadow", NU_CHANGED_PREVIEW_STYLE },
  { "folder-preview-rotation", NU_CHANGED_PREVIEW_STYLE },
  { "folder-preview-keep-aspect", NU_CHANGED_PREVIEW_STYLE },
  { "folder-preview-depth-effect", NU_CHANGED_PREVIEW_STYLE },
  { "folder-preview-corner-radius", NU_CHANGED_PREVIEW_STYLE },
  { "folder-preview-fade", NU_CHANGED_PREVIEW_STYLE },
  { "folder-preview-view-top-radius", NU_CHANGED_PREVIEW_STYLE },
  { "folder-preview-view-bottom-radius", NU_CHANGED_PREVIEW_STYLE },
  { "folder-preview-stack-padding", NU_CHANGED_PREVIEW_STYLE },
  { "folder-preview-stack-position", NU_CHANGED_PREVIEW_ICONS },
  { "folder-preview-stack-offset", NU_CHANGED_PREVIEW_ICONS },
  { "folder-preview-cycle", NU_CHANGED_PREVIEW_ICONS },
  { "folder-preview-folder-icon", NU_CHANGED_PREVIEW_ICONS },
  { "folder-preview-overlay-icon", NU_CHANGED_PREVIEW_ICONS },
};

typedef struct
{
  guint              id;
  NuSettingsListener fn;
  gpointer           data;
} Listener;

typedef struct
{
  GSettingsSchema  *schema;     /* Owned ref; NULL when the schema isn't installed */
  GSettingsBackend *backend;    /* Owned ref or NULL (= default backend) */
  GSettings        *settings;   /* Owned ref; NULL without schema */
  gulong            changed_id; /* "changed" handler on @settings */
  NuConfig          config;     /* Current values */
  char             *folder_icon;  /* Owned: storage of config.preview_folder_icon (freed by read_all/shutdown) */
  char             *overlay_icon; /* Owned: storage of config.preview_overlay_icon */
  GArray           *listeners;  /* Owned; of Listener */
  guint             next_id;
  NuChanged         pending;    /* Groups changed since the last notification */
  guint             notify_id;  /* Idle source delivering @pending, or 0 */
} NuSettings;

static NuSettings *ns; /* Created by init, freed by shutdown (main thread only). */

gboolean
nu_rgb_parse (const char *text,
              NuRgb      *out)
{
  if (text == NULL || strlen (text) != 7 || text[0] != '#')
    return FALSE;
  guint8 v[3];
  for (int i = 0; i < 3; i++)
    {
      int hi = g_ascii_xdigit_value (text[1 + 2 * i]);
      int lo = g_ascii_xdigit_value (text[2 + 2 * i]);
      if (hi < 0 || lo < 0)
        return FALSE;
      v[i] = (guint8) (hi * 16 + lo);
    }
  out->r = v[0];
  out->g = v[1];
  out->b = v[2];
  return TRUE;
}

/* Returns a newly allocated string; caller frees with g_free. */
char *
nu_rgb_to_string (NuRgb rgb)
{
  return g_strdup_printf ("#%02x%02x%02x", rgb.r, rgb.g, rgb.b);
}

gboolean
nu_folder_view_valid (const NuFolderView *v)
{
  return v != NULL && (v->view == NU_VIEW_LIST || v->view == NU_VIEW_GRID) && v->grid_zoom <= NU_GRID_ZOOM_MAX &&
         v->list_zoom <= NU_LIST_ZOOM_MAX;
}

const NuConfig *
nu_settings_defaults (void)
{
  return &defaults;
}

/* Reads colour @key into @out; an unparsable value keeps @fallback (a hand-edited dconf value). */
static void
read_colour (GSettings  *settings,
             const char *key,
             NuRgb       fallback,
             NuRgb      *out)
{
  g_autofree char *text = g_settings_get_string (settings, key);
  if (!nu_rgb_parse (text, out))
    {
      g_debug ("settings: %s = '%s' is not #rrggbb; using the default", key, text);
      *out = fallback;
    }
}

static void
read_all (void)
{
  GSettings *s = ns->settings;
  NuConfig *c = &ns->config;
  c->folder_preview_enabled = g_settings_get_boolean (s, "folder-preview-enabled");
  c->sidebar_drives_enabled = g_settings_get_boolean (s, "sidebar-drives-enabled");
  int mode = g_settings_get_enum (s, "bar-colour-mode");
  c->bar_colour_mode = (mode >= NU_BAR_COLOUR_ACCENT && mode <= NU_BAR_COLOUR_GRADIENT)
                         ? (NuBarColourMode) mode : defaults.bar_colour_mode;
  read_colour (s, "bar-colour", defaults.bar_colour, &c->bar_colour);
  read_colour (s, "bar-gradient-start", defaults.gradient_start, &c->gradient_start);
  read_colour (s, "bar-gradient-end", defaults.gradient_end, &c->gradient_end);
  c->show_usage_bars = g_settings_get_boolean (s, "show-usage-bars");
  c->show_size_labels = g_settings_get_boolean (s, "show-size-labels");
  c->eject_slide = g_settings_get_boolean (s, "eject-slide");
  c->folder_views_enabled = g_settings_get_boolean (s, "folder-views-enabled");
  c->folder_views_generation = g_settings_get_uint (s, "folder-views-generation");
  /* The schema enums keep these in range; check anyway, a value outside is the built-in default. */
  NuFolderView d = {
    .view = (NuViewType) g_settings_get_enum (s, "folder-views-default-view"),
    .grid_zoom = (guint8) CLAMP (g_settings_get_enum (s, "folder-views-default-grid-zoom"), 0, 255),
    .list_zoom = (guint8) CLAMP (g_settings_get_enum (s, "folder-views-default-list-zoom"), 0, 255),
  };
  c->folder_views_default = nu_folder_view_valid (&d) ? d : defaults.folder_views_default;
  c->folder_views_defaults_captured = g_settings_get_boolean (s, "folder-views-defaults-captured");

  int layout = g_settings_get_enum (s, "folder-preview-layout");
  c->preview_layout = (layout == NU_PREVIEW_GRID || layout == NU_PREVIEW_STACKED)
                        ? (NuPreviewLayout) layout : defaults.preview_layout;
  int step = g_settings_get_enum (s, "folder-preview-stack-step");
  c->preview_stack_step = (step == NU_STACK_STEP_ONE || step == NU_STACK_STEP_THREE)
                            ? (NuStackStep) step : defaults.preview_stack_step;
  c->preview_cycle = g_settings_get_boolean (s, "folder-preview-cycle");
  c->preview_border = g_settings_get_boolean (s, "folder-preview-border");
  read_colour (s, "folder-preview-border-colour", defaults.preview_border_colour, &c->preview_border_colour);
  c->preview_shadow = g_settings_get_boolean (s, "folder-preview-shadow");
  c->preview_rotation = g_settings_get_boolean (s, "folder-preview-rotation");
  c->preview_keep_aspect = g_settings_get_boolean (s, "folder-preview-keep-aspect");
  int depth = g_settings_get_enum (s, "folder-preview-depth-effect");
  c->preview_depth_effect = (depth >= NU_DEPTH_NONE && depth <= NU_DEPTH_DARKEN)
                              ? (NuDepthEffect) depth : defaults.preview_depth_effect;
  int position = g_settings_get_enum (s, "folder-preview-stack-position");
  c->preview_stack_position = (position >= NU_STACK_TOP && position <= NU_STACK_BOTTOM)
                                ? (NuStackPosition) position : defaults.preview_stack_position;
  /* The schema ranges keep these in range (a hand-edited value outside is refused by GSettings). */
  c->preview_corner_radius = g_settings_get_int (s, "folder-preview-corner-radius");
  c->preview_fade = g_settings_get_int (s, "folder-preview-fade");
  c->preview_view_top_radius = g_settings_get_int (s, "folder-preview-view-top-radius");
  c->preview_view_bottom_radius = g_settings_get_int (s, "folder-preview-view-bottom-radius");
  c->preview_stack_offset = g_settings_get_int (s, "folder-preview-stack-offset");
  c->preview_stack_padding = g_settings_get_int (s, "folder-preview-stack-padding");
  /* Replace the owned strings; the config borrows them. */
  g_free (ns->folder_icon);
  ns->folder_icon = g_settings_get_string (s, "folder-preview-folder-icon");
  c->preview_folder_icon = ns->folder_icon;
  g_free (ns->overlay_icon);
  ns->overlay_icon = g_settings_get_string (s, "folder-preview-overlay-icon");
  c->preview_overlay_icon = ns->overlay_icon;
}

static gboolean
notify_idle (gpointer data)
{
  (void) data;
  ns->notify_id = 0;
  NuChanged changed = ns->pending;
  ns->pending = 0;
  /* Listeners may add/remove listeners: iterate over a copy and skip ids that were removed meanwhile. */
  g_autoptr (GArray) snapshot = g_array_copy (ns->listeners);
  for (guint i = 0; i < snapshot->len; i++)
    {
      Listener *l = &g_array_index (snapshot, Listener, i);
      gboolean still_there = FALSE;
      for (guint j = 0; ns != NULL && j < ns->listeners->len; j++)
        if (g_array_index (ns->listeners, Listener, j).id == l->id)
          still_there = TRUE;
      if (still_there)
        l->fn (changed, l->data);
      if (ns == NULL) /* a listener shut the service down */
        break;
    }
  return G_SOURCE_REMOVE;
}

static void
on_changed (GSettings  *settings,
            const char *key,
            gpointer    data)
{
  (void) settings;
  (void) data;
  NuChanged group = 0;
  for (gsize i = 0; i < G_N_ELEMENTS (key_groups); i++)
    if (g_strcmp0 (key, key_groups[i].key) == 0)
      group = key_groups[i].group;
  if (group == 0)
    return;
  read_all ();
  ns->pending |= group;
  if (ns->notify_id == 0)
    ns->notify_id = g_idle_add (notify_idle, NULL);
}

void
nu_settings_init (void)
{
  nu_settings_init_full (NU_SETTINGS_SCHEMA_ID, NULL);
}

void
nu_settings_init_full (const char       *schema_id,
                       GSettingsBackend *backend)
{
  if (ns != NULL)
    return;
  ns = g_new0 (NuSettings, 1);
  ns->config = defaults;
  ns->listeners = g_array_new (FALSE, FALSE, sizeof (Listener));
  ns->next_id = 1;
  ns->backend = backend != NULL ? g_object_ref (backend) : NULL;

  GSettingsSchemaSource *source = g_settings_schema_source_get_default (); /* borrowed */
  ns->schema = source != NULL ? g_settings_schema_source_lookup (source, schema_id, TRUE) : NULL;
  if (ns->schema == NULL)
    {
      g_warning ("settings schema %s is not installed; using the defaults (see README: Settings)", schema_id);
      return;
    }
  ns->settings = g_settings_new_full (ns->schema, ns->backend, NULL);
  read_all ();
  ns->changed_id = g_signal_connect (ns->settings, "changed", G_CALLBACK (on_changed), NULL);
}

void
nu_settings_shutdown (void)
{
  if (ns == NULL)
    return;
  g_clear_handle_id (&ns->notify_id, g_source_remove);
  if (ns->settings != NULL)
    g_clear_signal_handler (&ns->changed_id, ns->settings);
  g_clear_object (&ns->settings);
  g_clear_pointer (&ns->schema, g_settings_schema_unref);
  g_clear_object (&ns->backend);
  g_clear_pointer (&ns->listeners, g_array_unref);
  g_clear_pointer (&ns->folder_icon, g_free);
  g_clear_pointer (&ns->overlay_icon, g_free);
  g_clear_pointer (&ns, g_free);
}

const NuConfig *
nu_settings_get (void)
{
  return ns != NULL ? &ns->config : &defaults;
}

gboolean
nu_settings_available (void)
{
  return ns != NULL && ns->settings != NULL;
}

/* Returns a new ref (caller unrefs) or NULL. */
GSettings *
nu_settings_new_delayed (void)
{
  if (!nu_settings_available ())
    return NULL;
  GSettings *s = g_settings_new_full (ns->schema, ns->backend, NULL);
  g_settings_delay (s);
  return s;
}

guint
nu_settings_add_listener (NuSettingsListener listener,
                          gpointer           user_data)
{
  g_return_val_if_fail (ns != NULL && listener != NULL, 0);
  Listener l = { ns->next_id++, listener, user_data };
  g_array_append_val (ns->listeners, l);
  return l.id;
}

void
nu_settings_remove_listener (guint id)
{
  if (ns == NULL || id == 0)
    return;
  for (guint i = 0; i < ns->listeners->len; i++)
    if (g_array_index (ns->listeners, Listener, i).id == id)
      {
        g_array_remove_index (ns->listeners, i);
        return;
      }
}

gboolean
nu_settings_capture_folder_view_default (const NuFolderView *files_default)
{
  if (!nu_settings_available () || g_settings_get_boolean (ns->settings, "folder-views-defaults-captured"))
    return FALSE;
  const NuFolderView *d = nu_folder_view_valid (files_default) ? files_default : &defaults.folder_views_default;
  /* One batch through a delayed copy (delay mode can't be undone, so not on ns->settings itself). */
  g_autoptr (GSettings) batch = nu_settings_new_delayed ();
  g_settings_set_enum (batch, "folder-views-default-view", d->view);
  g_settings_set_enum (batch, "folder-views-default-grid-zoom", d->grid_zoom);
  g_settings_set_enum (batch, "folder-views-default-list-zoom", d->list_zoom);
  g_settings_set_boolean (batch, "folder-views-defaults-captured", TRUE);
  g_settings_apply (batch);
  return TRUE;
}

gboolean
nu_settings_reset_folder_views (void)
{
  if (!nu_settings_available ())
    return FALSE;
  /* Wraps to 0 after 2^32 resets; the old generation-0 values would then count again (accepted). */
  guint next = g_settings_get_uint (ns->settings, "folder-views-generation") + 1;
  g_settings_set_uint (ns->settings, "folder-views-generation", next);
  return TRUE;
}
