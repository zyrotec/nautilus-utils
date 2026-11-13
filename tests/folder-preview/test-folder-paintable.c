/* Tests for src/folder_paintable.c: what gets drawn where, invalidation, ownership. No display needed:
 * snapshots are recorded into render-node trees and inspected. */
#include "config.h"
#include "folder_paintable.h"

/* Caller owns the result. */
static GdkTexture *
solid (int w, int h, guint8 r)
{
  gsize stride = (gsize) w * 4;
  g_autofree guint8 *px = g_malloc0 (stride * (gsize) h);
  for (gsize i = 0; i < stride * (gsize) h; i += 4)
    {
      px[i] = r;
      px[i + 3] = 255;
    }
  g_autoptr (GBytes) bytes = g_bytes_new_take (g_steal_pointer (&px), stride * (gsize) h);
  return gdk_memory_texture_new (w, h, GDK_MEMORY_R8G8B8A8, bytes, stride);
}

/* Collects (texture, bounds) of every texture / texture-scale node under @node. */
typedef struct { GdkTexture *texture; graphene_rect_t bounds; } Drawn;

static void
collect (GskRenderNode *node, GArray *out)
{
  switch (gsk_render_node_get_node_type (node))
    {
    case GSK_CONTAINER_NODE:
      for (guint i = 0; i < gsk_container_node_get_n_children (node); i++)
        collect (gsk_container_node_get_child (node, i), out);
      break;
    case GSK_TEXTURE_NODE:
    case GSK_TEXTURE_SCALE_NODE:
      {
        Drawn d;
        d.texture = gsk_render_node_get_node_type (node) == GSK_TEXTURE_NODE ? gsk_texture_node_get_texture (node)
                                                                              : gsk_texture_scale_node_get_texture (node);
        gsk_render_node_get_bounds (node, &d.bounds);
        g_array_append_val (out, d);
        break;
      }
    default:
      break;
    }
}

/* Returns what drawing @p at w×h produces (caller frees the array). */
static GArray *
draw (GdkPaintable *p, double w, double h)
{
  GtkSnapshot *s = gtk_snapshot_new ();
  gdk_paintable_snapshot (p, s, w, h);
  g_autoptr (GskRenderNode) node = gtk_snapshot_free_to_node (s);
  GArray *out = g_array_new (FALSE, FALSE, sizeof (Drawn));
  if (node != NULL)
    collect (node, out);
  return out;
}

static void
test_draws_icon_then_frame (void)
{
  g_autoptr (GdkTexture) icon = solid (96, 96, 10);
  g_autoptr (GdkTexture) frame = solid (54, 54, 200);
  g_autoptr (FpFolderPaintable) p = fp_folder_paintable_new (GDK_PAINTABLE (icon), 96);
  g_assert_cmpint (gdk_paintable_get_intrinsic_width (GDK_PAINTABLE (p)), ==, 96);
  g_assert_cmpint (gdk_paintable_get_intrinsic_height (GDK_PAINTABLE (p)), ==, 96);

  /* No frame yet: only the icon, full size. */
  g_autoptr (GArray) a = draw (GDK_PAINTABLE (p), 96, 96);
  g_assert_cmpuint (a->len, ==, 1);
  g_assert_true (g_array_index (a, Drawn, 0).texture == icon);

  fp_folder_paintable_set_frame (p, frame, "file:///photos");
  g_autoptr (GArray) b = draw (GDK_PAINTABLE (p), 96, 96);
  g_assert_cmpuint (b->len, ==, 2);
  g_assert_true (g_array_index (b, Drawn, 0).texture == icon);  /* icon first … */
  Drawn f = g_array_index (b, Drawn, 1);                         /* … frame on top */
  g_assert_true (f.texture == frame);
  graphene_rect_t want;
  fp_folder_paintable_frame_rect (96, 96, &want);
  g_assert_true (graphene_rect_equal (&f.bounds, &want));
  /* Inside the icon, on the lower ("front face") part. */
  g_assert_cmpfloat (f.bounds.origin.x, >=, 0);
  g_assert_cmpfloat (f.bounds.origin.y + f.bounds.size.height, <=, 96);
  g_assert_cmpfloat (f.bounds.origin.y + f.bounds.size.height / 2, >, 48);
  g_assert_cmpstr (fp_folder_paintable_get_frame_uri (p), ==, "file:///photos");

  /* Scales with the drawn size (zoom). */
  g_autoptr (GArray) c = draw (GDK_PAINTABLE (p), 256, 256);
  fp_folder_paintable_frame_rect (256, 256, &want);
  g_assert_true (graphene_rect_equal (&g_array_index (c, Drawn, 1).bounds, &want));

  /* Clearing the frame clears its URI too. */
  fp_folder_paintable_set_frame (p, NULL, "file:///ignored");
  g_assert_null (fp_folder_paintable_get_frame (p));
  g_assert_null (fp_folder_paintable_get_frame_uri (p));

  /* No icon: only the frame. */
  g_autoptr (FpFolderPaintable) q = fp_folder_paintable_new (NULL, 64);
  fp_folder_paintable_set_frame (q, frame, "file:///x");
  g_autoptr (GArray) d = draw (GDK_PAINTABLE (q), 64, 64);
  g_assert_cmpuint (d->len, ==, 1);
  g_assert_true (fp_folder_paintable_get_icon (q) == NULL);
}

