/* Tests for src/folder-preview/icon_art.c (PLAN.md §11): loading custom folder SVGs, rendering them at exact
 * sizes (cached), measuring the opaque body, and refusing files that aren't SVGs. GtkSvg needs GTK to be
 * initialised (its CSS machinery), so without a display every test is skipped. */
#include <math.h>

#include "icon_art.h"

static char *
fixture (const char *name)
{
  return g_test_build_filename (G_TEST_DIST, "data", name, NULL);
}

static void
assert_rect (const graphene_rect_t *r, double x, double y, double w, double h)
{
  const double tol = 0.02; /* a few pixels on the 256 px probe, for antialiasing */
  if (fabs (r->origin.x - x) > tol || fabs (r->origin.y - y) > tol || fabs (r->size.width - w) > tol ||
      fabs (r->size.height - h) > tol)
    g_error ("rect %.3f,%.3f %.3f×%.3f, want %.3f,%.3f %.3f×%.3f", r->origin.x, r->origin.y, r->size.width,
             r->size.height, x, y, w, h);
}

static void
test_load_render_body (void)
{
  g_autofree char *path = fixture ("back.svg");
  g_autoptr (GError) error = NULL;
  g_autoptr (FpIconArt) art = fp_icon_art_new_from_file (path, &error);
  g_assert_no_error (error);
  g_assert_cmpfloat (fp_icon_art_get_width (art), ==, 100);
  g_assert_cmpfloat (fp_icon_art_get_height (art), ==, 100);

  /* The body leaves out the faint shadow below the opaque rectangle. */
  graphene_rect_t body;
  g_assert_true (fp_icon_art_get_body (art, &body));
  assert_rect (&body, 0.10, 0.20, 0.80, 0.60);
  graphene_rect_t again;
  g_assert_true (fp_icon_art_get_body (art, &again)); /* measured once, same answer */
  g_assert_true (graphene_rect_equal (&body, &again));

  /* Exact sizes, cached per size. */
  g_autoptr (GdkTexture) a = fp_icon_art_render (art, 96, 96);
  g_autoptr (GdkTexture) b = fp_icon_art_render (art, 96, 96);
  g_autoptr (GdkTexture) c = fp_icon_art_render (art, 192, 192);
  g_assert_nonnull (a);
  g_assert_true (a == b);
  g_assert_cmpint (gdk_texture_get_width (c), ==, 192);
  graphene_rect_t measured;
  g_assert_true (fp_texture_opaque_bounds (c, &measured));
  assert_rect (&measured, 0.10, 0.20, 0.80, 0.60);

  g_assert_null (fp_icon_art_render (art, 0, 10));
  g_assert_null (fp_icon_art_render (art, 10, 5000));

  /* Many sizes: the per-SVG cache stays bounded and keeps working. */
  for (int px = 16; px < 120; px++)
    {
      g_autoptr (GdkTexture) t = fp_icon_art_render (art, px, px);
      g_assert_cmpint (gdk_texture_get_height (t), ==, px);
    }
}

/* A non-square SVG is drawn as large as fits, centred (letterboxed). */
static void
test_aspect_kept (void)
{
  g_autofree char *path = fixture ("wide.svg");
  g_autoptr (FpIconArt) art = fp_icon_art_new_from_file (path, NULL);
  g_assert_nonnull (art);
  graphene_rect_t body;
  g_assert_true (fp_icon_art_get_body (art, &body));
  assert_rect (&body, 0, 0.25, 1, 0.5);
  g_autoptr (GdkTexture) t = fp_icon_art_render (art, 200, 100); /* its own aspect: fills */
  graphene_rect_t full;
  g_assert_true (fp_texture_opaque_bounds (t, &full));
  assert_rect (&full, 0, 0, 1, 1);
}

static void
test_bad_files (void)
{
  const struct { const char *name; int code; } cases[] = {
    { "notes.txt", G_IO_ERROR_INVALID_DATA },
    { "broken.png", G_IO_ERROR_INVALID_DATA },
    { "does-not-exist.svg", G_IO_ERROR_NOT_FOUND },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree char *path = fixture (cases[i].name);
      g_autoptr (GError) error = NULL;
      g_autoptr (FpIconArt) art = fp_icon_art_new_from_file (path, &error);
      if (art != NULL)
        g_error ("%s loaded as an SVG", cases[i].name);
      g_assert_error (error, G_IO_ERROR, cases[i].code);
    }
  /* A valid but empty SVG loads, but has no body (the caller then uses the theme icon). */
  g_autofree char *empty = fixture ("empty.svg");
  g_autoptr (FpIconArt) art = fp_icon_art_new_from_file (empty, NULL);
  g_assert_nonnull (art);
  graphene_rect_t body;
  g_assert_false (fp_icon_art_get_body (art, &body));
}

static void
test_render_paintable (void)
{
  g_autofree char *path = fixture ("overlay.svg");
  g_autoptr (FpIconArt) art = fp_icon_art_new_from_file (path, NULL);
  g_autoptr (GdkTexture) t = fp_icon_art_render (art, 80, 40);
  /* A texture is a paintable too: drawn at another size, it stretches. */
  g_autoptr (GdkTexture) r = fp_render_paintable (GDK_PAINTABLE (t), 50, 50);
  g_assert_cmpint (gdk_texture_get_width (r), ==, 50);
  graphene_rect_t b;
  g_assert_true (fp_texture_opaque_bounds (r, &b));
  assert_rect (&b, 0, 0, 1, 1);
  g_assert_null (fp_render_paintable (GDK_PAINTABLE (t), 0, 5));
}

/* Leak hunting: load, render, free, many times. */
static void
test_stress (void)
{
  g_autofree char *path = fixture ("back.svg");
  for (int i = 0; i < 200; i++)
    {
      g_autoptr (FpIconArt) art = fp_icon_art_new_from_file (path, NULL);
      g_autoptr (GdkTexture) t = fp_icon_art_render (art, 32 + i % 5, 32);
      graphene_rect_t body;
      if (i % 2 == 0)
        g_assert_true (fp_icon_art_get_body (art, &body));
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  if (!gtk_init_check ())
    {
      g_test_skip ("no display: GtkSvg needs an initialised GTK");
      return g_test_run ();
    }
  g_test_add_func ("/icon-art/load-render-body", test_load_render_body);
  g_test_add_func ("/icon-art/aspect-kept", test_aspect_kept);
  g_test_add_func ("/icon-art/bad-files", test_bad_files);
  g_test_add_func ("/icon-art/render-paintable", test_render_paintable);
  g_test_add_func ("/icon-art/stress", test_stress);
  return g_test_run ();
}
