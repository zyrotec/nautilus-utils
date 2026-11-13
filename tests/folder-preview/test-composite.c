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
static FpStyle grid_style; /* The v1 look (fp_style_init_default), set in main. */

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
      g_autoptr (GdkTexture) t = fp_composite_frame (&grid_style, items, 4, w, &unreadable);
      g_assert_nonnull (t);
      g_assert_cmpuint (unreadable, ==, 0);
      g_assert_cmpint (gdk_texture_get_width (t), ==, (int) w);
      g_assert_cmpint (gdk_texture_get_height (t), ==, (int) fp_composite_frame_height (&grid_style, w));
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
      g_autoptr (GdkTexture) t = fp_composite_frame (&grid_style, wide, 1, w, NULL);
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
      g_autoptr (GdkTexture) u = fp_composite_frame (&grid_style, tall, 2, w, NULL);
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
  g_autoptr (GdkTexture) t1 = fp_composite_frame (&grid_style, one, 1, w, NULL);
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
  g_autoptr (GdkTexture) t2 = fp_composite_frame (&grid_style, two, 2, w, NULL);
  slot_center (w, 2, 0, &x, &y, &cw, &ch);
  assert_colour (t2, x, y, RED, "left half");
  slot_center (w, 2, 1, &x, &y, &cw, &ch);
  assert_colour (t2, x, y, GREEN, "right half");
  debug_save (t2, "layout-2.png");

  /* 3: two on top, one centred below — the bottom quarters' own centres stay clear. */
  FpCompositeItem three[] = { { red_png, 1 }, { green_png, 2 }, { blue_png, 3 } };
  g_autoptr (GdkTexture) t3 = fp_composite_frame (&grid_style, three, 3, w, NULL);
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
  g_autoptr (GdkTexture) th = fp_composite_frame (&grid_style, holes, 4, w, NULL);
  slot_center (w, 4, 0, &x, &y, &cw, &ch);
  assert_clear (th, x, y, "hole");
  slot_center (w, 4, 3, &x, &y, &cw, &ch);
  assert_colour (th, x, y, YELLOW, "quarter 3");

  /* Photos have a white border: some near-white pixels exist although no input is white. */
  g_autofree guint8 *buf = pixels (t1);
  guint white = 0;
  guint h = fp_composite_frame_height (&grid_style, w);
  for (gsize i = 0; i < (gsize) w * h * 4; i += 4)
    if (buf[i] > 240 && buf[i + 1] > 240 && buf[i + 2] > 240 && buf[i + 3] > 240)
      white++;
  g_assert_cmpuint (white, >, 50);

  /* Nothing at all → fully transparent frame. */
  g_autoptr (GdkTexture) t0 = fp_composite_frame (&grid_style, NULL, 0, w, NULL);
  g_autofree guint8 *b0 = pixels (t0);
  for (gsize i = 3; i < (gsize) w * h * 4; i += 4)
    g_assert_cmpuint (b0[i], ==, 0);
}

static void
test_deterministic (void)
{
  FpCompositeItem a[] = { { red_png, 7 }, { green_png, 8 }, { blue_png, 9 } };
  FpCompositeItem b[] = { { red_png, 7 }, { green_png, 8 }, { blue_png, 10 } };
  g_autoptr (GdkTexture) t1 = fp_composite_frame (&grid_style, a, 3, 146, NULL);
  g_autoptr (GdkTexture) t2 = fp_composite_frame (&grid_style, a, 3, 146, NULL);
  g_autoptr (GdkTexture) t3 = fp_composite_frame (&grid_style, b, 3, 146, NULL);
  gsize n = 146 * 4 * (gsize) fp_composite_frame_height (&grid_style, 146);
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
  g_autoptr (GdkTexture) t = fp_composite_frame (&grid_style, items, 4, 200, &unreadable);
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
  g_autoptr (GdkTexture) th = fp_composite_frame (&grid_style, hp, 1, 64, &unreadable);
  g_assert_cmpuint (unreadable, ==, 1);

  g_assert_null (fp_composite_frame (&grid_style, items, 4, 15, NULL));
  g_assert_null (fp_composite_frame (&grid_style, items, 4, 0, NULL));
  g_assert_null (fp_composite_frame (&grid_style, items, 4, 5000, NULL));
}

