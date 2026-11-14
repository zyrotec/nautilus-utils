/* Details pane demo (PLAN.md §13.6 Phase 2): the pane on its own, without Files.
 *
 *   demo-pane [DIR]                    a window: DIR's files on the left (Ctrl/Shift for several), the pane right
 *   demo-pane --png OUT.png FILE…      renders the pane for FILE… (one or several) to OUT.png and exits
 *   demo-pane --stress N FILE…         N rounds of show / play / destroy over FILE… (for ASan/valgrind), exits
 *
 * Needs a display. Not run by `meson test`. */
#include <adwaita.h>

#include "dp_pane.h"

static gboolean
wait_idle (DpPane *pane, guint timeout_ms)
{
  gint64 end = g_get_monotonic_time () + (gint64) timeout_ms * 1000;
  while (g_get_monotonic_time () < end)
    {
      g_main_context_iteration (NULL, FALSE);
      if (dp_pane_get_n_pending (pane) == 0)
        {
          /* Let layout and a few frames happen (the widget paintable needs a drawn frame). */
          gint64 settle = g_get_monotonic_time () + 400 * 1000;
          while (g_get_monotonic_time () < settle)
            {
              g_main_context_iteration (NULL, FALSE);
              g_usleep (2000);
            }
          return TRUE;
        }
      g_usleep (2000);
    }
  return FALSE;
}

static GPtrArray *
files_from_args (int argc, char **argv)
{
  GPtrArray *files = g_ptr_array_new_with_free_func (g_object_unref);
  for (int i = 0; i < argc; i++)
    g_ptr_array_add (files, g_file_new_for_commandline_arg (argv[i]));
  return files;
}

static int
render_png (const char *out, int argc, char **argv)
{
  g_autoptr (GPtrArray) files = files_from_args (argc, argv);
  GtkWidget *window = gtk_window_new ();
  gtk_window_set_default_size (GTK_WINDOW (window), 340, 1300);
  GtkWidget *pane = dp_pane_new ();
  gtk_window_set_child (GTK_WINDOW (window), pane);
  gtk_window_present (GTK_WINDOW (window));
  g_autoptr (GFile) parent = files->len > 0 ? g_file_get_parent (g_ptr_array_index (files, 0)) : NULL;
  dp_pane_show (DP_PANE (pane), parent, files, files->len);
  gboolean done = wait_idle (DP_PANE (pane), 15000);
  g_autofree char *text = dp_pane_describe (DP_PANE (pane));
  g_print ("%s\n", text);
  g_autoptr (GdkPaintable) shot = gtk_widget_paintable_new (pane);
  int w = gtk_widget_get_width (pane), h = gtk_widget_get_height (pane);
  g_autoptr (GtkSnapshot) snap = gtk_snapshot_new ();
  /* The window background (the off-screen render is transparent): libadwaita's window colour. */
  gboolean dark = adw_style_manager_get_dark (adw_style_manager_get_default ());
  GdkRGBA bg = dark ? (GdkRGBA) { 0.133, 0.133, 0.149, 1 } : (GdkRGBA) { 0.98, 0.98, 0.98, 1 };
  gtk_snapshot_append_color (snap, &bg, &GRAPHENE_RECT_INIT (0, 0, w, h));
  gdk_paintable_snapshot (shot, snap, w, h);
  g_autoptr (GskRenderNode) node = gtk_snapshot_free_to_node (g_steal_pointer (&snap));
  /* Cairo renderer: renders text reliably off screen. */
  g_autoptr (GskRenderer) renderer = gsk_cairo_renderer_new ();
  if (!gsk_renderer_realize_for_display (renderer, gtk_widget_get_display (window), NULL))
    g_clear_object (&renderer);
  int ok = 1;
  if (node == NULL || renderer == NULL)
    g_printerr ("nothing to render: node=%p renderer=%p size=%dx%d\n", (void *) node, (void *) renderer, w, h);
  if (node != NULL && renderer != NULL)
    {
      g_autoptr (GdkTexture) tex = gsk_renderer_render_texture (renderer, node, &GRAPHENE_RECT_INIT (0, 0, w, h));
      ok = gdk_texture_save_to_png (tex, out) ? 0 : 1;
      gsk_renderer_unrealize (renderer);
    }
  gtk_window_destroy (GTK_WINDOW (window));
  return done ? ok : 2;
}

