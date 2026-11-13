/* Tests for src/common/nu_settings.c (PLAN.md §9.5 Phase 1). Each test uses its own in-memory backend. */
#include <gio/gio.h>

#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>

#include <string.h>

#include "nu_settings.h"

static gboolean
rgb_equal (NuRgb a, NuRgb b)
{
  return a.r == b.r && a.g == b.g && a.b == b.b;
}

/* Field by field: memcmp would also compare padding bytes. */
static void
assert_config_equal (const NuConfig *a, const NuConfig *b)
{
  g_assert_cmpint (a->folder_preview_enabled, ==, b->folder_preview_enabled);
  g_assert_cmpint (a->sidebar_drives_enabled, ==, b->sidebar_drives_enabled);
  g_assert_cmpint (a->bar_colour_mode, ==, b->bar_colour_mode);
  g_assert_true (rgb_equal (a->bar_colour, b->bar_colour));
  g_assert_true (rgb_equal (a->gradient_start, b->gradient_start));
  g_assert_true (rgb_equal (a->gradient_end, b->gradient_end));
  g_assert_cmpint (a->show_usage_bars, ==, b->show_usage_bars);
  g_assert_cmpint (a->show_size_labels, ==, b->show_size_labels);
  g_assert_cmpint (a->eject_slide, ==, b->eject_slide);
  g_assert_cmpint (a->folder_views_enabled, ==, b->folder_views_enabled);
  g_assert_cmpuint (a->folder_views_generation, ==, b->folder_views_generation);
  g_assert_cmpint (a->folder_views_default.view, ==, b->folder_views_default.view);
  g_assert_cmpuint (a->folder_views_default.grid_zoom, ==, b->folder_views_default.grid_zoom);
  g_assert_cmpuint (a->folder_views_default.list_zoom, ==, b->folder_views_default.list_zoom);
  g_assert_cmpint (a->folder_views_defaults_captured, ==, b->folder_views_defaults_captured);
  g_assert_cmpint (a->preview_layout, ==, b->preview_layout);
  g_assert_cmpint (a->preview_cycle, ==, b->preview_cycle);
  g_assert_cmpint (a->preview_stack_step, ==, b->preview_stack_step);
  g_assert_cmpint (a->preview_border, ==, b->preview_border);
  g_assert_true (rgb_equal (a->preview_border_colour, b->preview_border_colour));
  g_assert_cmpint (a->preview_shadow, ==, b->preview_shadow);
  g_assert_cmpint (a->preview_rotation, ==, b->preview_rotation);
  g_assert_cmpint (a->preview_keep_aspect, ==, b->preview_keep_aspect);
  g_assert_cmpint (a->preview_depth, ==, b->preview_depth);
  g_assert_cmpstr (a->preview_folder_icon, ==, b->preview_folder_icon);
  g_assert_cmpstr (a->preview_overlay_icon, ==, b->preview_overlay_icon);
}

/* Runs every pending main-context event (GSettings delivers backend changes through idles). */
static void
drain (void)
{
  for (int round = 0; round < 5; round++)
    while (g_main_context_iteration (NULL, FALSE))
      ;
}

typedef struct
{
  guint     calls;
  NuChanged last;
  NuChanged all;
} Counter;

static void
count_listener (NuChanged changed, gpointer data)
{
  Counter *c = data;
  c->calls++;
  c->last = changed;
  c->all |= changed;
}

/* Starts the service on a fresh memory backend; the caller owns the returned backend ref. */
static GSettingsBackend *
start (void)
{
  GSettingsBackend *backend = g_memory_settings_backend_new ();
  nu_settings_init_full (NU_SETTINGS_SCHEMA_ID, backend);
  g_assert_true (nu_settings_available ());
  return backend;
}

static GSettings *
direct_settings (GSettingsBackend *backend)
{
  return g_settings_new_with_backend (NU_SETTINGS_SCHEMA_ID, backend);
}

