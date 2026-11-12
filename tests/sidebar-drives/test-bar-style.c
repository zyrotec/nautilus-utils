/* Tests for src/sidebar-drives/bar_style.c (PLAN.md §9.5 Phase 5). */
#include <gtk/gtk.h>

#include "bar_style.h"

static void
test_accent_is_empty (void)
{
  NuConfig c = *nu_settings_defaults ();
  c.bar_colour_mode = NU_BAR_COLOUR_ACCENT;
  g_autofree char *css = sd_bar_colour_css (&c);
  g_assert_cmpstr (css, ==, "");
}

static void
test_custom (void)
{
  NuConfig c = *nu_settings_defaults ();
  c.bar_colour_mode = NU_BAR_COLOUR_CUSTOM;
  c.bar_colour = (NuRgb) { 0xff, 0x00, 0x7f };
  g_autofree char *css = sd_bar_colour_css (&c);
  g_assert_nonnull (strstr (css, "background-color: #ff007f;"));
  g_assert_nonnull (strstr (css, "background-image: none;"));
  /* N3: overrides the warning/full colours too. */
  g_assert_nonnull (strstr (css, ".sd-warning block.filled"));
  g_assert_nonnull (strstr (css, ".sd-full block.filled"));
}

static void
test_gradient (void)
{
  NuConfig c = *nu_settings_defaults ();
  c.bar_colour_mode = NU_BAR_COLOUR_GRADIENT;
  c.gradient_start = (NuRgb) { 0x11, 0x22, 0x33 };
  c.gradient_end = (NuRgb) { 0xaa, 0xbb, 0xcc };
  g_autofree char *css = sd_bar_colour_css (&c);
  g_assert_nonnull (strstr (css, "linear-gradient(to right, #112233, #aabbcc)"));
  g_assert_nonnull (strstr (css, ".sd-full block.filled"));
}

/* The generated CSS must parse cleanly in GTK (no parsing-error signal). */
static void
on_parse_error (GtkCssProvider *provider, GtkCssSection *section, GError *error, gpointer data)
{
  (void) provider;
  (void) section;
  g_test_message ("CSS error: %s", error->message);
  (*(int *) data)++;
}

static void
test_css_parses (void)
{
  for (int mode = NU_BAR_COLOUR_ACCENT; mode <= NU_BAR_COLOUR_GRADIENT; mode++)
    {
      NuConfig c = *nu_settings_defaults ();
      c.bar_colour_mode = (NuBarColourMode) mode;
      g_autofree char *css = sd_bar_colour_css (&c);
      g_autoptr (GtkCssProvider) provider = gtk_css_provider_new ();
      int errors = 0;
      g_signal_connect (provider, "parsing-error", G_CALLBACK (on_parse_error), &errors);
      gtk_css_provider_load_from_string (provider, css);
      g_assert_cmpint (errors, ==, 0);
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/bar-style/accent-is-empty", test_accent_is_empty);
  g_test_add_func ("/bar-style/custom", test_custom);
  g_test_add_func ("/bar-style/gradient", test_gradient);
  g_test_add_func ("/bar-style/css-parses", test_css_parses);
  return g_test_run ();
}
