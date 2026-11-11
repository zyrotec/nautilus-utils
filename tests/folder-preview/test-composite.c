/* Tests for src/composite.c (Dolphin-style frames). Inputs are generated PNGs with known colours. Tilt and
 * nudge make exact pixel positions arbitrary, so the tests check properties that define the look: aspect
 * ratio kept, slot order, empty slots empty, white photo border, bounded and stable tilt, determinism.
 * Set FP_DEBUG_DIR to also write the frames as PNGs for a visual check. */
#include <math.h>
#include <string.h>

#include "composite.h"
#include "config.h"
#include "test-util.h"

static char *dir; /* Owned temp dir with the input PNGs. */

typedef struct { guint8 r, g, b, a; } Px;

static const Px RED = { 220, 30, 30, 255 }, GREEN = { 30, 200, 30, 255 }, BLUE = { 30, 30, 220, 255 },
                YELLOW = { 230, 220, 20, 255 };

/* Writes a w×h PNG of one colour; returns its path (caller frees). */
static char *
make_png (const char *name, int w, int h, Px c)
{
  gsize stride = (gsize) w * 4;
  g_autofree guint8 *buf = g_malloc (stride * (gsize) h);
  for (gsize i = 0; i < stride * (gsize) h; i += 4)
    {
      buf[i] = c.r; buf[i + 1] = c.g; buf[i + 2] = c.b; buf[i + 3] = c.a;
    }
  g_autoptr (GBytes) bytes = g_bytes_new_take (g_steal_pointer (&buf), stride * (gsize) h);
  g_autoptr (GdkTexture) t = gdk_memory_texture_new (w, h, GDK_MEMORY_R8G8B8A8, bytes, stride);
  char *path = g_build_filename (dir, name, NULL);
  g_assert_true (gdk_texture_save_to_png (t, path));
  return path;
}

/* Straight-alpha RGBA copy of @t; caller frees. */
static guint8 *
pixels (GdkTexture *t)
{
  int w = gdk_texture_get_width (t), h = gdk_texture_get_height (t);
  guint8 *buf = g_malloc ((gsize) w * 4 * (gsize) h);
  g_autoptr (GdkTextureDownloader) d = gdk_texture_downloader_new (t);
  gdk_texture_downloader_set_format (d, GDK_MEMORY_R8G8B8A8);
  gdk_texture_downloader_download_into (d, buf, (gsize) w * 4);
  return buf;
}

static Px
pixel (GdkTexture *t, double fx, double fy)
{
  int w = gdk_texture_get_width (t), h = gdk_texture_get_height (t);
  int x = CLAMP ((int) fx, 0, w - 1), y = CLAMP ((int) fy, 0, h - 1);
  g_autofree guint8 *buf = pixels (t);
  guint8 *p = buf + ((gsize) y * (gsize) w + (gsize) x) * 4;
  return (Px) { p[0], p[1], p[2], p[3] };
}

static gboolean
near (Px a, Px b)
{
  return ABS (a.r - b.r) < 45 && ABS (a.g - b.g) < 45 && ABS (a.b - b.b) < 45 && a.a > 200;
}

static void
assert_colour (GdkTexture *t, double x, double y, Px want, const char *what)
{
  Px got = pixel (t, x, y);
  if (!near (got, want))
    g_error ("%s at (%.0f,%.0f) = %u,%u,%u,%u, want ≈ %u,%u,%u", what, x, y, got.r, got.g, got.b, got.a,
             want.r, want.g, want.b);
}

static void
assert_clear (GdkTexture *t, double x, double y, const char *what)
{
  Px got = pixel (t, x, y);
  if (got.a > 40) /* allow the faint edge of a shadow */
    g_error ("%s at (%.0f,%.0f) not transparent: alpha %u", what, x, y, got.a);
}

static char *red_png, *green_png, *blue_png, *yellow_png, *wide_png, *tall_png; /* Owned paths. */

static void
debug_save (GdkTexture *t, const char *name)
{
  const char *out = g_getenv ("FP_DEBUG_DIR");
  if (out == NULL || t == NULL)
    return;
  g_mkdir_with_parents (out, 0755);
  g_autofree char *path = g_build_filename (out, name, NULL);
  gdk_texture_save_to_png (t, path);
}

