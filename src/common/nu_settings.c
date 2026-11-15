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
  .preview_border_width = 1.0,
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
  .folder_icons_enabled = TRUE,
  .icon_rules = NULL,
  .n_icon_rules = 0,
  .icon_rule_history = (const char *const[]) { NULL },
  .details_pane_enabled = TRUE,
  .details_pane_visible = FALSE,
  .details_pane_width = 320,
  .details_pane_height = 112,
  .details_pane_position = NU_PANE_RIGHT,
  .details_pane_border = TRUE,
  .details_pane_margin = 0,
  .details_pane_padding = 18,
  .details_pane_radius = 0,
  .details_pane_header_border = FALSE,
  .details_pane_preview_radius = 12,
  .details_pane_preview_margin = 0,
  .details_pane_icon_size = 128,
  .details_pane_media_fit = NU_MEDIA_FIT_SHAPE,
  .details_pane_title_centered = FALSE,
  .details_pane_show_icon = TRUE,
  .details_pane_preview_backdrop = FALSE,
  .details_pane_stack_depth = NU_DEPTH_BLUR,
  .details_pane_stack_shadow = TRUE,
  .details_pane_stack_rotation = TRUE,
  .details_pane_stack_keep_aspect = TRUE,
  .details_pane_stack_fade = 0,
  .details_pane_stack_card = TRUE,
  .details_pane_stack_border = FALSE,
  .details_pane_stack_border_colour = { 0xff, 0xff, 0xff },
  .details_pane_stack_border_width = 2.0,
  .details_pane_stack_corner_radius = 4,
  .details_pane_stack_media_square = FALSE,
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
  { "folder-preview-border-width", NU_CHANGED_PREVIEW_STYLE },
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
  { "folder-icons-enabled", NU_CHANGED_FOLDER_ICONS },
  { "folder-icon-rules", NU_CHANGED_FOLDER_ICONS },
  { "folder-icon-rule-history", NU_CHANGED_FOLDER_ICONS },
  { "folder-preview-folder-icon", NU_CHANGED_PREVIEW_ICONS },
  { "folder-preview-overlay-icon", NU_CHANGED_PREVIEW_ICONS },
  { "details-pane-enabled", NU_CHANGED_DETAILS_PANE },
  { "details-pane-visible", NU_CHANGED_DP_STATE },
  { "details-pane-width", NU_CHANGED_DP_STATE },
  { "details-pane-height", NU_CHANGED_DP_STATE },
  { "details-pane-position", NU_CHANGED_DP_LOOK },
  { "details-pane-border", NU_CHANGED_DP_LOOK },
  { "details-pane-margin", NU_CHANGED_DP_LOOK },
  { "details-pane-padding", NU_CHANGED_DP_LOOK },
  { "details-pane-radius", NU_CHANGED_DP_LOOK },
  { "details-pane-header-border", NU_CHANGED_DP_LOOK },
  { "details-pane-preview-radius", NU_CHANGED_DP_LOOK },
  { "details-pane-preview-margin", NU_CHANGED_DP_LOOK },
  { "details-pane-stack-depth-effect", NU_CHANGED_DP_LOOK },
  { "details-pane-icon-size", NU_CHANGED_DP_LOOK },
  { "details-pane-media-fit", NU_CHANGED_DP_LOOK },
  { "details-pane-title-centered", NU_CHANGED_DP_LOOK },
  { "details-pane-show-icon", NU_CHANGED_DP_LOOK },
  { "details-pane-preview-backdrop", NU_CHANGED_DP_LOOK },
  { "details-pane-stack-shadow", NU_CHANGED_DP_LOOK },
  { "details-pane-stack-rotation", NU_CHANGED_DP_LOOK },
  { "details-pane-stack-keep-aspect", NU_CHANGED_DP_LOOK },
  { "details-pane-stack-fade", NU_CHANGED_DP_LOOK },
  { "details-pane-stack-card", NU_CHANGED_DP_LOOK },
  { "details-pane-stack-border", NU_CHANGED_DP_LOOK },
  { "details-pane-stack-border-colour", NU_CHANGED_DP_LOOK },
  { "details-pane-stack-border-width", NU_CHANGED_DP_LOOK },
  { "details-pane-stack-corner-radius", NU_CHANGED_DP_LOOK },
  { "details-pane-stack-media-square", NU_CHANGED_DP_LOOK },
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
  GVariant         *rules_value;  /* Owned: folder-icon-rules; config.icon_rules points into its strings */
  NuIconRule       *rules;        /* Owned array (strings borrowed from @rules_value) = config.icon_rules */
  char            **history;      /* Owned strv = config.icon_rule_history */
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
  c->preview_border_width = g_settings_get_double (s, "folder-preview-border-width");
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

  c->folder_icons_enabled = g_settings_get_boolean (s, "folder-icons-enabled");
  /* Replace the owned rule storage; the config borrows it. */
  g_clear_pointer (&ns->rules, g_free);
  g_clear_pointer (&ns->rules_value, g_variant_unref);
  ns->rules_value = g_settings_get_value (s, "folder-icon-rules");
  gsize n = g_variant_n_children (ns->rules_value);
  ns->rules = n > 0 ? g_new0 (NuIconRule, n) : NULL;
  for (gsize i = 0; i < n; i++)
    g_variant_get_child (ns->rules_value, i, "(&s&s)", &ns->rules[i].name, &ns->rules[i].icon);
  c->icon_rules = ns->rules;
  c->n_icon_rules = (guint) n;
  g_strfreev (ns->history);
  ns->history = g_settings_get_strv (s, "folder-icon-rule-history");
  c->icon_rule_history = (const char *const *) ns->history;

  c->details_pane_enabled = g_settings_get_boolean (s, "details-pane-enabled");
  c->details_pane_visible = g_settings_get_boolean (s, "details-pane-visible");
  c->details_pane_width = CLAMP (g_settings_get_int (s, "details-pane-width"), NU_DETAILS_PANE_WIDTH_MIN,
                                 NU_DETAILS_PANE_WIDTH_MAX);
  c->details_pane_height = CLAMP (g_settings_get_int (s, "details-pane-height"), NU_DETAILS_PANE_HEIGHT_MIN,
                                  NU_DETAILS_PANE_HEIGHT_MAX);
  int pane_position = g_settings_get_enum (s, "details-pane-position");
  c->details_pane_position = pane_position == NU_PANE_BOTTOM ? NU_PANE_BOTTOM : NU_PANE_RIGHT;
  c->details_pane_border = g_settings_get_boolean (s, "details-pane-border");
  /* The schema ranges keep the integers in range. */
  c->details_pane_margin = g_settings_get_int (s, "details-pane-margin");
  c->details_pane_padding = g_settings_get_int (s, "details-pane-padding");
  c->details_pane_radius = g_settings_get_int (s, "details-pane-radius");
  c->details_pane_header_border = g_settings_get_boolean (s, "details-pane-header-border");
  c->details_pane_preview_radius = g_settings_get_int (s, "details-pane-preview-radius");
  c->details_pane_preview_margin = g_settings_get_int (s, "details-pane-preview-margin");
  /* Steps of 16 (a hand-edited value in between is rounded to the nearest step). */
  c->details_pane_media_fit = g_settings_get_enum (s, "details-pane-media-fit") == NU_MEDIA_FIT_BOX
                                ? NU_MEDIA_FIT_BOX : NU_MEDIA_FIT_SHAPE;
  c->details_pane_title_centered = g_settings_get_boolean (s, "details-pane-title-centered");
  c->details_pane_show_icon = g_settings_get_boolean (s, "details-pane-show-icon");
  c->details_pane_preview_backdrop = g_settings_get_boolean (s, "details-pane-preview-backdrop");
  c->details_pane_icon_size = CLAMP ((g_settings_get_int (s, "details-pane-icon-size") + 8) / 16 * 16, 16, 256);
  int sdepth = g_settings_get_enum (s, "details-pane-stack-depth-effect");
  c->details_pane_stack_depth = (sdepth >= NU_DEPTH_NONE && sdepth <= NU_DEPTH_DARKEN)
                                  ? (NuDepthEffect) sdepth : defaults.details_pane_stack_depth;
  c->details_pane_stack_shadow = g_settings_get_boolean (s, "details-pane-stack-shadow");
  c->details_pane_stack_rotation = g_settings_get_boolean (s, "details-pane-stack-rotation");
  c->details_pane_stack_keep_aspect = g_settings_get_boolean (s, "details-pane-stack-keep-aspect");
  c->details_pane_stack_fade = g_settings_get_int (s, "details-pane-stack-fade");
  c->details_pane_stack_card = g_settings_get_boolean (s, "details-pane-stack-card");
  c->details_pane_stack_border = g_settings_get_boolean (s, "details-pane-stack-border");
  read_colour (s, "details-pane-stack-border-colour", defaults.details_pane_stack_border_colour,
               &c->details_pane_stack_border_colour);
  c->details_pane_stack_border_width = CLAMP (g_settings_get_double (s, "details-pane-stack-border-width"), 0.5, 8.0);
  c->details_pane_stack_corner_radius = CLAMP (g_settings_get_int (s, "details-pane-stack-corner-radius"), 0, 32);
  c->details_pane_stack_media_square = g_settings_get_boolean (s, "details-pane-stack-media-square");
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
  /* An older installed schema (module updated, schema not): reading a missing key would abort Files, so behave
   * as if the schema were missing. */
  for (gsize i = 0; i < G_N_ELEMENTS (key_groups); i++)
    if (!g_settings_schema_has_key (ns->schema, key_groups[i].key))
      {
        g_warning ("settings schema %s is out of date (no key \"%s\"); using the defaults until it is "
                   "reinstalled (see README: Settings)", schema_id, key_groups[i].key);
        g_clear_pointer (&ns->schema, g_settings_schema_unref);
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
  g_clear_pointer (&ns->rules, g_free);
  g_clear_pointer (&ns->rules_value, g_variant_unref);
  g_clear_pointer (&ns->history, g_strfreev);
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
nu_settings_remember_icon_rule_names (const char *const *names)
{
  if (!nu_settings_available () || names == NULL)
    return FALSE;
  g_auto (GStrv) old = g_settings_get_strv (ns->settings, "folder-icon-rule-history");
  g_autoptr (GPtrArray) next = g_ptr_array_new (); /* borrowed strings from @old / @names */
  gboolean added = FALSE;
  for (gsize i = 0; old[i] != NULL; i++)
    g_ptr_array_add (next, old[i]);
  for (gsize i = 0; names[i] != NULL; i++)
    {
      gboolean known = *names[i] == '\0';
      for (guint k = 0; k < next->len && !known; k++)
        known = g_str_equal (g_ptr_array_index (next, k), names[i]);
      if (known)
        continue;
      g_ptr_array_add (next, (gpointer) names[i]);
      added = TRUE;
    }
  if (!added)
    return FALSE;
  guint drop = next->len > NU_ICON_RULE_HISTORY_MAX ? next->len - NU_ICON_RULE_HISTORY_MAX : 0;
  g_ptr_array_add (next, NULL);
  g_settings_set_strv (ns->settings, "folder-icon-rule-history", (const char *const *) next->pdata + drop);
  return TRUE;
}

gboolean
nu_settings_set_details_pane_state (gboolean visible,
                                    int      width)
{
  if (!nu_settings_available ())
    return FALSE;
  width = CLAMP (width, NU_DETAILS_PANE_WIDTH_MIN, NU_DETAILS_PANE_WIDTH_MAX);
  if (g_settings_get_boolean (ns->settings, "details-pane-visible") != visible)
    g_settings_set_boolean (ns->settings, "details-pane-visible", visible);
  if (g_settings_get_int (ns->settings, "details-pane-width") != width)
    g_settings_set_int (ns->settings, "details-pane-width", width);
  return TRUE;
}

gboolean
nu_settings_set_details_pane_height (int height)
{
  if (!nu_settings_available ())
    return FALSE;
  height = CLAMP (height, NU_DETAILS_PANE_HEIGHT_MIN, NU_DETAILS_PANE_HEIGHT_MAX);
  if (g_settings_get_int (ns->settings, "details-pane-height") != height)
    g_settings_set_int (ns->settings, "details-pane-height", height);
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
