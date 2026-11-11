/* Tests for src/sidebar-drives/drive_info.c (PLAN.md §8.5 Phase 1). */
#include <gio/gio.h>

#include "config.h"
#include "drive_info.h"

/* ------------------------------------------------------------------------------------------------------ */
/* SlowFile: a GFile whose filesystem query completes after a delay — a stand-in for a slow or dead        */
/* network server. Only query_filesystem_info_async/finish are implemented.                               */
/* ------------------------------------------------------------------------------------------------------ */

#define SLOW_TYPE_FILE (slow_file_get_type ())
G_DECLARE_FINAL_TYPE (SlowFile, slow_file, SLOW, FILE, GObject)

struct _SlowFile
{
  GObject  parent_instance;
  guint    delay_ms;         /* When the query completes on its own */
  gboolean honor_cancel;     /* Complete with G_IO_ERROR_CANCELLED as soon as the cancellable fires */
  gboolean report_size;      /* Include filesystem::size/free in the result */
  guint    n_completed;      /* Times a query actually returned (late results included) */
};

static void slow_file_iface_init (GFileIface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (SlowFile, slow_file, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (G_TYPE_FILE, slow_file_iface_init))

typedef struct
{
  GSource      *timer;    /* Owned ref while pending */
  GCancellable *cancel;   /* Owned ref or NULL */
  gulong        handler;
  gboolean      done;
} SlowQuery;

static void
slow_query_free (SlowQuery *q)
{
  if (q->cancel != NULL && q->handler != 0)
    g_cancellable_disconnect (q->cancel, q->handler);
  g_clear_object (&q->cancel);
  if (q->timer != NULL)
    {
      g_source_destroy (q->timer);
      g_clear_pointer (&q->timer, g_source_unref);
    }
  g_free (q);
}

/* Completes @task once (owned reference, dropped here). */
static void
slow_finish_task (GTask *task, gboolean cancelled)
{
  SlowQuery *q = g_task_get_task_data (task);
  SlowFile *self = SLOW_FILE (g_task_get_source_object (task));
  if (q->done)
    return;
  q->done = TRUE;
  self->n_completed++;
  if (cancelled)
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
  else
    {
      GFileInfo *info = g_file_info_new ();
      if (self->report_size)
        {
          g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE, 2000);
          g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, 500);
        }
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, TRUE);
      g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "slowfs");
      g_task_return_pointer (task, info, g_object_unref);
    }
  g_object_unref (task);
}

static gboolean
slow_on_timer (gpointer data)
{
  GTask *task = G_TASK (data);
  SlowQuery *q = g_task_get_task_data (task);
  g_clear_pointer (&q->timer, g_source_unref);
  slow_finish_task (task, FALSE);
  return G_SOURCE_REMOVE;
}

static gboolean
slow_on_cancel_idle (gpointer data)
{
  GTask *task = G_TASK (data);
  SlowQuery *q = g_task_get_task_data (task);
  if (q->timer != NULL)
    {
      g_source_destroy (q->timer);
      g_clear_pointer (&q->timer, g_source_unref);
    }
  slow_finish_task (task, TRUE);
  return G_SOURCE_REMOVE;
}

static void
slow_on_cancelled (GCancellable *c, gpointer data)
{
  (void) c;
  /* Tests cancel from the main thread; defer so we don't complete inside the cancel emission. */
  g_idle_add (slow_on_cancel_idle, data);
}

static void
slow_query_filesystem_info_async (GFile *file, const char *attributes, int io_priority,
                                  GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  (void) attributes; (void) io_priority;
  SlowFile *self = SLOW_FILE (file);
  GTask *task = g_task_new (file, cancellable, callback, user_data); /* dropped in slow_finish_task */
  SlowQuery *q = g_new0 (SlowQuery, 1);
  g_task_set_task_data (task, q, (GDestroyNotify) slow_query_free);
  q->timer = g_timeout_source_new (self->delay_ms);
  g_source_set_callback (q->timer, slow_on_timer, task, NULL);
  g_source_attach (q->timer, g_task_get_context (task));
  if (cancellable != NULL && self->honor_cancel)
    {
      q->cancel = g_object_ref (cancellable);
      q->handler = g_cancellable_connect (cancellable, G_CALLBACK (slow_on_cancelled), task, NULL);
    }
}

static GFileInfo *
slow_query_filesystem_info_finish (GFile *file, GAsyncResult *res, GError **error)
{
  (void) file;
  return g_task_propagate_pointer (G_TASK (res), error);
}