/* ---------------------------------------------------------------------------------------------- */

static void
test_rgb_parse (void)
{
  NuRgb c = { 1, 2, 3 };
  g_assert_true (nu_rgb_parse ("#3584e4", &c));
  g_assert_cmpuint (c.r, ==, 0x35);
  g_assert_cmpuint (c.g, ==, 0x84);
  g_assert_cmpuint (c.b, ==, 0xe4);
  g_assert_true (nu_rgb_parse ("#FFaa00", &c));
  g_assert_cmpuint (c.r, ==, 0xff);
  g_assert_cmpuint (c.g, ==, 0xaa);
  g_assert_cmpuint (c.b, ==, 0x00);

  const char *bad[] = { NULL, "", "#", "3584e4", "#3584e", "#3584e4f", "#35 4e4", "#xx84e4", "##584e4",
                        "#3584e\xc3", "rgb(1,2,3)" };
  for (gsize i = 0; i < G_N_ELEMENTS (bad); i++)
    {
      NuRgb keep = { 7, 8, 9 };
      g_assert_false (nu_rgb_parse (bad[i], &keep));
      g_assert_cmpuint (keep.r, ==, 7); /* untouched on failure */
      g_assert_cmpuint (keep.g, ==, 8);
      g_assert_cmpuint (keep.b, ==, 9);
    }

  /* Round trip over a spread of values. */
  for (guint v = 0; v < 256; v += 17)
    {
      NuRgb in = { (guint8) v, (guint8) (255 - v), (guint8) (v ^ 0x5a) }, out = { 0 };
      g_autofree char *s = nu_rgb_to_string (in);
      g_assert_cmpuint (strlen (s), ==, 7);
      g_assert_true (nu_rgb_parse (s, &out));
      g_assert_true (rgb_equal (in, out));
    }
}

/* The C defaults must equal the schema defaults, so "no schema" behaves exactly like a fresh install. */
static void
test_defaults_match_schema (void)
{
  g_autoptr (GSettingsBackend) backend = start ();
  const NuConfig *d = nu_settings_defaults ();
  const NuConfig *c = nu_settings_get ();
  assert_config_equal (d, c);
  nu_settings_shutdown ();
}

static void
test_reads_stored_values (void)
{
  g_autoptr (GSettingsBackend) backend = g_memory_settings_backend_new ();
  g_autoptr (GSettings) s = direct_settings (backend);
  g_settings_set_boolean (s, "folder-preview-enabled", FALSE);
  g_settings_set_enum (s, "bar-colour-mode", NU_BAR_COLOUR_GRADIENT);
  g_settings_set_string (s, "bar-gradient-end", "#00FF10");
  g_settings_set_boolean (s, "eject-slide", TRUE);
  g_settings_set_boolean (s, "show-size-labels", FALSE);

  nu_settings_init_full (NU_SETTINGS_SCHEMA_ID, backend);
  const NuConfig *c = nu_settings_get ();
  g_assert_false (c->folder_preview_enabled);
  g_assert_true (c->sidebar_drives_enabled);
  g_assert_cmpint (c->bar_colour_mode, ==, NU_BAR_COLOUR_GRADIENT);
  g_assert_cmpuint (c->gradient_end.r, ==, 0x00);
  g_assert_cmpuint (c->gradient_end.g, ==, 0xff);
  g_assert_cmpuint (c->gradient_end.b, ==, 0x10);
  g_assert_true (c->eject_slide);
  g_assert_true (c->show_usage_bars);
  g_assert_false (c->show_size_labels);
  nu_settings_shutdown ();
}