/* Centre and size of item @i's section when the frame (width @w) has @n items. */
static void
slot_center (guint w, guint n, guint i, double *x, double *y, double *cw, double *ch)
{
  double sx, sy;
  fp_composite_slot_rect (n, i, w, &sx, &sy, cw, ch);
  *x = sx + *cw / 2;
  *y = sy + *ch / 2;
}

static void
test_tilt (void)
{
  g_autoptr (GHashTable) seen = g_hash_table_new (g_int64_hash, g_int64_equal);
  double min = 0, max = 0;
  for (guint seed = 0; seed < 1000; seed++)
    {
      double t = fp_composite_tilt (seed);
      g_assert_cmpfloat (fabs (t), <=, FRAME_MAX_TILT_DEG);
      g_assert_cmpfloat (t, ==, fp_composite_tilt (seed)); /* stable */
      min = MIN (min, t);
      max = MAX (max, t);
    }
  /* Varied: uses most of the range in both directions. */
  g_assert_cmpfloat (min, <, -0.8 * FRAME_MAX_TILT_DEG);
  g_assert_cmpfloat (max, >, 0.8 * FRAME_MAX_TILT_DEG);
  g_assert_cmpfloat (fp_composite_tilt (1), !=, fp_composite_tilt (2));
}

static void
test_four_slots (void)
{
  FpCompositeItem items[] = { { red_png, 11 }, { green_png, 22 }, { blue_png, 33 }, { yellow_png, 44 } };
  const Px want[] = { RED, GREEN, BLUE, YELLOW };
  guint widths[] = { 16, 33, 64, 73, 146, 195, 512 };
  for (gsize s = 0; s < G_N_ELEMENTS (widths); s++)
    {
      guint w = widths[s];
      guint unreadable = 99;
      g_autoptr (GdkTexture) t = fp_composite_frame (items, 4, w, &unreadable);
      g_assert_nonnull (t);
      g_assert_cmpuint (unreadable, ==, 0);
      g_assert_cmpint (gdk_texture_get_width (t), ==, (int) w);
      g_assert_cmpint (gdk_texture_get_height (t), ==, (int) fp_composite_frame_height (w));
      if (w >= 64) /* at tiny sizes the centre pixel may be border/filtering */
        for (guint i = 0; i < 4; i++)
          {
            double x, y, cw, ch;
            slot_center (w, 4, i, &x, &y, &cw, &ch);
            assert_colour (t, x, y, want[i], "slot centre");
          }
      g_autofree char *name = g_strdup_printf ("four-%u.png", w);
      debug_save (t, name);
    }
}

static void
test_aspect_ratio_kept (void)
{
  /* 4:1 and 1:4 images: coloured along their long axis, transparent across it — neither cropped to a
   * square nor stretched to fill the cell. */
  guint w = 400;
  for (guint seed = 0; seed < 8; seed++)
    {
      FpCompositeItem wide[] = { { wide_png, seed } };
      g_autoptr (GdkTexture) t = fp_composite_frame (wide, 1, w, NULL);
      double x, y, cw, ch;
      slot_center (w, 1, 0, &x, &y, &cw, &ch); /* one item: the whole face */
      assert_colour (t, x, y, RED, "wide centre");
      assert_colour (t, x - 0.3 * cw, y, RED, "wide left");
      assert_colour (t, x + 0.3 * cw, y, RED, "wide right");
      assert_clear (t, x, y - 0.44 * ch, "above wide image");
      assert_clear (t, x, y + 0.44 * ch, "below wide image");
      if (seed == 0)
        debug_save (t, "wide-400.png");

      FpCompositeItem tall[] = { { NULL, 0 }, { tall_png, seed } };
      g_autoptr (GdkTexture) u = fp_composite_frame (tall, 2, w, NULL);
      slot_center (w, 2, 1, &x, &y, &cw, &ch); /* two items: right half */
      assert_colour (u, x, y, GREEN, "tall centre");
      assert_colour (u, x, y - 0.3 * ch, GREEN, "tall top");
      assert_clear (u, x - 0.4 * cw, y, "left of tall image");
      assert_clear (u, x + 0.4 * cw, y, "right of tall image");
      if (seed == 0)
        debug_save (u, "tall-400.png");
    }
}

