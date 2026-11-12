/* Tests for src/sidebar-drives/fade_bin.c (PLAN.md §9.5 Phase 6). The geometry needs no display; the widget
 * tests need one (skipped without) but never map a window: mapped rendering pulls in font/GPU caches that
 * leak checkers report, and is covered by the in-Nautilus renders and the smoke test instead. */
#include <gtk/gtk.h>

#include "fade_bin.h"

static gboolean have_display;

#define NEED_DISPLAY()                       \
  G_STMT_START                               \
  {                                          \
    if (!have_display)                       \
      {                                      \
        g_test_skip ("no display");          \
        return;                              \
      }                                      \
  }                                          \
  G_STMT_END

static void
test_mask_line (void)
{
  graphene_point_t opaque, clear;
  g_assert_true (sd_fade_mask_line (200.0f, 34.0f, FALSE, &opaque, &clear));
  g_assert_cmpfloat (opaque.x, ==, 166.0f);
  g_assert_cmpfloat (clear.x, ==, 200.0f);
  g_assert_true (sd_fade_mask_line (200.0f, 34.0f, TRUE, &opaque, &clear)); /* right-to-left: button on the left */
  g_assert_cmpfloat (opaque.x, ==, 34.0f);
  g_assert_cmpfloat (clear.x, ==, 0.0f);
  g_assert_true (sd_fade_mask_line (20.0f, 34.0f, FALSE, &opaque, &clear)); /* clamped to the width */
  g_assert_cmpfloat (opaque.x, ==, 0.0f);
  g_assert_false (sd_fade_mask_line (200.0f, 0.0f, FALSE, &opaque, &clear));
  g_assert_false (sd_fade_mask_line (200.0f, 0.4f, FALSE, &opaque, &clear));
  g_assert_false (sd_fade_mask_line (0.0f, 34.0f, FALSE, &opaque, &clear));

  /* Opaque at the button's outer edge, transparent from its middle on (icon never over visible content). */
  g_assert_cmpfloat (sd_fade_mask_stops[0].offset, ==, 0.0f);
  g_assert_cmpfloat (sd_fade_mask_stops[0].color.alpha, ==, 1.0f);
  g_assert_cmpfloat (sd_fade_mask_stops[1].offset, ==, 0.5f);
  g_assert_cmpfloat (sd_fade_mask_stops[1].color.alpha, ==, 0.0f);
  g_assert_cmpfloat (sd_fade_mask_stops[SD_FADE_MASK_N_STOPS - 1].offset, ==, 1.0f);
  g_assert_cmpfloat (sd_fade_mask_stops[SD_FADE_MASK_N_STOPS - 1].color.alpha, ==, 0.0f);
}

static void
test_child (void)
{
  NEED_DISPLAY ();
  GtkWidget *bin = g_object_ref_sink (sd_fade_bin_new ());
  GtkWidget *a = g_object_ref_sink (gtk_label_new ("a"));
  GtkWidget *b = g_object_ref_sink (gtk_label_new ("b"));
  sd_fade_bin_set_child (SD_FADE_BIN (bin), a);
  g_assert_true (gtk_widget_get_parent (a) == bin);
  g_assert_true (sd_fade_bin_get_child (SD_FADE_BIN (bin)) == a);
  sd_fade_bin_set_child (SD_FADE_BIN (bin), b); /* replaces: a is unparented */
  g_assert_null (gtk_widget_get_parent (a));
  g_assert_true (gtk_widget_get_parent (b) == bin);
  sd_fade_bin_set_child (SD_FADE_BIN (bin), NULL);
  g_assert_null (gtk_widget_get_parent (b));
  sd_fade_bin_set_child (SD_FADE_BIN (bin), a);
  g_object_unref (bin); /* disposes: unparents a */
  g_assert_null (gtk_widget_get_parent (a));
  g_object_unref (a);
  g_object_unref (b);

  GtkWidget *f = g_object_ref_sink (sd_fade_bin_new ());
  sd_fade_bin_set_fade_width (SD_FADE_BIN (f), 34.0);
  g_assert_cmpfloat (sd_fade_bin_get_fade_width (SD_FADE_BIN (f)), ==, 34.0);
  sd_fade_bin_set_fade_width (SD_FADE_BIN (f), -5.0); /* clamped to 0 */
  g_assert_cmpfloat (sd_fade_bin_get_fade_width (SD_FADE_BIN (f)), ==, 0.0);
  g_object_unref (f);
}

/* An unmapped bin draws no mask (snapshot of the unmapped tree must also be safe). */
static void
snapshot_unmapped (GtkWidget *bin)
{
  GtkSnapshot *snapshot = gtk_snapshot_new ();
  GTK_WIDGET_GET_CLASS (bin)->snapshot (bin, snapshot);
  g_autoptr (GskRenderNode) node = gtk_snapshot_free_to_node (snapshot);
  g_assert_true (node == NULL || gsk_render_node_get_node_type (node) != GSK_MASK_NODE);
}

/* Following a revealer: weak in both directions, whichever is destroyed first. */
static void
test_follow_lifecycle (void)
{
  NEED_DISPLAY ();
  for (int i = 0; i < 300; i++)
    {
      GtkWidget *bin = g_object_ref_sink (sd_fade_bin_new ());
      sd_fade_bin_set_child (SD_FADE_BIN (bin), gtk_button_new ());
      GtkWidget *revealer = g_object_ref_sink (gtk_revealer_new ());
      gtk_revealer_set_child (GTK_REVEALER (revealer), gtk_button_new ());
      sd_fade_bin_follow (SD_FADE_BIN (bin), GTK_REVEALER (revealer));
      gtk_revealer_set_reveal_child (GTK_REVEALER (revealer), (i % 2) == 0);
      snapshot_unmapped (bin);
      if (i % 3 == 0)
        sd_fade_bin_follow (SD_FADE_BIN (bin), NULL);
      if (i % 5 == 0)
        sd_fade_bin_follow (SD_FADE_BIN (bin), GTK_REVEALER (revealer)); /* re-follow the same revealer */
      if (i % 2 == 0)
        {
          g_object_unref (revealer); /* revealer first: the bin's weak ref must clear */
          snapshot_unmapped (bin);
          sd_fade_bin_follow (SD_FADE_BIN (bin), NULL);
          g_object_unref (bin);
        }
      else
        {
          g_object_unref (bin); /* bin first: its handler on the revealer must go */
          gtk_revealer_set_reveal_child (GTK_REVEALER (revealer), FALSE);
          gtk_revealer_set_reveal_child (GTK_REVEALER (revealer), TRUE);
          g_object_unref (revealer);
        }
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  have_display = gtk_init_check ();
  g_test_add_func ("/fade-bin/mask-line", test_mask_line);
  g_test_add_func ("/fade-bin/child", test_child);
  g_test_add_func ("/fade-bin/follow-lifecycle", test_follow_lifecycle);
  return g_test_run ();
}