static void
count (GdkPaintable *p, gpointer data)
{
  (void) p;
  (*(guint *) data)++;
}

static void
test_invalidation (void)
{
  g_autoptr (GdkTexture) icon = solid (48, 48, 1);
  g_autoptr (GdkTexture) icon2 = solid (48, 48, 2);
  g_autoptr (GdkTexture) frame = solid (27, 27, 3);
  g_autoptr (FpFolderPaintable) p = fp_folder_paintable_new (GDK_PAINTABLE (icon), 48);
  guint contents = 0, size = 0;
  g_signal_connect (p, "invalidate-contents", G_CALLBACK (count), &contents);
  g_signal_connect (p, "invalidate-size", G_CALLBACK (count), &size);

  fp_folder_paintable_set_frame (p, frame, "file:///a");
  g_assert_cmpuint (contents, ==, 1);
  fp_folder_paintable_set_frame (p, frame, "file:///a"); /* unchanged: no redraw */
  g_assert_cmpuint (contents, ==, 1);
  fp_folder_paintable_set_frame (p, frame, "file:///b"); /* same pixels, other folder: redraw */
  g_assert_cmpuint (contents, ==, 2);
  fp_folder_paintable_set_icon (p, GDK_PAINTABLE (icon2));
  g_assert_cmpuint (contents, ==, 3);
  fp_folder_paintable_set_icon (p, GDK_PAINTABLE (icon2));
  g_assert_cmpuint (contents, ==, 3);
  fp_folder_paintable_set_size (p, 64);
  g_assert_cmpuint (size, ==, 1);
  fp_folder_paintable_set_size (p, 64);
  g_assert_cmpuint (size, ==, 1);
  g_assert_cmpint (gdk_paintable_get_intrinsic_width (GDK_PAINTABLE (p)), ==, 64);
  fp_folder_paintable_set_size (p, -5); /* clamped */
  g_assert_cmpint (gdk_paintable_get_intrinsic_width (GDK_PAINTABLE (p)), ==, 0);
}

static void
test_frame_px (void)
{
  g_assert_cmpuint (fp_folder_paintable_frame_px (96, 1), ==, 88);   /* 0.76 × 1.2 × 96 = 87.55 */
  g_assert_cmpuint (fp_folder_paintable_frame_px (96, 2), ==, 175);
  g_assert_cmpuint (fp_folder_paintable_frame_px (256, 1), ==, 233);
  g_assert_cmpuint (fp_folder_paintable_frame_px (0, 1), ==, 0);
  g_assert_cmpuint (fp_folder_paintable_frame_px (96, 0), ==, 0);
}