/* The owner's layout: 1 → whole area, 2 → halves, 3 → two on top + one centred below, 4 → quarters. */
static void
test_layout_by_count (void)
{
  guint w = 300;
  double x, y, cw, ch;

  /* 1: centred in the whole face; far corner of the frame stays clear. */
  FpCompositeItem one[] = { { blue_png, 5 } };
  g_autoptr (GdkTexture) t1 = fp_composite_frame (one, 1, w, NULL);
  slot_center (w, 1, 0, &x, &y, &cw, &ch);
  g_assert_cmpfloat (fabs (x - w / 2.0), <, 1);
  assert_colour (t1, x, y, BLUE, "single photo");
  /* A square photo in the wide face is limited by the height: it spans most of it. */
  assert_colour (t1, x, y - 0.35 * ch, BLUE, "single photo is large (top)");
  assert_colour (t1, x, y + 0.35 * ch, BLUE, "single photo is large (bottom)");
  assert_colour (t1, x - 0.2 * cw, y, BLUE, "single photo is large (left)");
  assert_colour (t1, x + 0.2 * cw, y, BLUE, "single photo is large (right)");
  assert_clear (t1, 1, 1, "frame corner");
  debug_save (t1, "layout-1.png");

  /* 2: left half and right half. */
  FpCompositeItem two[] = { { red_png, 1 }, { green_png, 2 } };
  g_autoptr (GdkTexture) t2 = fp_composite_frame (two, 2, w, NULL);
  slot_center (w, 2, 0, &x, &y, &cw, &ch);
  assert_colour (t2, x, y, RED, "left half");
  slot_center (w, 2, 1, &x, &y, &cw, &ch);
  assert_colour (t2, x, y, GREEN, "right half");
  debug_save (t2, "layout-2.png");

  /* 3: two on top, one centred below — the bottom quarters' own centres stay clear. */
  FpCompositeItem three[] = { { red_png, 1 }, { green_png, 2 }, { blue_png, 3 } };
  g_autoptr (GdkTexture) t3 = fp_composite_frame (three, 3, w, NULL);
  slot_center (w, 3, 0, &x, &y, &cw, &ch);
  assert_colour (t3, x, y, RED, "top left");
  slot_center (w, 3, 1, &x, &y, &cw, &ch);
  assert_colour (t3, x, y, GREEN, "top right");
  slot_center (w, 3, 2, &x, &y, &cw, &ch);
  g_assert_cmpfloat (fabs (x - w / 2.0), <, 1); /* centred horizontally */
  assert_colour (t3, x, y, BLUE, "bottom centre");
  slot_center (w, 4, 2, &x, &y, &cw, &ch);
  assert_clear (t3, x - 0.25 * cw, y, "bottom-left corner area");
  slot_center (w, 4, 3, &x, &y, &cw, &ch);
  assert_clear (t3, x + 0.25 * cw, y, "bottom-right corner area");
  debug_save (t3, "layout-3.png");

  /* 4 with NULL holes: holes stay empty, the others keep their quarters. */
  FpCompositeItem holes[] = { { NULL, 0 }, { green_png, 1 }, { NULL, 0 }, { yellow_png, 2 } };
  g_autoptr (GdkTexture) th = fp_composite_frame (holes, 4, w, NULL);
  slot_center (w, 4, 0, &x, &y, &cw, &ch);
  assert_clear (th, x, y, "hole");
  slot_center (w, 4, 3, &x, &y, &cw, &ch);
  assert_colour (th, x, y, YELLOW, "quarter 3");

  /* Photos have a white border: some near-white pixels exist although no input is white. */
  g_autofree guint8 *buf = pixels (t1);
  guint white = 0;
  guint h = fp_composite_frame_height (w);
  for (gsize i = 0; i < (gsize) w * h * 4; i += 4)
    if (buf[i] > 240 && buf[i + 1] > 240 && buf[i + 2] > 240 && buf[i + 3] > 240)
      white++;
  g_assert_cmpuint (white, >, 50);

  /* Nothing at all → fully transparent frame. */
  g_autoptr (GdkTexture) t0 = fp_composite_frame (NULL, 0, w, NULL);
  g_autofree guint8 *b0 = pixels (t0);
  for (gsize i = 3; i < (gsize) w * h * 4; i += 4)
    g_assert_cmpuint (b0[i], ==, 0);
}

