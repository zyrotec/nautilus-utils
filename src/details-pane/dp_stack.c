/* Details pane stack. See dp_stack.h. */
#include "dp_stack.h"

#include "dp_thumb.h"

typedef struct
{
  FpStyle     style;
  guint       n;
  guint       width;
  char       *uri[DP_STACK_MAX];      /* Owned */
  guint64     mtime[DP_STACK_MAX];
  gboolean    thumbnail[DP_STACK_MAX];
  GdkTexture *fallback[DP_STACK_MAX]; /* Owned refs or NULL */
  guint       seed[DP_STACK_MAX];
} Job;

static void
job_free (gpointer data)
{
  Job *job = data;
  for (guint i = 0; i < MIN (job->n, DP_STACK_MAX); i++)
    {
      g_free (job->uri[i]);
      g_clear_object (&job->fallback[i]);
    }
  g_free (job);
}

void
dp_stack_style_init (FpStyle *style, int depth, gboolean shadow, gboolean rotation, gboolean keep_aspect, int fade)
{
  fp_style_init_default (style);
  style->layout = FP_LAYOUT_STACKED;
  style->border = FALSE; /* P10: border and corner radius are not options of the pane */
  style->depth = (FpDepth) CLAMP (depth, FP_DEPTH_NONE, FP_DEPTH_DARKEN);
  style->shadow = shadow;
  style->rotation = rotation;
  style->keep_aspect = keep_aspect;
  style->fade = (guint) CLAMP (fade, 0, 100);
}

static void
render_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  (void) source;
  Job *job = data;
  guint n = MIN (job->n, DP_STACK_MAX); /* bounded by dp_stack_render_async; explicit for -fanalyzer */
  char *paths[DP_STACK_MAX] = { NULL };
  FpCompositeItem items[DP_STACK_MAX] = { { NULL, 0, NULL } };
  for (guint i = 0; i < n; i++)
    {
      if (job->thumbnail[i] && !g_cancellable_is_cancelled (cancellable))
        paths[i] = dp_thumb_find_existing (job->uri[i], job->mtime[i]); /* owned, freed below */
      items[i].path = paths[i];
      items[i].texture = paths[i] == NULL ? job->fallback[i] : NULL;
      items[i].seed = job->seed[i];
    }
  GdkTexture *frame = NULL;
  if (!g_cancellable_is_cancelled (cancellable))
    frame = fp_composite_frame (&job->style, items, n, job->width, NULL);
  for (guint i = 0; i < n; i++)
    g_free (paths[i]);
  if (g_task_return_error_if_cancelled (task))
    {
      g_clear_object (&frame);
      return;
    }
  if (frame == NULL)
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "stack could not be drawn");
  else
    g_task_return_pointer (task, frame, g_object_unref);
}

void
dp_stack_render_async (const FpStyle       *style,
                       const DpStackItem   *items,
                       guint                n_items,
                       guint                width,
                       GCancellable        *cancellable,
                       GAsyncReadyCallback  callback,
                       gpointer             user_data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, dp_stack_render_async);
  Job *job = g_new0 (Job, 1); /* owned by the task */
  job->style = *style;
  job->style.layout = FP_LAYOUT_STACKED;
  job->n = MIN (n_items, DP_STACK_MAX);
  job->width = width;
  for (guint i = 0; i < job->n; i++)
    {
      job->uri[i] = g_strdup (items[i].uri);
      job->mtime[i] = items[i].mtime;
      job->thumbnail[i] = items[i].thumbnail && items[i].uri != NULL;
      job->fallback[i] = items[i].fallback != NULL ? g_object_ref (items[i].fallback) : NULL;
      job->seed[i] = items[i].seed;
    }
  g_task_set_task_data (task, job, job_free);
  g_task_run_in_thread (task, render_thread);
}

GdkTexture *
dp_stack_render_finish (GAsyncResult  *result,
                        GError       **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}
