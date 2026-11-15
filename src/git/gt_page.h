/* Git: the details pane's Git view (PLAN.md §14.2 item 4), plugged into the pane's extra-view slot
 * (dp_pane_set_extra): the repository (name, location, branch, upstream and counts, last commit) and what is
 * selected (its Git status and last commit; several items: how many of each state). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* DpExtraFunc: the page for @folder / @files, or NULL outside a Git repository (or for non-native locations). */
GtkWidget *gt_page_new (GFile     *folder,
                        GPtrArray *files,
                        gpointer   user_data);

/* One line describing the page (debug driver). Caller frees. */
char *gt_page_describe (GtkWidget *page);

G_END_DECLS
