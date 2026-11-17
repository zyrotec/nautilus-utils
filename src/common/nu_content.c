/* Shared content column. See nu_content.h. */
#include "nu_content.h"

#include <adwaita.h>

#define COLUMN_KEY "nu-content-column" /* on the tab view: its column (borrowed) */
#define USERS_KEY  "nu-content-users"  /* on the column: how many acquires */

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
      gtk_widget_set_vexpand (tab_view, TRUE);
      gtk_box_append (GTK_BOX (column), tab_view);
      g_object_unref (tab_view);
      g_object_unref (column); /* now owned by its parent */
      g_object_set_data (G_OBJECT (tab_view), COLUMN_KEY, column);
      g_object_set_data (G_OBJECT (column), "nu-content-tab-view", tab_view);
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
  if (tab_view == NULL || gtk_widget_get_parent (tab_view) != column)
    return;
  g_object_ref (column);
  g_object_ref (tab_view);
  gtk_box_remove (GTK_BOX (column), tab_view);
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
