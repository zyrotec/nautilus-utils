/* Details pane: several selected items drawn as the folder-preview stack (PLAN.md §13.10, P10–P11).
 * No Nautilus types. */
#pragma once

#include <gtk/gtk.h>

#include "composite.h"

G_BEGIN_DECLS

#define DP_STACK_MAX 3 /* = the folder preview's STACK_CARDS */

typedef struct
{
  const char *uri;      /* Borrowed (copied by the async call): its existing thumbnail is used if there is one */
  guint64     mtime;
  gboolean    thumbnail; /* look for a thumbnail (native files only, PLAN.md §13.3 rule 3) */
  GdkTexture *fallback; /* Borrowed (ref'd by the async call): drawn when there is no thumbnail, or NULL */
  guint       seed;     /* tilt */
} DpStackItem;

/* Renders up to DP_STACK_MAX @items (front first) as a stacked frame @width device pixels wide with @style
 * (its layout is forced to stacked) in a worker. */
void        dp_stack_render_async  (const FpStyle       *style,
                                    const DpStackItem   *items,
                                    guint                n_items,
                                    guint                width,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data);

/* Caller owns the texture; NULL with @error on cancellation or if nothing could be drawn. */
GdkTexture *dp_stack_render_finish (GAsyncResult  *result,
                                    GError       **error);

/* The pane's stack style from its options: the folder-preview defaults, stacked, no border (P10). */
void dp_stack_style_init (FpStyle  *style,
                          int       depth,
                          gboolean  shadow,
                          gboolean  rotation,
                          gboolean  keep_aspect,
                          int       fade);

G_END_DECLS
