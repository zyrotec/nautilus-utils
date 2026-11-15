/* Details pane thumbnails (PLAN.md §13.1.2 item 1): freedesktop thumbnails from the shared
 * ~/.cache/thumbnails, x-large (512 px) first, made in a worker if missing (so Files benefits too).
 * No Nautilus types. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Loads the best thumbnail of @uri (modified at @mtime) as a texture, in a worker. With @allow_generate,
 * a missing x-large thumbnail is made (an external, sandboxed thumbnailer); otherwise only existing x-large
 * or large ones are used. Not for non-native files (the caller decides, PLAN.md §13.3 rule 3). */
void        dp_thumb_load_async  (const char          *uri,
                                  const char          *content_type,
                                  guint64              mtime,
                                  gboolean             allow_generate,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data);

/* Caller owns the texture; NULL without @error = there is no thumbnail. */
GdkTexture *dp_thumb_load_finish (GAsyncResult  *result,
                                  GError       **error);

/* Worker threads only (takes the factory lock): path of an existing x-large or large thumbnail of @uri, or NULL.
 * Never generates. Caller owns the string. */
char *dp_thumb_find_existing (const char *uri,
                              guint64     mtime);

/* Drops the factories (running loads keep their own refs). Main thread. */
void dp_thumb_shutdown (void);

G_END_DECLS
