/* Shared content column. See nu_content.h. */
#include "nu_content.h"

#include <adwaita.h>

#define COLUMN_KEY "nu-content-column" /* on the tab view: its column (borrowed) */
#define USERS_KEY  "nu-content-users"  /* on the column: how many acquires */
#define STATE_KEY  "nu-content-state"  /* on the column: its Shadow state (owned) */

/* The column is [GtkOverlay (Files' tab view + our scroll shadow)][bottom bars…]. */
typedef struct
{
  GtkWidget *column;      /* borrowed: we are its data */
  GtkWidget *overlay;     /* borrowed: child of the column */
  GtkWidget *shadow;      /* borrowed: overlay child */
  GtkWidget *tab_view;    /* borrowed: the overlay's child */
  GWeakRef   scrolled;    /* the folder view's GtkScrolledWindow we watch */
  GtkAdjustment *adj;     /* its vadjustment (ref) */
  gulong     adj_changed_id, adj_value_id, unmap_id, page_id, map_id;
  GPtrArray *below;       /* GWeakRef* of tracked widgets */
  guint      rescan_id, update_id;
  guint      rescan_tries;
} Shadow;

static GtkCssProvider *css;

static const char style_sheet[] =
  /* libadwaita's undershoot.top (a line and a 4 px gradient), upside down. */
  ".nu-scroll-shadow { min-height: 6px; background: linear-gradient(to top, color-mix(in srgb, var(--shade-color) 75%, "
  "transparent), transparent 4px); box-shadow: inset 0 -1px color-mix(in srgb, var(--shade-color) 75%, transparent); }\n"
  "@media (prefers-contrast: more) { .nu-scroll-shadow { background: none; } }\n";

static void
ensure_style (void)
{
  if (css != NULL || gdk_display_get_default () == NULL)
    return;
  css = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (css, style_sheet);
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static void schedule_update (Shadow *sh);
static void schedule_rescan (Shadow *sh);

static void
unwatch (Shadow *sh)
{
  g_autoptr (GtkWidget) sw = g_weak_ref_get (&sh->scrolled);
  if (sw != NULL)
    g_clear_signal_handler (&sh->unmap_id, sw);
  sh->unmap_id = 0;
  if (sh->adj != NULL)
    {
      g_clear_signal_handler (&sh->adj_changed_id, sh->adj);
      g_clear_signal_handler (&sh->adj_value_id, sh->adj);
      g_clear_object (&sh->adj);
    }
  g_weak_ref_set (&sh->scrolled, NULL);
}

static void
shadow_free (gpointer data)
{
  Shadow *sh = data;
  g_clear_handle_id (&sh->rescan_id, g_source_remove);
  g_clear_handle_id (&sh->update_id, g_source_remove);
  unwatch (sh);
  g_weak_ref_clear (&sh->scrolled);
  /* Weak pointers: when the window closes, the overlay or Files' tab view may be gone before the column. */
  if (sh->tab_view != NULL)
    {
      g_clear_signal_handler (&sh->page_id, sh->tab_view);
      g_object_remove_weak_pointer (G_OBJECT (sh->tab_view), (gpointer *) &sh->tab_view);
    }
  if (sh->overlay != NULL)
    {
      g_clear_signal_handler (&sh->map_id, sh->overlay);
      g_object_remove_weak_pointer (G_OBJECT (sh->overlay), (gpointer *) &sh->overlay);
    }
  g_ptr_array_unref (sh->below);
  g_free (sh);
}

static void
weak_ref_free (gpointer data)
{
  g_weak_ref_clear (data);
  g_free (data);
}

/* The folder view's scroller: the biggest mapped GtkScrolledWindow in the tab shown. */
static void
find_scrolled (GtkWidget *w, GtkWidget **best, int *best_area)
{
  if (!gtk_widget_get_mapped (w))
    return;
  if (GTK_IS_SCROLLED_WINDOW (w))
    {
      int area = gtk_widget_get_width (w) * gtk_widget_get_height (w);
      if (area > *best_area)
        {
          *best = w;
          *best_area = area;
        }
      return;
    }
  for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL; c = gtk_widget_get_next_sibling (c))
    find_scrolled (c, best, best_area);
}

