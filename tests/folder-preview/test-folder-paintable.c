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
  g_test_add_func ("/folder-paintable/stress", test_stress);
  return g_test_run ();
}
