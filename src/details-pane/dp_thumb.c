/* Details pane thumbnails. See dp_thumb.h. */
#define GNOME_DESKTOP_USE_UNSTABLE_API
#include "dp_thumb.h"

#include <libgnome-desktop/gnome-desktop-thumbnail.h>

/* GnomeDesktopThumbnailFactory isn't documented as thread-safe: every use is serialised by this lock
 * (thumbnails for the pane are rare and debounced; folder-preview has its own factories). */
static GMutex lock;
static GnomeDesktopThumbnailFactory *xlarge; /* Owned, under @lock; created on first use. */
static GnomeDesktopThumbnailFactory *large;  /* Owned, under @lock. */

typedef struct
{
  char    *uri;          /* Owned. */
  char    *content_type; /* Owned or NULL. */
  time_t   mtime;
  gboolean allow_generate;
} Job;

static void
job_free (gpointer data)
{
  Job *job = data;
  g_free (job->uri);
  g_free (job->content_type);
  g_free (job);
}

/* Worker: path of the best thumbnail (new string) or NULL. Must hold @lock. */
static char *
best_path (Job *job, GCancellable *cancellable)
{
  if (xlarge == NULL)
    xlarge = gnome_desktop_thumbnail_factory_new (GNOME_DESKTOP_THUMBNAIL_SIZE_XLARGE);
  if (large == NULL)
    large = gnome_desktop_thumbnail_factory_new (GNOME_DESKTOP_THUMBNAIL_SIZE_LARGE);
  char *path = gnome_desktop_thumbnail_factory_lookup (xlarge, job->uri, job->mtime);
  if (path != NULL)
    return path;
  if (job->allow_generate && job->content_type != NULL &&
      !gnome_desktop_thumbnail_factory_has_valid_failed_thumbnail (xlarge, job->uri, job->mtime) &&
      gnome_desktop_thumbnail_factory_can_thumbnail (xlarge, job->uri, job->content_type, job->mtime))
    {
      g_autoptr (GError) error = NULL;
      g_autoptr (GdkPixbuf) pixbuf = gnome_desktop_thumbnail_factory_generate_thumbnail (
        xlarge, job->uri, job->content_type, cancellable, &error);
      if (pixbuf != NULL)
        {
          if (gnome_desktop_thumbnail_factory_save_thumbnail (xlarge, pixbuf, job->uri, job->mtime, NULL, NULL))
            return gnome_desktop_thumbnail_path_for_uri (job->uri, GNOME_DESKTOP_THUMBNAIL_SIZE_XLARGE);
        }
      else if (!g_cancellable_is_cancelled (cancellable))
        {
          g_debug ("no thumbnail for %s: %s", job->uri, error != NULL ? error->message : "unknown error");
          gnome_desktop_thumbnail_factory_create_failed_thumbnail (xlarge, job->uri, job->mtime, NULL, NULL);
        }
    }
  return gnome_desktop_thumbnail_factory_lookup (large, job->uri, job->mtime);
}

static void
load_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  (void) source;
  g_autofree char *path = NULL;
  g_mutex_lock (&lock);
  path = best_path (data, cancellable);
  g_mutex_unlock (&lock);
  if (g_task_return_error_if_cancelled (task))
    return;
  GdkTexture *texture = path != NULL ? gdk_texture_new_from_filename (path, NULL) : NULL; /* thread-safe */
  g_task_return_pointer (task, texture, texture != NULL ? g_object_unref : NULL);
}

void
dp_thumb_load_async (const char          *uri,
                     const char          *content_type,
                     guint64              mtime,
                     gboolean             allow_generate,
                     GCancellable        *cancellable,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, dp_thumb_load_async);
  Job *job = g_new0 (Job, 1);
  job->uri = g_strdup (uri);
  job->content_type = g_strdup (content_type);
  job->mtime = (time_t) mtime;
  job->allow_generate = allow_generate;
  g_task_set_task_data (task, job, job_free);
  g_task_run_in_thread (task, load_thread);
}

GdkTexture *
dp_thumb_load_finish (GAsyncResult  *result,
                      GError       **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

char *
dp_thumb_find_existing (const char *uri,
                        guint64     mtime)
{
  Job job = { .uri = (char *) uri, .content_type = NULL, .mtime = (time_t) mtime, .allow_generate = FALSE };
  g_mutex_lock (&lock);
  char *path = best_path (&job, NULL); /* allow_generate FALSE: lookups only */
  g_mutex_unlock (&lock);
  return path;
}

void
dp_thumb_shutdown (void)
{
  /* A load still running holds @lock while it uses the factories, so this waits for it. */
  g_mutex_lock (&lock);
  g_clear_object (&xlarge);
  g_clear_object (&large);
  g_mutex_unlock (&lock);
}
