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
