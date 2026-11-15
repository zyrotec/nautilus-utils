/* DpBox: the details pane's preview box (PLAN.md §13.12 P17). One child, sized two ways:
 *  - media (ratio > 0): the box takes the media's shape — as wide as it may be (right) or as tall as the strip
 *    (bottom), never taller than max-height — and the child gets exactly that rectangle, so a thumbnail fills it
 *    edge to edge;
 *  - icon (ratio = 0): a fixed-height box across the width (right) or a square (bottom).
 * Plain GTK. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DP_TYPE_BOX (dp_box_get_type ())
G_DECLARE_FINAL_TYPE (DpBox, dp_box, DP, BOX, GtkWidget)

/* A floating new box. */
GtkWidget *dp_box_new (void);

/* Takes a reference on @child (NULL removes it). */
void dp_box_set_child (DpBox     *self,
                       GtkWidget *child);

/* Media: width / height of what is shown (> 0). Icon: 0. */
void dp_box_set_ratio (DpBox  *self,
                       double  ratio);

/* Icon mode height (right layout). */
void dp_box_set_icon_height (DpBox *self,
                             int    height);

/* Media mode: the tallest the box gets. */
void dp_box_set_max_height (DpBox *self,
                            int    height);

/* VERTICAL: right of the view (width given, height follows); HORIZONTAL: bottom strip (height given). */
void dp_box_set_layout (DpBox          *self,
                        GtkOrientation  layout);

/* TRUE in media mode (ratio > 0). */
gboolean dp_box_get_ratio_is_media (DpBox *self);

/* For tests: the child's rectangle after the last allocation. */
void dp_box_get_child_rect (DpBox *self,
                            int   *x,
                            int   *y,
                            int   *width,
                            int   *height);

G_END_DECLS