static void
test_invalid_colour_falls_back (void)
{
  g_autoptr (GSettingsBackend) backend = start ();
  g_autoptr (GSettings) s = direct_settings (backend);
  g_settings_set_string (s, "bar-colour", "#123456");
  drain ();
  g_assert_cmpuint (nu_settings_get ()->bar_colour.r, ==, 0x12);
  g_settings_set_string (s, "bar-colour", "red");
  drain ();
  g_assert_true (rgb_equal (nu_settings_get ()->bar_colour, nu_settings_defaults ()->bar_colour));
  nu_settings_shutdown ();
}

/* The dialog's delayed copy changes nothing until apply, and one apply = one notification. */
static void
test_delayed_apply_coalesces (void)
{
  g_autoptr (GSettingsBackend) backend = start ();
  Counter counter = { 0 };
  guint id = nu_settings_add_listener (count_listener, &counter);
  g_assert_cmpuint (id, >, 0);

  g_autoptr (GSettings) pending = nu_settings_new_delayed ();
  g_assert_nonnull (pending);
  g_settings_set_boolean (pending, "sidebar-drives-enabled", FALSE);
  g_settings_set_enum (pending, "bar-colour-mode", NU_BAR_COLOUR_CUSTOM);
  g_settings_set_string (pending, "bar-colour", "#ff0000");
  g_settings_set_boolean (pending, "show-usage-bars", FALSE);
  drain ();
  g_assert_true (g_settings_get_has_unapplied (pending));
  g_assert_cmpuint (counter.calls, ==, 0);
  g_assert_true (nu_settings_get ()->sidebar_drives_enabled);
  g_assert_cmpint (nu_settings_get ()->bar_colour_mode, ==, NU_BAR_COLOUR_ACCENT);

  g_settings_apply (pending);
  drain ();
  g_assert_false (g_settings_get_has_unapplied (pending));
  g_assert_cmpuint (counter.calls, ==, 1);
  g_assert_cmpuint (counter.last, ==, NU_CHANGED_SIDEBAR_DRIVES | NU_CHANGED_BAR_COLOUR | NU_CHANGED_ROW_CONTENT);
  const NuConfig *c = nu_settings_get ();
  g_assert_false (c->sidebar_drives_enabled);
  g_assert_cmpint (c->bar_colour_mode, ==, NU_BAR_COLOUR_CUSTOM);
  g_assert_cmpuint (c->bar_colour.r, ==, 0xff);
  g_assert_false (c->show_usage_bars);

  nu_settings_remove_listener (id);
  nu_settings_shutdown ();
}

static void
test_revert_discards (void)
{
  g_autoptr (GSettingsBackend) backend = start ();
  Counter counter = { 0 };
  nu_settings_add_listener (count_listener, &counter);
  g_autoptr (GSettings) pending = nu_settings_new_delayed ();
  g_settings_set_boolean (pending, "eject-slide", TRUE);
  g_settings_revert (pending);
  drain ();
  g_assert_false (g_settings_get_has_unapplied (pending));
  g_assert_cmpuint (counter.calls, ==, 0);
  g_assert_false (nu_settings_get ()->eject_slide);
  g_autoptr (GSettings) fresh = direct_settings (backend);
  g_assert_false (g_settings_get_boolean (fresh, "eject-slide"));
  nu_settings_shutdown ();
}