static void
assert_near_rect (const graphene_rect_t *r, double x, double y, double w, double h)
{
  g_assert_cmpfloat_with_epsilon (r->origin.x, x, 1e-4);
  g_assert_cmpfloat_with_epsilon (r->origin.y, y, 1e-4);
  g_assert_cmpfloat_with_epsilon (r->size.width, w, 1e-4);
  g_assert_cmpfloat_with_epsilon (r->size.height, h, 1e-4);
}

/* PLAN.md §11.1: stack placement from the folder body. */
static void
test_decor_geometry (void)
{
  FpDecor grid;
  fp_decor_init_grid (&grid);
  graphene_rect_t want;
  fp_folder_paintable_frame_rect (1, 1, &want);
  g_assert_true (graphene_rect_equal (&grid.frame_rect, &want));
  g_assert_cmpuint (fp_decor_frame_px (&grid, 96, 1), ==, fp_folder_paintable_frame_px (96, 1));
  g_assert_null (grid.back);
  g_assert_null (grid.overlay);

  graphene_rect_t body = GRAPHENE_RECT_INIT (0.1f, 0.2f, 0.8f, 0.6f);
  FpDecor st;
  fp_decor_init_stacked (&st, &body, FP_STACK_CENTRE, 0);
  g_assert_cmpint (st.layout, ==, FP_LAYOUT_STACKED);
  /* 8 px from both sides at 64 × 64: 48 px wide; centre = inside the folder body. */
  double sw = 48.0 / 64, pad = STACK_PAD * sw, sh = STACK_H * sw;
  assert_near_rect (&st.frame_rect, 8.0 / 64 - pad, 0.2 + STACK_BODY_TOP * 0.6 - pad, sw + 2 * pad, sh + 2 * pad);
  /* Top / bottom: 8 px from the icon's edge; the offset moves it (px at 64 × 64). */
  FpDecor top, bottom, moved;
  fp_decor_init_stacked (&top, &body, FP_STACK_TOP, 0);
  assert_near_rect (&top.frame_rect, 8.0 / 64 - pad, 8.0 / 64 - pad, sw + 2 * pad, sh + 2 * pad);
  fp_decor_init_stacked (&bottom, &body, FP_STACK_BOTTOM, 0);
  g_assert_cmpfloat_with_epsilon (bottom.frame_rect.origin.y + bottom.frame_rect.size.height - pad, 56.0 / 64, 1e-4);
  fp_decor_init_stacked (&moved, &body, FP_STACK_TOP, -4);
  g_assert_cmpfloat_with_epsilon (moved.frame_rect.origin.y, top.frame_rect.origin.y - 4.0 / 64, 1e-4);
  /* The frame's size doesn't depend on the position: moving the stack needs no new frames. */
  g_assert_cmpuint (fp_decor_frame_px (&top, 96, 2), ==, fp_decor_frame_px (&st, 96, 2));
  /* The frame texture's aspect ratio matches the rectangle it is drawn in. */
  FpStyle style;
  fp_style_init_default (&style);
  style.layout = FP_LAYOUT_STACKED;
  guint px = fp_decor_frame_px (&st, 256, 1);
  g_assert_cmpfloat_with_epsilon ((double) fp_composite_frame_height (&style, px) / px,
                                  st.frame_rect.size.height / st.frame_rect.size.width, 0.01);
}

/* Stacked decor: custom back and overlay are drawn only together with a frame (V3), in order
 * back → frame → overlay; the paintable keeps its own refs. */