/* A tracked widget directly below the view (its top edge at the view's bottom, overlapping it sideways). */
static gboolean
something_below (Shadow *sh)
{
  graphene_rect_t view;
  if (!gtk_widget_compute_bounds (sh->overlay, GTK_WIDGET (gtk_widget_get_root (sh->overlay)), &view))
    return FALSE;
  for (guint i = 0; i < sh->below->len; i++)
    {
      g_autoptr (GtkWidget) w = g_weak_ref_get (g_ptr_array_index (sh->below, i));
      graphene_rect_t b;
      if (w == NULL || !gtk_widget_get_mapped (w) || gtk_widget_get_height (w) <= 0 ||
          gtk_widget_get_root (w) != gtk_widget_get_root (sh->overlay) ||
          !gtk_widget_compute_bounds (w, GTK_WIDGET (gtk_widget_get_root (w)), &b))
        continue;
      double gap = b.origin.y - (view.origin.y + view.size.height);
      gboolean overlaps = b.origin.x < view.origin.x + view.size.width && b.origin.x + b.size.width > view.origin.x;
      if (gap > -2 && gap < 12 && overlaps) /* a paned handle or a margin may sit in between */
        return TRUE;
    }
  return FALSE;
}

static gboolean
update_now (gpointer data)
{
  Shadow *sh = data;
  sh->update_id = 0;
  if (sh->overlay == NULL || sh->shadow == NULL)
    return G_SOURCE_REMOVE;
  gboolean more = sh->adj != NULL &&
                  gtk_adjustment_get_value (sh->adj) + gtk_adjustment_get_page_size (sh->adj) <
                    gtk_adjustment_get_upper (sh->adj) - 0.5;
  gtk_widget_set_visible (sh->shadow, more && something_below (sh));
  return G_SOURCE_REMOVE;
}

static void
schedule_update (Shadow *sh)
{
  if (sh->update_id == 0)
    sh->update_id = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, update_now, sh, NULL);
}

static void
on_adj (GtkAdjustment *adj, gpointer data)
{
  (void) adj;
  /* Value changes while scrolling: decide now, so the shadow never lags a frame behind. */
  Shadow *sh = data;
  g_clear_handle_id (&sh->update_id, g_source_remove);
  update_now (sh);
}

static void
on_scrolled_unmap (GtkWidget *sw, gpointer data)
{
  (void) sw;
  schedule_rescan (data); /* the view was replaced (list ↔ grid) or the tab changed */
}

static gboolean
rescan_now (gpointer data)
{
  Shadow *sh = data;
  sh->rescan_id = 0;
  if (sh->tab_view == NULL || sh->overlay == NULL)
    return G_SOURCE_REMOVE;
  GtkWidget *best = NULL;
  int area = 0;
  find_scrolled (sh->tab_view, &best, &area);
  g_autoptr (GtkWidget) current = g_weak_ref_get (&sh->scrolled);
  if (best != current)
    {
      unwatch (sh);
      if (best != NULL)
        {
          g_weak_ref_set (&sh->scrolled, best);
          sh->unmap_id = g_signal_connect (best, "unmap", G_CALLBACK (on_scrolled_unmap), sh);
          sh->adj = g_object_ref (gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (best)));
          sh->adj_changed_id = g_signal_connect (sh->adj, "changed", G_CALLBACK (on_adj), sh);
          sh->adj_value_id = g_signal_connect (sh->adj, "value-changed", G_CALLBACK (on_adj), sh);
        }
    }
  update_now (sh);
  /* The new view may not be mapped yet: look again a few times. */
  if (best == NULL && sh->rescan_tries++ < 20)
    sh->rescan_id = g_timeout_add (100, rescan_now, sh);
  return G_SOURCE_REMOVE;
}