static void
test_each_group (void)
{
  static const struct
  {
    const char *key;
    NuChanged   group;
  } cases[] = {
    { "folder-preview-enabled", NU_CHANGED_FOLDER_PREVIEW },
    { "sidebar-drives-enabled", NU_CHANGED_SIDEBAR_DRIVES },
    { "show-usage-bars", NU_CHANGED_ROW_CONTENT },
    { "show-size-labels", NU_CHANGED_ROW_CONTENT },
    { "eject-slide", NU_CHANGED_EJECT },
    { "folder-views-enabled", NU_CHANGED_FOLDER_VIEWS },
    { "folder-views-defaults-captured", NU_CHANGED_FV_DEFAULT },
    { "folder-preview-border", NU_CHANGED_PREVIEW_STYLE },
    { "folder-preview-shadow", NU_CHANGED_PREVIEW_STYLE },
    { "folder-preview-rotation", NU_CHANGED_PREVIEW_STYLE },
    { "folder-preview-keep-aspect", NU_CHANGED_PREVIEW_STYLE },
    { "folder-preview-depth", NU_CHANGED_PREVIEW_STYLE },
    { "folder-preview-cycle", NU_CHANGED_PREVIEW_ICONS },
  };
  g_autoptr (GSettingsBackend) backend = start ();
  g_autoptr (GSettings) s = direct_settings (backend);
  Counter counter = { 0 };
  nu_settings_add_listener (count_listener, &counter);
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      counter.calls = 0;
      g_settings_set_boolean (s, cases[i].key, !g_settings_get_boolean (s, cases[i].key));
      drain ();
      g_assert_cmpuint (counter.calls, ==, 1);
      g_assert_cmpuint (counter.last, ==, cases[i].group);
    }
  for (const char *const *k = (const char *const[]) { "bar-gradient-start", "bar-gradient-end", NULL }; *k; k++)
    {
      counter.calls = 0;
      g_settings_set_string (s, *k, "#010203");
      drain ();
      g_assert_cmpuint (counter.calls, ==, 1);
      g_assert_cmpuint (counter.last, ==, NU_CHANGED_BAR_COLOUR);
    }
  static const struct
  {
    const char *key;
    int         value;
  } enums[] = {
    { "folder-views-default-view", NU_VIEW_LIST },
    { "folder-views-default-grid-zoom", 4 },
    { "folder-views-default-list-zoom", 0 },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (enums); i++)
    {
      counter.calls = 0;
      g_settings_set_enum (s, enums[i].key, enums[i].value);
      drain ();
      g_assert_cmpuint (counter.calls, ==, 1);
      g_assert_cmpuint (counter.last, ==, NU_CHANGED_FV_DEFAULT);
    }
  static const struct
  {
    const char *key;
    int         value;
  } preview_enums[] = {
    { "folder-preview-layout", NU_PREVIEW_STACKED },
    { "folder-preview-stack-step", NU_STACK_STEP_THREE },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (preview_enums); i++)
    {
      counter.calls = 0;
      g_settings_set_enum (s, preview_enums[i].key, preview_enums[i].value);
      drain ();
      g_assert_cmpuint (counter.calls, ==, 1);
      g_assert_cmpuint (counter.last, ==, NU_CHANGED_PREVIEW_STYLE);
    }
  g_assert_cmpint (nu_settings_get ()->preview_layout, ==, NU_PREVIEW_STACKED);
  g_assert_cmpint (nu_settings_get ()->preview_stack_step, ==, NU_STACK_STEP_THREE);
  counter.calls = 0;
  g_settings_set_string (s, "folder-preview-border-colour", "#102030");
  drain ();
  g_assert_cmpuint (counter.last, ==, NU_CHANGED_PREVIEW_STYLE);
  g_assert_cmpuint (nu_settings_get ()->preview_border_colour.g, ==, 0x20);
  for (const char *const *k = (const char *const[]) { "folder-preview-folder-icon", "folder-preview-overlay-icon", NULL };
       *k; k++)
    {
      counter.calls = 0;
      g_settings_set_string (s, *k, "/some/icon.svg");
      drain ();
      g_assert_cmpuint (counter.calls, ==, 1);
      g_assert_cmpuint (counter.last, ==, NU_CHANGED_PREVIEW_ICONS);
    }
  g_assert_cmpstr (nu_settings_get ()->preview_folder_icon, ==, "/some/icon.svg");
  g_assert_cmpstr (nu_settings_get ()->preview_overlay_icon, ==, "/some/icon.svg");
  NuFolderView d = nu_settings_get ()->folder_views_default;
  g_assert_cmpint (d.view, ==, NU_VIEW_LIST);
  g_assert_cmpuint (d.grid_zoom, ==, 4);
  g_assert_cmpuint (d.list_zoom, ==, 0);
  counter.calls = 0;
  g_settings_set_uint (s, "folder-views-generation", 5);
  drain ();
  g_assert_cmpuint (counter.calls, ==, 1);
  g_assert_cmpuint (counter.last, ==, NU_CHANGED_FV_GENERATION);
  g_assert_cmpuint (nu_settings_get ()->folder_views_generation, ==, 5);
  nu_settings_shutdown ();
}