static void
test_decor_drawing (void)
{
  g_autoptr (GdkTexture) icon = solid (96, 96, 10);
  g_autoptr (GdkTexture) frame = solid (60, 40, 200);
  GdkTexture *back = solid (96, 96, 50);     /* owned until handed over below */
  GdkTexture *overlay = solid (96, 96, 90);  /* owned until handed over below */
  g_autoptr (FpFolderPaintable) p = fp_folder_paintable_new (GDK_PAINTABLE (icon), 96);
  guint contents = 0;
  g_signal_connect (p, "invalidate-contents", G_CALLBACK (count), &contents);

  graphene_rect_t body = GRAPHENE_RECT_INIT (0.1f, 0.2f, 0.8f, 0.6f);
  FpDecor d;
  fp_decor_init_stacked (&d, &body, FP_STACK_CENTRE, 0);
  d.back = back;
  d.overlay = overlay;
  fp_folder_paintable_set_decor (p, &d);
  g_assert_cmpuint (contents, ==, 1);
  fp_folder_paintable_set_decor (p, &d); /* unchanged: no redraw */
  g_assert_cmpuint (contents, ==, 1);
  g_object_unref (back);    /* the paintable holds its own refs now */
  g_object_unref (overlay);

  g_autoptr (GArray) a = draw (GDK_PAINTABLE (p), 96, 96);
  g_assert_cmpuint (a->len, ==, 1); /* no frame: the theme icon only */
  g_assert_true (g_array_index (a, Drawn, 0).texture == icon);

  fp_folder_paintable_set_frame (p, frame, "file:///photos");
  g_autoptr (GArray) b = draw (GDK_PAINTABLE (p), 96, 96);
  g_assert_cmpuint (b->len, ==, 3);
  g_assert_true (g_array_index (b, Drawn, 0).texture == back);
  graphene_rect_t full = GRAPHENE_RECT_INIT (0, 0, 96, 96);
  g_assert_true (graphene_rect_equal (&g_array_index (b, Drawn, 0).bounds, &full));
  g_assert_true (g_array_index (b, Drawn, 1).texture == frame);
  Drawn o = g_array_index (b, Drawn, 2);
  g_assert_true (o.texture == overlay);
  g_assert_true (graphene_rect_equal (&o.bounds, &full)); /* same size and place as the back (V4) */

  /* Back to the grid look: icon + frame on the front face. */
  fp_folder_paintable_set_decor (p, NULL);
  g_autoptr (GArray) c = draw (GDK_PAINTABLE (p), 96, 96);
  g_assert_cmpuint (c->len, ==, 2);
  g_assert_true (g_array_index (c, Drawn, 0).texture == icon);
  graphene_rect_t want;
  fp_folder_paintable_frame_rect (96, 96, &want);
  g_assert_true (graphene_rect_equal (&g_array_index (c, Drawn, 1).bounds, &want));
}

/* Leak hunting under ASan/valgrind: many create/draw/destroy cycles with ref juggling. */
static void
test_stress (void)
{
  g_autoptr (GdkTexture) icon = solid (32, 32, 9);
  for (guint i = 0; i < 2000; i++)
    {
      g_autoptr (GdkTexture) frame = solid (16, 16, (guint8) i);
      g_autoptr (FpFolderPaintable) p = fp_folder_paintable_new (GDK_PAINTABLE (icon), 32);
      fp_folder_paintable_set_frame (p, frame, "file:///s");
      if (i % 2 == 0)
        {
          graphene_rect_t body = GRAPHENE_RECT_INIT (0, 0, 1, 1);
          FpDecor d;
          fp_decor_init_stacked (&d, &body, FP_STACK_CENTRE, 0);
          d.back = icon;
          d.overlay = i % 4 == 0 ? frame : NULL;
          fp_folder_paintable_set_decor (p, &d);
        }
      if (i % 3 == 0)
        fp_folder_paintable_set_frame (p, NULL, NULL);
      g_autoptr (GArray) a = draw (GDK_PAINTABLE (p), 32, 32);
      g_assert_cmpuint (a->len, >=, 1);
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/folder-paintable/draws-icon-then-frame", test_draws_icon_then_frame);
  g_test_add_func ("/folder-paintable/invalidation", test_invalidation);
  g_test_add_func ("/folder-paintable/frame-px", test_frame_px);
  g_test_add_func ("/folder-paintable/decor-geometry", test_decor_geometry);
  g_test_add_func ("/folder-paintable/decor-drawing", test_decor_drawing);
  g_test_add_func ("/folder-paintable/stress", test_stress);
  return g_test_run ();
}