static void
slow_file_iface_init (GFileIface *iface)
{
  iface->query_filesystem_info_async = slow_query_filesystem_info_async;
  iface->query_filesystem_info_finish = slow_query_filesystem_info_finish;
}

static void slow_file_init (SlowFile *self) { (void) self; }
static void slow_file_class_init (SlowFileClass *klass) { (void) klass; }

static SlowFile *
slow_file_new (guint delay_ms, gboolean honor_cancel, gboolean report_size)
{
  SlowFile *f = g_object_new (SLOW_TYPE_FILE, NULL);
  f->delay_ms = delay_ms;
  f->honor_cancel = honor_cancel;
  f->report_size = report_size;
  return f;
}

/* ------------------------------------------------------------------------------------------------------ */
/* Helpers                                                                                                */
/* ------------------------------------------------------------------------------------------------------ */

typedef struct
{
  gboolean  done;
  guint     n_callbacks;
  SdUsage  *usage;   /* Owned */
  GError   *error;   /* Owned */
} Result;

static void
result_clear (Result *r)
{
  g_clear_pointer (&r->usage, sd_usage_free);
  g_clear_error (&r->error);
  r->done = FALSE;
  r->n_callbacks = 0;
}

static void
on_usage (GObject *source, GAsyncResult *res, gpointer data)
{
  Result *r = data;
  r->n_callbacks++;
  r->done = TRUE;
  r->usage = sd_query_usage_finish (G_FILE (source), res, &r->error);
}

static void
wait_for (gboolean *flag)
{
  while (!*flag)
    g_main_context_iteration (NULL, TRUE);
}

/* Runs the main loop for @ms more milliseconds (lets late results arrive). */
static void
spin_ms (guint ms)
{
  gint64 end = g_get_monotonic_time () + (gint64) ms * 1000;
  while (g_get_monotonic_time () < end)
    g_main_context_iteration (NULL, FALSE);
}

/* Runs the main loop until @f has completed @n queries (late results included); fails after 30 s. */
static void
wait_for_completed (SlowFile *f, guint n)
{
  gint64 end = g_get_monotonic_time () + 30 * G_USEC_PER_SEC;
  while (f->n_completed < n)
    {
      g_assert_cmpint (g_get_monotonic_time (), <, end);
      g_main_context_iteration (NULL, TRUE);
    }
  spin_ms (20);
  g_assert_cmpuint (f->n_completed, ==, n);
}

/* ------------------------------------------------------------------------------------------------------ */
/* Tests                                                                                                  */
/* ------------------------------------------------------------------------------------------------------ */

static void
test_os_name (void)
{
  g_autofree char *name = sd_os_name ();
  g_autofree char *pretty = g_get_os_info (G_OS_INFO_KEY_PRETTY_NAME);
  g_assert_nonnull (name);
  g_assert_cmpstr (name, !=, "");
  if (pretty != NULL && *pretty != '\0')
    g_assert_cmpstr (name, ==, g_strstrip (pretty));
  else
    g_assert_cmpstr (name, ==, SD_OS_FALLBACK_NAME);
}

static void
test_classify (void)
{
  g_assert_cmpint (sd_classify (TRUE, FALSE), ==, SD_DRIVE_LOCAL);    /* ext4, btrfs, ntfs3 under /mnt */
  g_assert_cmpint (sd_classify (TRUE, TRUE), ==, SD_DRIVE_NETWORK);   /* NFS / CIFS mounted by the kernel */
  g_assert_cmpint (sd_classify (FALSE, FALSE), ==, SD_DRIVE_NETWORK); /* smb://, sftp:// via gvfs */
  g_assert_cmpint (sd_classify (FALSE, TRUE), ==, SD_DRIVE_NETWORK);
}

