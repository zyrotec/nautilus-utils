/* DpStackView: several selected items drawn as a stack of live layers (PLAN.md §13.14, P24–P27).
 *
 * Every layer is drawn each frame from its own paintable — the theme icon (vector), a folder's preview, or the
 * file's thumbnail — with GSK nodes for tilt, border, corners, shadow and depth, so nothing is rasterised
 * ahead and the stack stays sharp at any size and scale.
 *
 * Two shapes:
 *  - icons: the front icon at the icon size on the bottom edge; each one behind ×0.85 and raised so its top
 *    peeks out. Icons and folders are always drawn plain (no border, corners or box shadow).
 *  - cards: when a picture, video or PDF is among the items (and media aren't squared), the folder-preview
 *    stack: cards behind are narrower and raised. Only media cards get the border, corners and box shadow.
 * With the "square media" option, media are squares the size of the icons and the icon shape is used.
 *
 * Changes are animated: items that stay move to their new place, new ones fade in, removed ones fade out, a
 * thumbnail that arrives cross-fades over its icon. Plain GTK; no Nautilus types. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DP_STACK_VIEW_MAX 3 /* layers drawn (front first) */

#define DP_TYPE_STACK_VIEW (dp_stack_view_get_type ())
G_DECLARE_FINAL_TYPE (DpStackView, dp_stack_view, DP, STACK_VIEW, GtkWidget)

typedef struct
{
  int      icon_size;     /* px: the front icon */
  gboolean border;        /* media cards only */
  GdkRGBA  border_color;
  double   border_width;  /* px */
  int      corner_radius; /* px, media cards only */
  gboolean shadow;
  gboolean rotation;      /* tilt the layers behind the front one */
  gboolean keep_aspect;   /* media cards take their picture's shape (else the picture fills the card) */
  int      depth;         /* FpDepth: how layers further back look */
  int      fade;          /* % of the stack's height faded out at the bottom (0 = off) */
  gboolean card;          /* translucent card behind icons */
  gboolean media_square;  /* media as squares the size of the icons (P26) */
} DpStackStyle;

typedef struct
{
  const char   *key;   /* Borrowed: identifies the item across updates (its URI) */
  GdkPaintable *icon;  /* Borrowed (ref'd): its icon, or a folder's preview; may be NULL */
  gboolean      media; /* picture, video or PDF: a card that may get a thumbnail */
  guint         seed;  /* tilt */
} DpStackViewItem;

GtkWidget *dp_stack_view_new (void);

void dp_stack_view_set_style (DpStackView        *self,
                              const DpStackStyle *style);

/* Shows @items (front first; at most DP_STACK_VIEW_MAX are used), animated from what was shown. Items whose
 * key was already shown keep their thumbnail. n_items = 0 empties the stack. */
void dp_stack_view_set_items (DpStackView           *self,
                              const DpStackViewItem *items,
                              guint                  n_items);

/* The thumbnail of item @key arrived (borrowed; NULL = none): cross-faded over its icon. */
void dp_stack_view_set_thumbnail (DpStackView *self,
                                  const char  *key,
                                  GdkTexture  *thumbnail);

/* TRUE if item @key is shown with its thumbnail already. */
gboolean dp_stack_view_has_thumbnail (DpStackView *self,
                                      const char  *key);

/* The shape the box should give the stack for @n_items: width / height (> 0, cards) or 0 with the height in
 * *icon_height (icon shape: the height for the icon size, room for DP_STACK_VIEW_MAX layers). */
double dp_stack_view_get_ratio (DpStackView *self,
                                int         *icon_height);

/* For tests and the debug driver --------------------------------------------------------------- */

/* Layers shown (not counting ones fading out). */
guint dp_stack_view_get_n_items (DpStackView *self);

/* "icons" or "cards". */
const char *dp_stack_view_get_shape (DpStackView *self);

/* TRUE while something is animating. */
gboolean dp_stack_view_is_animating (DpStackView *self);

/* Thumbnails shown. */
guint dp_stack_view_get_n_thumbnails (DpStackView *self);

G_END_DECLS