static int
stress (guint rounds, int argc, char **argv)
{
  g_autoptr (GPtrArray) files = files_from_args (argc, argv);
  for (guint r = 0; r < rounds; r++)
    {
      GtkWidget *window = gtk_window_new ();
      GtkWidget *pane = dp_pane_new ();
      gtk_window_set_child (GTK_WINDOW (window), pane);
      gtk_window_present (GTK_WINDOW (window));
      for (guint i = 0; i < files->len; i++)
        {
          g_autoptr (GPtrArray) one = g_ptr_array_new ();
          g_ptr_array_add (one, g_ptr_array_index (files, i));
          dp_pane_show (DP_PANE (pane), NULL, one, 1);
          /* Sometimes switch before the reads finish, sometimes after. */
          if ((r + i) % 2 == 0)
            {
              wait_idle (DP_PANE (pane), 5000);
              if (dp_pane_play (DP_PANE (pane)))
                for (int k = 0; k < 200; k++)
                  g_main_context_iteration (NULL, FALSE);
            }
        }
      dp_pane_show (DP_PANE (pane), NULL, files, files->len);
      if (r % 3 == 0)
        wait_idle (DP_PANE (pane), 5000);
      gtk_window_destroy (GTK_WINDOW (window)); /* reads may still be running */
      for (int k = 0; k < 20; k++)
        g_main_context_iteration (NULL, FALSE);
    }
  /* Let cancelled reads finish before exiting. */
  for (int k = 0; k < 500; k++)
    {
      g_main_context_iteration (NULL, FALSE);
      g_usleep (2000);
    }
  dp_pane_unload_style ();
  g_print ("stress: %u rounds over %u files\n", rounds, files->len);
  return 0;
}

/* Interactive window ------------------------------------------------------------------------------ */

static void
on_selection (GtkSelectionModel *model, guint position, guint n, gpointer data)
{
  (void) position;
  (void) n;
  DpPane *pane = data;
  g_autoptr (GPtrArray) files = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr (GtkBitset) set = gtk_selection_model_get_selection (model);
  GtkBitsetIter iter;
  guint i;
  for (gboolean ok = gtk_bitset_iter_init_first (&iter, set, &i); ok; ok = gtk_bitset_iter_next (&iter, &i))
    {
      g_autoptr (GFileInfo) fi = g_list_model_get_item (G_LIST_MODEL (model), i);
      g_ptr_array_add (files, g_object_ref (G_FILE (g_file_info_get_attribute_object (fi, "standard::file"))));
    }
  GFile *dir = g_object_get_data (G_OBJECT (pane), "dir");
  dp_pane_show (pane, dir, files, files->len);
}

static void
setup_item (GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
  (void) f;
  (void) data;
  GtkWidget *l = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  gtk_list_item_set_child (item, l);
}

static void
bind_item (GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
  (void) f;
  (void) data;
  GFileInfo *fi = gtk_list_item_get_item (item);
  gtk_label_set_text (GTK_LABEL (gtk_list_item_get_child (item)), g_file_info_get_display_name (fi));
}

static void
on_activate (GtkApplication *app, gpointer data)
{
  const char *dir_arg = data;
  GtkWidget *window = adw_application_window_new (app);
  gtk_window_set_default_size (GTK_WINDOW (window), 900, 700);
  gtk_window_set_title (GTK_WINDOW (window), "Details Pane Demo");
  GtkWidget *view = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (view), adw_header_bar_new ());
  GtkWidget *paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
  GFile *dir = g_file_new_for_commandline_arg (dir_arg);
  GtkDirectoryList *list = gtk_directory_list_new ("standard::*", dir);
  GtkMultiSelection *sel = gtk_multi_selection_new (G_LIST_MODEL (list));
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup", G_CALLBACK (setup_item), NULL);
  g_signal_connect (factory, "bind", G_CALLBACK (bind_item), NULL);
  GtkWidget *lv = gtk_list_view_new (GTK_SELECTION_MODEL (sel), factory);
  GtkWidget *sw = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sw), lv);
  GtkWidget *pane = dp_pane_new ();
  g_object_set_data_full (G_OBJECT (pane), "dir", dir, g_object_unref);
  g_signal_connect (sel, "selection-changed", G_CALLBACK (on_selection), pane);
  gtk_paned_set_start_child (GTK_PANED (paned), sw);
  gtk_paned_set_end_child (GTK_PANED (paned), pane);
  gtk_paned_set_resize_end_child (GTK_PANED (paned), FALSE);
  gtk_paned_set_shrink_end_child (GTK_PANED (paned), FALSE);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (view), paned);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (window), view);
  dp_pane_show (DP_PANE (pane), dir, NULL, 0);
  gtk_window_present (GTK_WINDOW (window));
}

int
main (int argc, char **argv)
{
  if (argc >= 3 && g_str_equal (argv[1], "--png"))
    {
      adw_init ();
      return render_png (argv[2], argc - 3, argv + 3);
    }
  if (argc >= 3 && g_str_equal (argv[1], "--stress"))
    {
      adw_init ();
      return stress ((guint) g_ascii_strtoull (argv[2], NULL, 10), argc - 3, argv + 3);
    }
  g_autoptr (AdwApplication) app = adw_application_new ("io.github.zyrotec.NautilusUtils.DetailsPaneDemo",
                                                        G_APPLICATION_NON_UNIQUE);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), argc > 1 ? argv[1] : (char *) ".");
  char *args[] = { argv[0], NULL };
  return g_application_run (G_APPLICATION (app), 1, args);
}