/* Style helpers (PLAN.md §11): default = v1 grid look, keys differ per look, slots and steps per layout. */
static void
test_style_helpers (void)
{
  FpStyle a, b;
  fp_style_init_default (&a);
  g_assert_cmpint (a.layout, ==, FP_LAYOUT_GRID);
  g_assert_true (a.border && a.shadow && a.rotation);
  g_assert_cmpuint (a.border_r & a.border_g & a.border_b, ==, 0xff);
  g_assert_cmpuint (fp_style_slots (&a), ==, PREVIEW_SLOTS);
  g_assert_cmpuint (fp_style_step (&a), ==, PREVIEW_SLOTS);
  b = a;
  g_assert_true (fp_style_equal (&a, &b));
  g_autofree char *ka = fp_style_key (&a);
  b.border_g = 0x10;
  g_assert_false (fp_style_equal (&a, &b));
  g_autofree char *kb = fp_style_key (&b);
  g_assert_cmpstr (ka, !=, kb);

  b = a;
  b.stack_step = 3; /* ignored by the grid: same key */
  g_autofree char *kc = fp_style_key (&b);
  g_assert_cmpstr (ka, ==, kc);
  b.layout = FP_LAYOUT_STACKED;
  g_assert_cmpuint (fp_style_slots (&b), ==, STACK_CARDS);
  g_assert_cmpuint (fp_style_step (&b), ==, 3);
  g_autofree char *kd = fp_style_key (&b);
  b.stack_step = 1;
  g_assert_cmpuint (fp_style_step (&b), ==, 1);
  g_autofree char *ke = fp_style_key (&b);
  g_assert_cmpstr (kd, !=, ke);
  g_assert_cmpstr (ka, !=, ke);
  b.stack_step = 0; /* anything but 3 is one per step */
  g_assert_cmpuint (fp_style_step (&b), ==, 1);
}

/* Grid with rotation off: border colour, border off and shadow off are visible at known pixels. */
static void
test_grid_style_options (void)
{
  guint w = 300;
  FpCompositeItem one[] = { { blue_png, 5 } };
  double cx, cy, cw, ch;
  slot_center (w, 1, 0, &cx, &cy, &cw, &ch);
  double pic = MIN (cw, ch) * 0.96; /* the square blue photo incl. border */

  FpStyle st = grid_style;
  st.rotation = FALSE;
  st.border_r = 0, st.border_g = 0, st.border_b = 0;
  g_autoptr (GdkTexture) black = fp_composite_frame (&st, one, 1, w, NULL);
  debug_save (black, "grid-black-border.png");
  assert_colour (black, cx, cy, BLUE, "photo centre");
  assert_colour (black, cx - pic / 2 + 2, cy, (Px) { 0, 0, 0, 255 }, "black border");

  st.border = FALSE;
  st.shadow = FALSE;
  g_autoptr (GdkTexture) plain = fp_composite_frame (&st, one, 1, w, NULL);
  debug_save (plain, "grid-plain.png");
  assert_colour (plain, cx - pic / 2 + 2, cy, BLUE, "no border: picture reaches the edge");
  assert_clear (plain, cx, cy + pic / 2 + 3, "no shadow below the photo");
  Px below = pixel (plain, cx, cy + pic / 2 + 3);
  g_assert_cmpuint (below.a, ==, 0);

  st.shadow = TRUE;
  g_autoptr (GdkTexture) shadow = fp_composite_frame (&st, one, 1, w, NULL);
  g_assert_cmpuint (pixel (shadow, cx, cy + pic / 2 + 3).a, >, 0); /* shadow below the photo */
}

/* Px darkened by @dim (the stack's depth cue). */
static Px
dimmed (Px c, double dim)
{
  return (Px) { (guint8) (c.r * (1 - dim)), (guint8) (c.g * (1 - dim)), (guint8) (c.b * (1 - dim)), 255 };
}