static void
schedule_rescan (Shadow *sh)
{
  sh->rescan_tries = 0;
  if (sh->rescan_id == 0)
    sh->rescan_id = g_timeout_add (30, rescan_now, sh);
}

static void
on_selected_page (GObject *tab_view, GParamSpec *pspec, gpointer data)
{
  (void) tab_view, (void) pspec;
  schedule_rescan (data);
}

static void
on_overlay_map (GtkWidget *overlay, gpointer data)
{
  (void) overlay;
  schedule_rescan (data);
}

/* Puts @widget where @old is (toolbar view content or a paned child). Both must be ref'd by the caller. */
static gboolean
replace (GtkWidget *old, GtkWidget *widget)
{
  GtkWidget *parent = gtk_widget_get_parent (old);
  if (ADW_IS_TOOLBAR_VIEW (parent) && adw_toolbar_view_get_content (ADW_TOOLBAR_VIEW (parent)) == old)
    {
      adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (parent), NULL);
      if (gtk_widget_get_parent (widget) != NULL)
        gtk_widget_unparent (widget);
      adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (parent), widget);
      return TRUE;
    }
  if (GTK_IS_PANED (parent))
    {
      gboolean start = gtk_paned_get_start_child (GTK_PANED (parent)) == old;
      if (start)
        gtk_paned_set_start_child (GTK_PANED (parent), NULL);
      else
        gtk_paned_set_end_child (GTK_PANED (parent), NULL);
      if (gtk_widget_get_parent (widget) != NULL)
        gtk_widget_unparent (widget);
      if (start)
        gtk_paned_set_start_child (GTK_PANED (parent), widget);
      else
        gtk_paned_set_end_child (GTK_PANED (parent), widget);
      return TRUE;
    }
  return FALSE;
}

GtkWidget *
nu_content_acquire (GtkWidget *tab_view)
{
  g_return_val_if_fail (GTK_IS_WIDGET (tab_view), NULL);
  GtkWidget *column = g_object_get_data (G_OBJECT (tab_view), COLUMN_KEY);
  if (column == NULL)
    {
      GtkWidget *parent = gtk_widget_get_parent (tab_view);
      if (!ADW_IS_TOOLBAR_VIEW (parent) && !GTK_IS_PANED (parent))
        return NULL;
      ensure_style ();
      column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      gtk_widget_add_css_class (column, "nu-content");
      g_object_ref_sink (column);
      g_object_ref (tab_view);
      if (!replace (tab_view, column))
        {
          g_object_unref (tab_view);
          g_object_unref (column);
          return NULL;
        }
      GtkWidget *overlay = gtk_overlay_new ();
      gtk_widget_set_vexpand (overlay, TRUE);
      gtk_overlay_set_child (GTK_OVERLAY (overlay), tab_view);
      GtkWidget *shadow = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_add_css_class (shadow, "nu-scroll-shadow");
      gtk_widget_set_valign (shadow, GTK_ALIGN_END);
      gtk_widget_set_can_target (shadow, FALSE);
      gtk_widget_set_can_focus (shadow, FALSE);
      gtk_widget_set_visible (shadow, FALSE);
      gtk_overlay_add_overlay (GTK_OVERLAY (overlay), shadow);
      gtk_widget_set_vexpand (tab_view, TRUE);
      gtk_box_append (GTK_BOX (column), overlay);
      g_object_unref (tab_view);
      g_object_unref (column); /* now owned by its parent */
      g_object_set_data (G_OBJECT (tab_view), COLUMN_KEY, column);
      g_object_set_data (G_OBJECT (column), "nu-content-tab-view", tab_view);

      Shadow *sh = g_new0 (Shadow, 1);
      sh->column = column;
      sh->overlay = overlay;
      sh->shadow = shadow;
      sh->tab_view = tab_view;
      g_object_add_weak_pointer (G_OBJECT (overlay), (gpointer *) &sh->overlay);
      g_object_add_weak_pointer (G_OBJECT (tab_view), (gpointer *) &sh->tab_view);
      g_weak_ref_init (&sh->scrolled, NULL);
      sh->below = g_ptr_array_new_with_free_func (weak_ref_free);
      if (g_object_class_find_property (G_OBJECT_GET_CLASS (tab_view), "selected-page") != NULL)
        sh->page_id = g_signal_connect (tab_view, "notify::selected-page", G_CALLBACK (on_selected_page), sh);
      sh->map_id = g_signal_connect (overlay, "map", G_CALLBACK (on_overlay_map), sh);
      g_object_set_data_full (G_OBJECT (column), STATE_KEY, sh, shadow_free);
      schedule_rescan (sh);
    }
  guint users = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (column), USERS_KEY));
  g_object_set_data (G_OBJECT (column), USERS_KEY, GUINT_TO_POINTER (users + 1));
  return column;
}