static void
test_fill (void)
{
  g_assert_cmpfloat (sd_fill_fraction (0, 0), <, 0.0);
  g_assert_cmpint (sd_fill_state (sd_fill_fraction (0, 0)), ==, SD_FILL_UNKNOWN);
  g_assert_cmpfloat (sd_fill_fraction (100, 100), ==, 0.0);
  g_assert_cmpfloat (sd_fill_fraction (100, 250), ==, 0.0);      /* free > size is clamped */
  g_assert_cmpfloat (sd_fill_fraction (100, 0), ==, 1.0);
  g_assert_cmpfloat_with_epsilon (sd_fill_fraction (497300000000ULL, 445700000000ULL), 0.10376, 0.0001);

  g_assert_cmpint (sd_fill_state (0.0), ==, SD_FILL_NORMAL);
  g_assert_cmpint (sd_fill_state (0.8999), ==, SD_FILL_NORMAL);
  g_assert_cmpint (sd_fill_state (SD_WARN_FRACTION), ==, SD_FILL_WARNING);
  g_assert_cmpint (sd_fill_state (0.9499), ==, SD_FILL_WARNING);
  g_assert_cmpint (sd_fill_state (SD_FULL_FRACTION), ==, SD_FILL_FULL);
  g_assert_cmpint (sd_fill_state (1.0), ==, SD_FILL_FULL);
  /* Exact byte counts at the thresholds. */
  g_assert_cmpint (sd_fill_state (sd_fill_fraction (1000, 100)), ==, SD_FILL_WARNING);
  g_assert_cmpint (sd_fill_state (sd_fill_fraction (1000, 101)), ==, SD_FILL_NORMAL);
  g_assert_cmpint (sd_fill_state (sd_fill_fraction (1000, 50)), ==, SD_FILL_FULL);
}

static void
test_usage_text (void)
{
  /* g_format_size: one decimal, SI units, U+00A0 before the unit (image 3 of the owner's request). */
  g_autofree char *a = sd_usage_text (497300000000ULL, 445700000000ULL);
  g_assert_cmpstr (a, ==, "445.7 GB free of 497.3 GB");
  g_autofree char *b = sd_usage_text (1000204886016ULL, 803200000000ULL);
  g_assert_cmpstr (b, ==, "803.2 GB free of 1.0 TB");
  g_autofree char *c = sd_usage_text (100, 250); /* free clamped to size; GLib uses a plain space for "bytes" */
  g_assert_cmpstr (c, ==, "100 bytes free of 100 bytes");
}

static void
test_query_real (void)
{
  g_autoptr(GFile) dir = g_file_new_for_path (g_get_tmp_dir ());
  Result r = { 0 };
  /* No timeout: this checks the real query (timeouts have their own tests); under valgrind it can be slow. */
  sd_query_usage_async (dir, 0, NULL, on_usage, &r);
  g_assert_false (r.done); /* never completes synchronously */
  wait_for (&r.done);
  g_assert_no_error (r.error);
  g_assert_nonnull (r.usage);
  g_assert_true (r.usage->known);
  g_assert_cmpuint (r.usage->size, >, 0);
  g_assert_cmpuint (r.usage->free, <=, r.usage->size);
  g_assert_nonnull (r.usage->fs_type);
  g_assert_cmpint (sd_classify (TRUE, r.usage->remote), ==, SD_DRIVE_LOCAL);
  spin_ms (50);
  g_assert_cmpuint (r.n_callbacks, ==, 1);
  result_clear (&r);
}

static void
test_query_unknown_size (void)
{
  g_autoptr(SlowFile) f = slow_file_new (10, TRUE, FALSE);
  Result r = { 0 };
  sd_query_usage_async (G_FILE (f), 1000, NULL, on_usage, &r);
  wait_for (&r.done);
  g_assert_no_error (r.error);
  g_assert_false (r.usage->known);
  g_assert_true (r.usage->remote);
  g_assert_cmpstr (r.usage->fs_type, ==, "slowfs");
  g_assert_cmpint (sd_classify (TRUE, r.usage->remote), ==, SD_DRIVE_NETWORK);
  result_clear (&r);
}

static void
test_query_slow_but_in_time (void)
{
  g_autoptr(SlowFile) f = slow_file_new (50, TRUE, TRUE);
  Result r = { 0 };
  sd_query_usage_async (G_FILE (f), 1000, NULL, on_usage, &r);
  wait_for (&r.done);
  g_assert_no_error (r.error);
  g_assert_true (r.usage->known);
  g_assert_cmpuint (r.usage->size, ==, 2000);
  g_assert_cmpuint (r.usage->free, ==, 500);
  spin_ms (100);
  g_assert_cmpuint (r.n_callbacks, ==, 1);
  result_clear (&r);
}

/* A dead server: the underlying query ignores cancellation and only returns much later. The caller must
 * get G_IO_ERROR_TIMED_OUT on time, exactly once, and the late result must be dropped safely. */
