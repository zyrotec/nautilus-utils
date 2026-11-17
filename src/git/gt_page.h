/* Git: the details pane's Git view (PLAN.md §14.2 item 4), plugged into the pane's extra-view slot
 * (dp_pane_set_extra): a branch card instead of the preview, and the history (with a graph, or a plain list)
 * instead of the details (PLAN.md §14.11). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* DpExtraAvailableFunc: whether what is shown is inside a Git repository (native locations; stat only). */
gboolean gt_page_available (GFile     *folder,
                            GPtrArray *files,
                            gpointer   user_data);

/* DpExtraFunc: the page (the history) for @folder / @files and in *@header the branch card, or NULL outside a Git
 * repository. @current, if it is a page of the same repository (and look), is kept: only its selection line is read
 * again. @user_data: GINT_TO_POINTER (TRUE) for the history with its graph. */
GtkWidget *gt_page_new (GFile      *folder,
                        GPtrArray  *files,
                        GtkWidget  *current,
                        GtkWidget **header,
                        gpointer    user_data);

/* Opens the hover card of commit @index (debug driver). FALSE if there is no such commit. */
gboolean gt_page_open_card (GtkWidget *page,
                            guint      index);

/* One line describing the page (debug driver). Caller frees. */
char *gt_page_describe (GtkWidget *page);

G_END_DECLS