static void
test_deterministic (void)
{
  FpCompositeItem a[] = { { red_png, 7 }, { green_png, 8 }, { blue_png, 9 } };
  FpCompositeItem b[] = { { red_png, 7 }, { green_png, 8 }, { blue_png, 10 } };
  g_autoptr (GdkTexture) t1 = fp_composite_frame (a, 3, 146, NULL);
  g_autoptr (GdkTexture) t2 = fp_composite_frame (a, 3, 146, NULL);
  g_autoptr (GdkTexture) t3 = fp_composite_frame (b, 3, 146, NULL);
  gsize n = 146 * 4 * (gsize) fp_composite_frame_height (146);
  g_autofree guint8 *p1 = pixels (t1);
  g_autofree guint8 *p2 = pixels (t2);
  g_autofree guint8 *p3 = pixels (t3);
  g_assert_cmpmem (p1, n, p2, n);              /* same files → same picture */
  g_assert_true (memcmp (p1, p3, n) != 0);     /* another file's seed → another tilt */
}

static void
test_unreadable_and_bad_sizes (void)
{
  g_autofree char *missing = g_build_filename (dir, "missing.png", NULL);
  g_autofree char *garbage = g_build_filename (dir, "garbage.png", NULL);
  g_assert_true (g_file_set_contents (garbage, "not a png", -1, NULL));
  FpCompositeItem items[] = { { missing, 1 }, { red_png, 2 }, { garbage, 3 }, { NULL, 4 } };
  guint unreadable = 0;
  g_autoptr (GdkTexture) t = fp_composite_frame (items, 4, 200, &unreadable);
  g_assert_nonnull (t);
  g_assert_cmpuint (unreadable, ==, 2);
  double x, y, cw, ch;
  slot_center (200, 4, 1, &x, &y, &cw, &ch);
  assert_colour (t, x, y, RED, "readable slot");
  slot_center (200, 4, 0, &x, &y, &cw, &ch);
  assert_clear (t, x, y, "missing slot");

  /* Absurdly large sources are refused (not decoded into huge buffers). */
  g_autofree char *huge = make_png ("huge.png", 2000, 10, RED);
  FpCompositeItem hp[] = { { huge, 1 } };
  g_autoptr (GdkTexture) th = fp_composite_frame (hp, 1, 64, &unreadable);
  g_assert_cmpuint (unreadable, ==, 1);

  g_assert_null (fp_composite_frame (items, 4, 15, NULL));
  g_assert_null (fp_composite_frame (items, 4, 0, NULL));
  g_assert_null (fp_composite_frame (items, 4, 5000, NULL));
}

/* PLAN.md §4.2: thousands of composite create/destroy cycles. */
static void
test_stress (void)
{
  FpCompositeItem items[] = { { red_png, 1 }, { green_png, 2 }, { blue_png, 3 }, { yellow_png, 4 } };
  for (guint i = 0; i < 2000; i++)
    {
      items[0].seed = i;
      g_autoptr (GdkTexture) t = fp_composite_frame (items, 1 + i % 4, 16 + (i % 7) * 8, NULL);
      g_assert_nonnull (t);
    }
}

int
main (int argc, char **argv)
{
  dir = tu_make_tmpdir ("fp-test-composite");
  g_test_init (&argc, &argv, NULL);
  red_png = make_png ("red.png", 256, 192, RED);
  green_png = make_png ("green.png", 192, 256, GREEN);
  blue_png = make_png ("blue.png", 256, 256, BLUE);
  yellow_png = make_png ("yellow.png", 40, 30, YELLOW); /* small source: upscaled */
  wide_png = make_png ("wide.png", 400, 100, RED);
  tall_png = make_png ("tall.png", 100, 400, GREEN);

  g_test_add_func ("/composite/tilt", test_tilt);
  g_test_add_func ("/composite/four-slots", test_four_slots);
  g_test_add_func ("/composite/aspect-ratio-kept", test_aspect_ratio_kept);
  g_test_add_func ("/composite/layout-by-count", test_layout_by_count);
  g_test_add_func ("/composite/deterministic", test_deterministic);
  g_test_add_func ("/composite/unreadable-and-bad-sizes", test_unreadable_and_bad_sizes);
  g_test_add_func ("/composite/stress", test_stress);
  int ret = g_test_run ();

  g_clear_pointer (&red_png, g_free);
  g_clear_pointer (&green_png, g_free);
  g_clear_pointer (&blue_png, g_free);
  g_clear_pointer (&yellow_png, g_free);
  g_clear_pointer (&wide_png, g_free);
  g_clear_pointer (&tall_png, g_free);
  tu_rm_rf (dir);
  g_clear_pointer (&dir, g_free);
  return ret;
}