static void
test_folder_view_valid (void)
{
  g_assert_false (nu_folder_view_valid (NULL));
  g_assert_true (nu_folder_view_valid (&nu_settings_defaults ()->folder_views_default));
  const NuFolderView good[] = { { NU_VIEW_LIST, 0, 0 }, { NU_VIEW_GRID, NU_GRID_ZOOM_MAX, NU_LIST_ZOOM_MAX } };
  const NuFolderView bad[] = { { 0, 0, 0 }, { 3, 0, 0 }, { NU_VIEW_GRID, NU_GRID_ZOOM_MAX + 1, 0 },
                               { NU_VIEW_LIST, 0, NU_LIST_ZOOM_MAX + 1 } };
  for (gsize i = 0; i < G_N_ELEMENTS (good); i++)
    g_assert_true (nu_folder_view_valid (&good[i]));
  for (gsize i = 0; i < G_N_ELEMENTS (bad); i++)
    g_assert_false (nu_folder_view_valid (&bad[i]));
}

/* The one-time copy of Files' default (PLAN.md §10.1.1): one batch, only once, invalid → built-in. */
static void
test_capture_folder_view_default (void)
{
  g_autoptr (GSettingsBackend) backend = start ();
  Counter counter = { 0 };
  nu_settings_add_listener (count_listener, &counter);
  g_assert_false (nu_settings_get ()->folder_views_defaults_captured);

  NuFolderView files = { NU_VIEW_LIST, 1, 2 };
  g_assert_true (nu_settings_capture_folder_view_default (&files));
  drain ();
  g_assert_cmpuint (counter.calls, ==, 1);
  g_assert_cmpuint (counter.last, ==, NU_CHANGED_FV_DEFAULT);
  const NuConfig *c = nu_settings_get ();
  g_assert_true (c->folder_views_defaults_captured);
  g_assert_cmpint (c->folder_views_default.view, ==, NU_VIEW_LIST);
  g_assert_cmpuint (c->folder_views_default.grid_zoom, ==, 1);
  g_assert_cmpuint (c->folder_views_default.list_zoom, ==, 2);

  /* Second capture (e.g. next start, after Files' keys drifted): nothing changes. */
  NuFolderView drifted = { NU_VIEW_GRID, 4, 0 };
  g_assert_false (nu_settings_capture_folder_view_default (&drifted));
  drain ();
  g_assert_cmpuint (counter.calls, ==, 1);
  g_assert_cmpint (nu_settings_get ()->folder_views_default.view, ==, NU_VIEW_LIST);

  /* Persisted: a fresh service on the same backend sees the captured default. */
  nu_settings_shutdown ();
  nu_settings_init_full (NU_SETTINGS_SCHEMA_ID, backend);
  g_assert_true (nu_settings_get ()->folder_views_defaults_captured);
  g_assert_cmpuint (nu_settings_get ()->folder_views_default.list_zoom, ==, 2);
  nu_settings_shutdown ();

  /* Invalid input → built-in default, still marked captured. */
  g_autoptr (GSettingsBackend) backend2 = start ();
  NuFolderView bad = { 9, 9, 9 };
  g_assert_true (nu_settings_capture_folder_view_default (&bad));
  drain ();
  const NuFolderView *d = &nu_settings_defaults ()->folder_views_default;
  g_assert_cmpint (nu_settings_get ()->folder_views_default.view, ==, d->view);
  g_assert_cmpuint (nu_settings_get ()->folder_views_default.grid_zoom, ==, d->grid_zoom);
  g_assert_true (nu_settings_get ()->folder_views_defaults_captured);
  nu_settings_shutdown ();

  /* NULL input behaves like invalid. */
  g_autoptr (GSettingsBackend) backend3 = start ();
  g_assert_true (nu_settings_capture_folder_view_default (NULL));
  drain ();
  g_assert_true (nu_settings_get ()->folder_views_defaults_captured);
  nu_settings_shutdown ();
}