static void
test_query_timeout_dead_server (void)
{
  g_autoptr(SlowFile) f = slow_file_new (3000, FALSE, TRUE);
  Result r = { 0 };
  gint64 start = g_get_monotonic_time ();
  sd_query_usage_async (G_FILE (f), 100, NULL, on_usage, &r);
  wait_for (&r.done);
  gint64 elapsed_ms = (g_get_monotonic_time () - start) / 1000;
  g_assert_error (r.error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  g_assert_null (r.usage);
  g_assert_cmpint (elapsed_ms, >=, 90);
  g_assert_cmpint (elapsed_ms, <, 2500);  /* returned long before the backend (3 s), with valgrind margin */
  g_assert_cmpuint (f->n_completed, ==, 0);
  wait_for_completed (f, 1); /* the late result arrives and must be ignored */
  g_assert_cmpuint (r.n_callbacks, ==, 1);
  result_clear (&r);
}

/* Timeout with a backend that honours cancellation: one TIMED_OUT, backend cancelled. */
static void
test_query_timeout_cancels_backend (void)
{
  g_autoptr(SlowFile) f = slow_file_new (5000, TRUE, TRUE);
  Result r = { 0 };
  sd_query_usage_async (G_FILE (f), 50, NULL, on_usage, &r);
  wait_for (&r.done);
  g_assert_error (r.error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  spin_ms (100);
  g_assert_cmpuint (f->n_completed, ==, 1); /* backend finished via cancellation, not after 5 s */
  g_assert_cmpuint (r.n_callbacks, ==, 1);
  result_clear (&r);
}

static void
test_query_cancel (void)
{
  g_autoptr(SlowFile) f = slow_file_new (5000, TRUE, TRUE);
  g_autoptr(GCancellable) c = g_cancellable_new ();
  Result r = { 0 };
  sd_query_usage_async (G_FILE (f), 2000, c, on_usage, &r);
  spin_ms (20);
  g_cancellable_cancel (c);
  wait_for (&r.done);
  g_assert_error (r.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  spin_ms (50);
  g_assert_cmpuint (r.n_callbacks, ==, 1);
  result_clear (&r);
}

static void
test_query_already_cancelled (void)
{
  g_autoptr(GFile) dir = g_file_new_for_path (g_get_tmp_dir ());
  g_autoptr(GCancellable) c = g_cancellable_new ();
  g_cancellable_cancel (c);
  Result r = { 0 };
  sd_query_usage_async (dir, 1000, c, on_usage, &r);
  wait_for (&r.done);
  g_assert_error (r.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  result_clear (&r);
}

/* Repetition makes leaks and lifetime bugs visible to ASan/LSan and valgrind (PLAN.md §4.2). */
static void
test_query_stress (void)
{
  g_autoptr(GFile) dir = g_file_new_for_path (g_get_tmp_dir ());
  g_autoptr(SlowFile) dead = slow_file_new (200, FALSE, TRUE);
  for (int i = 0; i < 300; i++)
    {
      g_autoptr(GCancellable) c = g_cancellable_new ();
      Result a = { 0 }, b = { 0 }, d = { 0 };
      sd_query_usage_async (dir, 1000, c, on_usage, &a);
      sd_query_usage_async (G_FILE (dead), 5, NULL, on_usage, &b);   /* times out, late result follows */
      sd_query_usage_async (dir, 0, c, on_usage, &d);
      if (i % 2 == 0)
        g_cancellable_cancel (c);
      wait_for (&a.done);
      wait_for (&b.done);
      wait_for (&d.done);
      g_assert_error (b.error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
      result_clear (&a);
      result_clear (&b);
      result_clear (&d);
    }
  wait_for_completed (dead, 300); /* drain late results of the dead file */
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/drive-info/os-name", test_os_name);
  g_test_add_func ("/drive-info/classify", test_classify);
  g_test_add_func ("/drive-info/fill", test_fill);
  g_test_add_func ("/drive-info/usage-text", test_usage_text);
  g_test_add_func ("/drive-info/query/real", test_query_real);
  g_test_add_func ("/drive-info/query/unknown-size", test_query_unknown_size);
  g_test_add_func ("/drive-info/query/slow-but-in-time", test_query_slow_but_in_time);
  g_test_add_func ("/drive-info/query/timeout-dead-server", test_query_timeout_dead_server);
  g_test_add_func ("/drive-info/query/timeout-cancels-backend", test_query_timeout_cancels_backend);
  g_test_add_func ("/drive-info/query/cancel", test_query_cancel);
  g_test_add_func ("/drive-info/query/already-cancelled", test_query_already_cancelled);
  g_test_add_func ("/drive-info/query/stress", test_query_stress);
  return g_test_run ();
}