void
nu_content_release (GtkWidget *column)
{
  g_return_if_fail (GTK_IS_BOX (column));
  guint users = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (column), USERS_KEY));
  g_return_if_fail (users > 0);
  g_object_set_data (G_OBJECT (column), USERS_KEY, GUINT_TO_POINTER (users - 1));
  if (users > 1)
    return;
  GtkWidget *tab_view = g_object_get_data (G_OBJECT (column), "nu-content-tab-view");
  Shadow *sh = g_object_get_data (G_OBJECT (column), STATE_KEY);
  if (tab_view == NULL || sh == NULL || sh->overlay == NULL || gtk_widget_get_parent (tab_view) != sh->overlay)
    return;
  g_object_ref (column);
  g_object_ref (tab_view);
  g_object_set_data (G_OBJECT (column), STATE_KEY, NULL); /* stops watching (the tab view stays) */
  gtk_overlay_set_child (GTK_OVERLAY (gtk_widget_get_parent (tab_view)), NULL);
  g_object_set_data (G_OBJECT (tab_view), COLUMN_KEY, NULL);
  if (!replace (column, tab_view))
    g_warning ("content column: could not put Files' tab view back");
  g_object_unref (tab_view);
  g_object_unref (column);
}

void
nu_content_add_bottom (GtkWidget *column, GtkWidget *bar)
{
  g_return_if_fail (GTK_IS_BOX (column) && GTK_IS_WIDGET (bar));
  gtk_box_append (GTK_BOX (column), bar);
}

static void
on_tracked_changed (GtkWidget *widget, gpointer data)
{
  (void) widget;
  GtkWidget *column = data;
  nu_content_refresh (column);
}

void
nu_content_track_below (GtkWidget *column, GtkWidget *widget)
{
  g_return_if_fail (GTK_IS_BOX (column) && GTK_IS_WIDGET (widget));
  Shadow *sh = g_object_get_data (G_OBJECT (column), STATE_KEY);
  if (sh == NULL)
    return;
  for (guint i = 0; i < sh->below->len; i++)
    {
      g_autoptr (GtkWidget) w = g_weak_ref_get (g_ptr_array_index (sh->below, i));
      if (w == widget)
        return;
    }
  GWeakRef *r = g_new0 (GWeakRef, 1);
  g_weak_ref_init (r, widget);
  g_ptr_array_add (sh->below, r);
  /* Shown, hidden, moved or resized (a paned drag): look again. */
  g_signal_connect_object (widget, "map", G_CALLBACK (on_tracked_changed), column, 0);
  g_signal_connect_object (widget, "unmap", G_CALLBACK (on_tracked_changed), column, 0);
  schedule_update (sh);
}

void
nu_content_refresh (GtkWidget *column)
{
  g_return_if_fail (GTK_IS_WIDGET (column));
  Shadow *sh = g_object_get_data (G_OBJECT (column), STATE_KEY);
  if (sh != NULL)
    schedule_update (sh);
}

gboolean
nu_content_shadow_shown (GtkWidget *column)
{
  Shadow *sh = column != NULL ? g_object_get_data (G_OBJECT (column), STATE_KEY) : NULL;
  return sh != NULL && sh->overlay != NULL && gtk_widget_get_visible (sh->shadow);
}