/* Reset All bumps the generation immediately, persisted, one notification each. */
static void
test_reset_folder_views (void)
{
  g_autoptr (GSettingsBackend) backend = start ();
  Counter counter = { 0 };
  nu_settings_add_listener (count_listener, &counter);
  g_assert_cmpuint (nu_settings_get ()->folder_views_generation, ==, 0);
  for (guint i = 1; i <= 3; i++)
    {
      g_assert_true (nu_settings_reset_folder_views ());
      drain ();
      g_assert_cmpuint (nu_settings_get ()->folder_views_generation, ==, i);
      g_assert_cmpuint (counter.calls, ==, i);
      g_assert_cmpuint (counter.last, ==, NU_CHANGED_FV_GENERATION);
    }
  /* A pending dialog copy doesn't hold the reset back. */
  g_autoptr (GSettings) pending = nu_settings_new_delayed ();
  g_settings_set_boolean (pending, "folder-views-enabled", FALSE);
  g_assert_true (nu_settings_reset_folder_views ());
  drain ();
  g_assert_cmpuint (nu_settings_get ()->folder_views_generation, ==, 4);
  g_assert_true (nu_settings_get ()->folder_views_enabled);
  /* Wraps around instead of failing. */
  g_autoptr (GSettings) s = direct_settings (backend);
  g_settings_set_uint (s, "folder-views-generation", G_MAXUINT);
  drain ();
  g_assert_true (nu_settings_reset_folder_views ());
  drain ();
  g_assert_cmpuint (nu_settings_get ()->folder_views_generation, ==, 0);
  nu_settings_shutdown ();
}

static void
test_missing_schema (void)
{
  g_test_expect_message ("settings", G_LOG_LEVEL_WARNING, "*not.installed.Schema is not installed*");
  nu_settings_init_full ("not.installed.Schema", NULL);
  g_test_assert_expected_messages ();
  g_assert_false (nu_settings_available ());
  g_assert_null (nu_settings_new_delayed ());
  NuFolderView v = { NU_VIEW_LIST, 0, 0 };
  g_assert_false (nu_settings_capture_folder_view_default (&v));
  g_assert_false (nu_settings_reset_folder_views ());
  const NuConfig *d = nu_settings_defaults ();
  assert_config_equal (nu_settings_get (), d);
  /* Listeners can still be registered; they just never fire. */
  Counter counter = { 0 };
  guint id = nu_settings_add_listener (count_listener, &counter);
  drain ();
  g_assert_cmpuint (counter.calls, ==, 0);
  nu_settings_remove_listener (id);
  nu_settings_shutdown ();
}

static void
test_without_init (void)
{
  const NuConfig *d = nu_settings_defaults ();
  assert_config_equal (nu_settings_get (), d);
  g_assert_false (nu_settings_available ());
  g_assert_null (nu_settings_new_delayed ());
  nu_settings_remove_listener (5);
  nu_settings_shutdown ();
}

/* The production entry point: installed schema id + default backend (GSETTINGS_BACKEND=memory here). */
static void
test_default_init_idempotent (void)
{
  nu_settings_init ();
  g_assert_true (nu_settings_available ());
  const NuConfig *first = nu_settings_get ();
  nu_settings_init (); /* second init keeps the running service */
  g_assert_true (nu_settings_get () == first);
  nu_settings_shutdown ();
  g_assert_false (nu_settings_available ());
}