/* Stacked layout: aspect ratio, card order (front on top), the back cards' top edges, blank cards, and
 * fewer cards for fewer photos. */
static void
test_stacked (void)
{
  FpStyle st = grid_style;
  st.layout = FP_LAYOUT_STACKED;
  st.rotation = FALSE;
  st.border = FALSE;
  st.keep_aspect = FALSE; /* pictures fill the cards */
  st.depth = FALSE;       /* plain dimming: exact colours */
  guint w = 336;
  guint h = fp_composite_frame_height (&st, w);
  g_assert_cmpfloat (fabs ((double) h / w - (STACK_H + 2 * STACK_PAD) / (1 + 2 * STACK_PAD)), <, 0.01);

  FpCompositeItem items[] = { { red_png, 1 }, { green_png, 2 }, { blue_png, 3 } };
  g_autoptr (GdkTexture) t = fp_composite_frame (&st, items, 3, w, NULL);
  debug_save (t, "stacked-3.png");
  g_assert_cmpint (gdk_texture_get_width (t), ==, (int) w);
  g_assert_cmpint (gdk_texture_get_height (t), ==, (int) h);

  double x[3], y[3], cw[3], ch[3];
  for (guint i = 0; i < 3; i++)
    fp_composite_card_rect (i, w, &x[i], &y[i], &cw[i], &ch[i]);
  /* Each card further back is narrower and higher, all the same height. */
  g_assert_cmpfloat (cw[1], <, cw[0]);
  g_assert_cmpfloat (cw[2], <, cw[1]);
  g_assert_cmpfloat (y[1], <, y[0]);
  g_assert_cmpfloat (y[2], <, y[1]);
  g_assert_cmpfloat (ch[0], ==, ch[2]);

  double mid = w / 2.0;
  assert_colour (t, mid, y[0] + ch[0] / 2, RED, "front card shows the first photo (cropped to fill)");
  assert_colour (t, x[0] + 3, y[0] + ch[0] / 2, RED, "front card filled to its left edge");
  assert_colour (t, mid, (y[1] + y[0]) / 2, dimmed (GREEN, STACK_DIM), "middle card's top edge");
  assert_colour (t, mid, (y[2] + y[1]) / 2, dimmed (BLUE, 2 * STACK_DIM), "back card's top edge");
  assert_clear (t, mid, y[2] - 4, "above the stack");

  /* One photo: one card; the space of the back cards stays clear. */
  g_autoptr (GdkTexture) t1 = fp_composite_frame (&st, items, 1, w, NULL);
  debug_save (t1, "stacked-1.png");
  assert_colour (t1, mid, y[0] + ch[0] / 2, RED, "single card");
  assert_clear (t1, mid, (y[2] + y[1]) / 2, "no back card");

  /* A hole is a blank card in the border colour (border on) so the stack keeps its shape. */
  st.border = TRUE;
  st.border_r = 10, st.border_g = 200, st.border_b = 10;
  FpCompositeItem holes[] = { { red_png, 1 }, { NULL, 0 }, { "/nonexistent.png", 3 } };
  guint unreadable = 0;
  g_autoptr (GdkTexture) th = fp_composite_frame (&st, holes, 3, w, &unreadable);
  debug_save (th, "stacked-holes.png");
  g_assert_cmpuint (unreadable, ==, 1);
  assert_colour (th, mid, (y[2] + y[1]) / 2, dimmed ((Px) { 10, 200, 10, 255 }, 2 * STACK_DIM), "blank back card");
  assert_colour (th, x[0] + 1, y[0] + ch[0] / 2, (Px) { 10, 200, 10, 255 }, "front card border");

  /* Rotation and shadow on: still the right photo in the middle, deterministic. */
  st.rotation = TRUE;
  g_autoptr (GdkTexture) r1 = fp_composite_frame (&st, items, 3, w, NULL);
  g_autoptr (GdkTexture) r2 = fp_composite_frame (&st, items, 3, w, NULL);
  debug_save (r1, "stacked-tilted.png");
  assert_colour (r1, mid, y[0] + ch[0] / 2, RED, "tilted front card");
  g_autofree guint8 *p1 = pixels (r1);
  g_autofree guint8 *p2 = pixels (r2);
  g_assert_cmpint (memcmp (p1, p2, (gsize) w * 4 * h), ==, 0);

  /* Too many items for the layout is refused. */
  FpCompositeItem four[] = { { red_png, 1 }, { red_png, 2 }, { red_png, 3 }, { red_png, 4 } };
  g_test_expect_message ("folder-preview", G_LOG_LEVEL_CRITICAL, "*n_items <= fp_style_slots*");
  g_assert_null (fp_composite_frame (&st, four, 4, w, NULL));
  g_test_assert_expected_messages ();
}

