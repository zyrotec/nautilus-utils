/* The content column (PLAN.md §14.5 item 3): a vertical box [Files' tab view][bottom bars…] that features share, so
 * the details pane (which puts it in its paned) and the Git status bar (a bottom bar) work with and without each
 * other. Main thread. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* The column around @tab_view, made on the first acquire right where the tab view is (an AdwToolbarView's content
 * or a GtkPaned's child); NULL if the tab view is somewhere else. Borrowed; every acquire needs a release. */
GtkWidget *nu_content_acquire (GtkWidget *tab_view);

/* Drops one acquire; the last one puts the tab view back where the column is and destroys the column. */
void nu_content_release (GtkWidget *column);

/* Adds @bar (floating or not; the column takes a reference) below the tab view, after earlier bars. Removing a
 * bar: gtk_box_remove (column, bar). */
void nu_content_add_bottom (GtkWidget *column,
                            GtkWidget *bar);

/* The scroll shadow (like libadwaita's header bar undershoot, upside down): drawn at the bottom of the folder view
 * while it can scroll further down and a widget tracked here (the Git status bar, the details pane at the bottom)
 * sits right below the view. Tracked widgets are held weakly; tracking one twice is harmless. */
void nu_content_track_below (GtkWidget *column,
                             GtkWidget *widget);

/* Re-checks the shadow (after moving or showing a tracked widget). */
void nu_content_refresh (GtkWidget *column);

/* Whether the shadow is shown (debug driver and tests). */
gboolean nu_content_shadow_shown (GtkWidget *column);

G_END_DECLS