/* Listeners that remove others, remove themselves or shut the service down mid-dispatch. */
static guint victim_id;
static Counter victim_counter;

static void
remover_listener (NuChanged changed, gpointer data)
{
  (void) changed;
  (*(guint *) data)++;
  nu_settings_remove_listener (victim_id);
}

static void
shutdown_listener (NuChanged changed, gpointer data)
{
  (void) changed;
  (*(guint *) data)++;
  nu_settings_shutdown ();
}

static void
test_listener_reentrancy (void)
{
  g_autoptr (GSettingsBackend) backend = start ();
  g_autoptr (GSettings) s = direct_settings (backend);
  guint remover_calls = 0;
  nu_settings_add_listener (remover_listener, &remover_calls);
  victim_counter = (Counter) { 0 };
  victim_id = nu_settings_add_listener (count_listener, &victim_counter);
  g_settings_set_boolean (s, "eject-slide", TRUE);
  drain ();
  g_assert_cmpuint (remover_calls, ==, 1);
  g_assert_cmpuint (victim_counter.calls, ==, 0); /* removed before its turn */

  guint shutdown_calls = 0;
  nu_settings_add_listener (shutdown_listener, &shutdown_calls);
  Counter after = { 0 };
  nu_settings_add_listener (count_listener, &after);
  g_settings_set_boolean (s, "eject-slide", FALSE);
  drain ();
  g_assert_cmpuint (shutdown_calls, ==, 1);
  g_assert_cmpuint (after.calls, ==, 0); /* service gone: dispatch stopped */
  g_assert_false (nu_settings_available ());
  nu_settings_shutdown (); /* second shutdown is a no-op */
}

/* Init/shutdown with pending notifications, listeners and delayed copies, many times over. */
static void
test_stress (void)
{
  for (int i = 0; i < 300; i++)
    {
      g_autoptr (GSettingsBackend) backend = start ();
      g_autoptr (GSettings) s = direct_settings (backend);
      Counter counter = { 0 };
      for (int j = 0; j < 4; j++)
        nu_settings_add_listener (count_listener, &counter);
      g_autoptr (GSettings) pending = nu_settings_new_delayed ();
      g_settings_set_boolean (pending, "eject-slide", (i % 2) == 0);
      g_settings_set_string (pending, "bar-colour", (i % 3) == 0 ? "#abcdef" : "bogus");
      g_settings_apply (pending);
      g_settings_set_boolean (s, "folder-preview-enabled", (i % 2) != 0);
      if (i % 2 == 0)
        drain ();
      nu_settings_shutdown (); /* half the time with a notification still pending */
      drain ();
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/settings/rgb-parse", test_rgb_parse);
  g_test_add_func ("/settings/defaults-match-schema", test_defaults_match_schema);
  g_test_add_func ("/settings/reads-stored-values", test_reads_stored_values);
  g_test_add_func ("/settings/invalid-colour-falls-back", test_invalid_colour_falls_back);
  g_test_add_func ("/settings/delayed-apply-coalesces", test_delayed_apply_coalesces);
  g_test_add_func ("/settings/revert-discards", test_revert_discards);
  g_test_add_func ("/settings/each-group", test_each_group);
  g_test_add_func ("/settings/folder-view-valid", test_folder_view_valid);
  g_test_add_func ("/settings/capture-folder-view-default", test_capture_folder_view_default);
  g_test_add_func ("/settings/reset-folder-views", test_reset_folder_views);
  g_test_add_func ("/settings/missing-schema", test_missing_schema);
  g_test_add_func ("/settings/without-init", test_without_init);
  g_test_add_func ("/settings/default-init-idempotent", test_default_init_idempotent);
  g_test_add_func ("/settings/listener-reentrancy", test_listener_reentrancy);
  g_test_add_func ("/settings/stress", test_stress);
  return g_test_run ();
}