/* Keep aspect off (grid): a 4:1 picture fills its whole section, cropped. Stacked with keep aspect on: the
 * card takes the picture's shape (a 4:1 picture makes a short card, top-aligned in the card rectangle). */
static void
test_keep_aspect_option (void)
{
  guint w = 400;
  FpStyle st = grid_style;
  st.keep_aspect = FALSE;
  st.rotation = FALSE;
  FpCompositeItem wide[] = { { wide_png, 1 } };
  g_autoptr (GdkTexture) t = fp_composite_frame (&st, wide, 1, w, NULL);
  debug_save (t, "grid-fill.png");
  double x, y, cw, ch;
  slot_center (w, 1, 0, &x, &y, &cw, &ch);
  assert_colour (t, x, y - 0.40 * ch, RED, "fills above the 4:1 picture's own height");
  assert_colour (t, x, y + 0.40 * ch, RED, "fills below too");

  st.layout = FP_LAYOUT_STACKED;
  st.keep_aspect = TRUE;
  st.border = FALSE;
  st.depth = FALSE;
  g_autoptr (GdkTexture) s1 = fp_composite_frame (&st, wide, 1, w, NULL);
  debug_save (s1, "stacked-aspect.png");
  double cx, cy, cardw, cardh;
  fp_composite_card_rect (0, w, &cx, &cy, &cardw, &cardh);
  double pich = cardw / 4; /* the 4:1 picture, full card width */
  assert_colour (s1, cx + cardw / 2, cy + pich / 2, RED, "card top: picture");
  assert_clear (s1, cx + cardw / 2, cy + pich + (cardh - pich) / 2, "below the short card");
  st.keep_aspect = FALSE;
  g_autoptr (GdkTexture) s2 = fp_composite_frame (&st, wide, 1, w, NULL);
  assert_colour (s2, cx + cardw / 2, cy + cardh * 0.8, RED, "cropped: the card is filled");
}

/* Depth effect: cards further back are darker than with plain dimming, and their edges are soft. */
static void
test_depth (void)
{
  FpStyle st = grid_style;
  st.layout = FP_LAYOUT_STACKED;
  st.rotation = FALSE;
  st.border = FALSE;
  st.shadow = FALSE;
  st.keep_aspect = FALSE;
  guint w = 400;
  FpCompositeItem items[] = { { red_png, 1 }, { green_png, 2 }, { blue_png, 3 } };
  st.depth = FALSE;
  g_autoptr (GdkTexture) flat = fp_composite_frame (&st, items, 3, w, NULL);
  st.depth = TRUE;
  g_autoptr (GdkTexture) deep = fp_composite_frame (&st, items, 3, w, NULL);
  debug_save (deep, "stacked-depth.png");
  double x[3], y[3], cw[3], ch[3];
  for (guint i = 0; i < 3; i++)
    fp_composite_card_rect (i, w, &x[i], &y[i], &cw[i], &ch[i]);
  double mid = w / 2.0, back = (y[2] + y[1]) / 2;
  Px f = pixel (flat, mid, back), d = pixel (deep, mid, back);
  g_assert_cmpuint (d.b, <, f.b); /* darker */
  g_assert_true (near (d, dimmed (BLUE, 2 * STACK_DEPTH_DIM)));
  /* Soft top edge: just above the back card there is some colour with depth, none without. */
  g_assert_cmpuint (pixel (flat, mid, y[2] - 2).a, ==, 0);
  g_assert_cmpuint (pixel (deep, mid, y[2] - 2).a, >, 0);
  /* The front card stays sharp and undimmed. */
  assert_colour (deep, mid, y[0] + ch[0] / 2, RED, "front card");
}

/* The drop shadow follows the picture's shape: a picture whose right half is transparent casts a shadow
 * only under its left half (a box shadow would darken under both). */
static void
test_drop_shadow_shape (void)
{
  gsize stride = 200 * 4;
  g_autofree guint8 *buf = g_malloc0 (stride * 100);
  for (int yy = 0; yy < 100; yy++)
    for (int xx = 0; xx < 100; xx++)
      {
        guint8 *p = buf + (gsize) yy * stride + (gsize) xx * 4;
        p[0] = 220, p[1] = 30, p[2] = 30, p[3] = 255;
      }
  g_autoptr (GBytes) bytes = g_bytes_new_take (g_steal_pointer (&buf), stride * 100);
  g_autoptr (GdkTexture) half = gdk_memory_texture_new (200, 100, GDK_MEMORY_R8G8B8A8, bytes, stride);
  g_autofree char *path = g_build_filename (dir, "half.png", NULL);
  g_assert_true (gdk_texture_save_to_png (half, path));

  FpStyle st = grid_style;
  st.border = FALSE;
  st.rotation = FALSE;
  FpCompositeItem one[] = { { path, 1 } };
  guint w = 400;
  g_autoptr (GdkTexture) t = fp_composite_frame (&st, one, 1, w, NULL);
  debug_save (t, "drop-shadow.png");
  double x, y, cw, ch;
  slot_center (w, 1, 0, &x, &y, &cw, &ch);
  double pw = cw * 0.96, ph = pw / 2; /* the 2:1 picture fills the width */
  double below = y + ph / 2 + 3;
  g_assert_cmpuint (pixel (t, x - pw / 4, below).a, >, 0);   /* under the opaque half */
  g_assert_cmpuint (pixel (t, x + pw / 4, below).a, ==, 0);  /* under the transparent half: nothing */
  g_assert_cmpuint (pixel (t, x + pw / 4, y).a, ==, 0);      /* and no box behind it */
}

/* PLAN.md §4.2: thousands of composite create/destroy cycles. */
static void
test_stress (void)
{
  FpCompositeItem items[] = { { red_png, 1 }, { green_png, 2 }, { blue_png, 3 }, { yellow_png, 4 } };
  for (guint i = 0; i < 2000; i++)
    {
      items[0].seed = i;
      g_autoptr (GdkTexture) t = fp_composite_frame (&grid_style, items, 1 + i % 4, 16 + (i % 7) * 8, NULL);
      g_assert_nonnull (t);
      FpStyle st = grid_style;
      st.layout = FP_LAYOUT_STACKED;
      st.border = (i % 2) == 0;
      st.shadow = (i % 3) == 0;
      st.keep_aspect = (i % 5) != 0;
      st.depth = (i % 7) != 0;
      g_autoptr (GdkTexture) u = fp_composite_frame (&st, items, i % 4, 16 + (i % 5) * 8, NULL);
      g_assert_nonnull (u);
    }
}

int
main (int argc, char **argv)
{
  dir = tu_make_tmpdir ("fp-test-composite");
  g_test_init (&argc, &argv, NULL);
  fp_style_init_default (&grid_style);
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
  g_test_add_func ("/composite/style-helpers", test_style_helpers);
  g_test_add_func ("/composite/grid-style-options", test_grid_style_options);
  g_test_add_func ("/composite/stacked", test_stacked);
  g_test_add_func ("/composite/keep-aspect-option", test_keep_aspect_option);
  g_test_add_func ("/composite/depth", test_depth);
  g_test_add_func ("/composite/drop-shadow-shape", test_drop_shadow_shape);
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
