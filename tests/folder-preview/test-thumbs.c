/* Tests for src/thumbs.c (PLAN.md §6.5 Phase 3). These run the real system thumbnailers (glycin, gst-video-
 * thumbnailer, gst-audio-thumbnailer) on the fixtures in tests/data/, with an isolated thumbnail cache. */
#define GNOME_DESKTOP_USE_UNSTABLE_API
#include <glib/gstdio.h>
#include <libgnome-desktop/gnome-desktop-thumbnail.h>
#include <locale.h>

#include "config.h"
#include "scanner.h"
#include "test-util.h"
#include "thumbs.h"

static char *run_dir;   /* Owned: per-run dir under the build dir (not /tmp, see tu_make_tmpdir_in). */
static char *data_dir;  /* Owned: tests/data in the source tree. */
static char *cache_dir; /* Owned: $XDG_CACHE_HOME for this run. */

static FpThumbs *thumbs; /* Owned; shared by the synchronous tests. */

/* ---- helpers ---- */

/* Caller owns the result. */
static FpScanResult *
scan (const char *dir, guint window)
{
  g_autoptr (GFile) folder = g_file_new_for_path (dir);
  g_autoptr (GError) error = NULL;
  FpScanResult *r = fp_scan_folder (folder, window, NULL, &error);
  g_assert_no_error (error);
  return r;
}

/* Caller owns the result. */
static FpResolved *
resolve (FpScanResult *s, guint wanted)
{
  g_autoptr (GError) error = NULL;
  FpResolved *r = fp_thumbs_resolve (thumbs, s, wanted, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (r);
  return r;
}

/* Space-separated names of the resolved thumbnails; caller frees. Also checks every path is a real file
 * inside our isolated cache. */
static char *
names_of (FpResolved *r)
{
  g_autoptr (GString) s = g_string_new (NULL);
  for (guint i = 0; i < r->thumbs->len; i++)
    {
      FpThumb *t = g_ptr_array_index (r->thumbs, i);
      g_assert_true (g_file_test (t->thumbnail_path, G_FILE_TEST_IS_REGULAR));
      g_assert_true (g_str_has_prefix (t->thumbnail_path, cache_dir));
      g_string_append_printf (s, "%s%s", i ? " " : "", t->name);
    }
  return g_string_free (g_steal_pointer (&s), FALSE);
}

/* Copies the named fixtures into a new folder @name and returns its path (caller frees). */
static char *
folder_with (const char *name, const char *const *fixtures)
{
  char *dir = tu_mkdir (run_dir, name);
  for (gsize i = 0; fixtures[i] != NULL; i++)
    tu_copy (data_dir, fixtures[i], dir, fixtures[i]);
  return dir;
}

/* ---- tests ---- */

/* PLAN.md §6.5 Phase 3 acceptance: mixed images/videos/mp3-with-art/mp3-without-art/txt → only images, videos and
 * audio with art, in natural order. Extra: FLAC with art, M4A without, a corrupt PNG. */
static void
test_acceptance_mixed (void)
{
  const char *fixtures[] = { "img2.png", "img10.png", "photo.jpg", "clip.mp4", "art.mp3", "noart.mp3",
                             "art.flac", "noart.m4a", "notes.txt", "broken.png", NULL };
  g_autofree char *dir = folder_with ("mixed", fixtures);

  g_autoptr (FpScanResult) s1 = scan (dir, SCAN_WINDOW);
  g_assert_cmpuint (s1->candidates->len, ==, 9); /* everything but notes.txt is a media type */
  g_autoptr (FpResolved) r1 = resolve (s1, MAX_PREVIEW_FILES);
  g_autofree char *got1 = names_of (r1);
  g_assert_cmpstr (got1, ==, "art.flac art.mp3 clip.mp4 img2.png img10.png photo.jpg");
  g_assert_cmpuint (r1->n_generated, ==, 6);
  g_assert_cmpuint (r1->n_failed, ==, 3); /* broken.png, noart.m4a, noart.mp3 */
  g_assert_false (r1->needs_rescan);

  FpMediaKind kinds[] = { FP_MEDIA_AUDIO, FP_MEDIA_AUDIO, FP_MEDIA_VIDEO, FP_MEDIA_IMAGE, FP_MEDIA_IMAGE,
                          FP_MEDIA_IMAGE };
  for (guint i = 0; i < r1->thumbs->len; i++)
    g_assert_cmpint (((FpThumb *) g_ptr_array_index (r1->thumbs, i))->kind, ==, kinds[i]);

  /* Second pass: failures are now recorded (the scanner skips them) and thumbnails are cached. */
  g_autoptr (FpScanResult) s2 = scan (dir, SCAN_WINDOW);
  g_assert_cmpuint (s2->candidates->len, ==, 6);
  for (guint i = 0; i < s2->candidates->len; i++)
    g_assert_nonnull (((FpCandidate *) g_ptr_array_index (s2->candidates, i))->thumbnail_path);
  g_autoptr (FpResolved) r2 = resolve (s2, MAX_PREVIEW_FILES);
  g_autofree char *got2 = names_of (r2);
  g_assert_cmpstr (got2, ==, got1);
  g_assert_cmpuint (r2->n_generated, ==, 0);
  g_assert_cmpuint (r2->n_failed, ==, 0);
}

/* Static display only needs frame 0 (PLAN.md §6.1.3): resolving 4 must not generate more than 4. */
static void
test_stops_at_wanted (void)
{
  g_autofree char *dir = tu_mkdir (run_dir, "ten");
  for (guint i = 1; i <= 10; i++)
    {
      g_autofree char *name = g_strdup_printf ("pic%u.png", i);
      tu_copy (data_dir, i % 2 ? "img2.png" : "img10.png", dir, name);
    }
  g_autoptr (FpScanResult) s = scan (dir, SCAN_WINDOW);
  g_autoptr (FpResolved) frame0 = resolve (s, PREVIEW_SLOTS);
  g_autofree char *n0 = names_of (frame0);
  g_assert_cmpstr (n0, ==, "pic1.png pic2.png pic3.png pic4.png");
  g_assert_cmpuint (frame0->n_generated, ==, 4);

  /* Hover frames later: only the 4 new ones are generated. */
  g_autoptr (FpResolved) more = resolve (s, 8);
  g_assert_cmpuint (more->thumbs->len, ==, 8);
  g_assert_cmpuint (more->n_generated, ==, 4);
}

/* All candidates in the window fail → needs_rescan; the rescan skips the recorded failures. */
static void
test_needs_rescan (void)
{
  g_autofree char *dir = tu_mkdir (run_dir, "rescan");
  tu_copy (data_dir, "broken.png", dir, "a1.png");
  tu_copy (data_dir, "broken.png", dir, "a2.png");
  tu_copy (data_dir, "broken.png", dir, "a3.png");
  tu_copy (data_dir, "img2.png", dir, "b.png");

  g_autoptr (FpScanResult) s1 = scan (dir, 2);
  g_assert_true (s1->more_available);
  g_autoptr (FpResolved) r1 = resolve (s1, 2);
  g_assert_cmpuint (r1->thumbs->len, ==, 0);
  g_assert_cmpuint (r1->n_failed, ==, 2);
  g_assert_true (r1->needs_rescan);

  g_autoptr (FpScanResult) s2 = scan (dir, 2);
  g_autoptr (FpResolved) r2 = resolve (s2, 2);
  g_autofree char *n2 = names_of (r2);
  g_assert_cmpstr (n2, ==, "b.png");
  g_assert_false (r2->needs_rescan);
}

/* A file that changed after its thumbnail was made gets a fresh one, not the stale one. */
static void
test_regenerates_after_change (void)
{
  const char *fixtures[] = { "img2.png", NULL };
  g_autofree char *dir = folder_with ("changed", fixtures);
  g_autoptr (FpScanResult) s1 = scan (dir, SCAN_WINDOW);
  g_autoptr (FpResolved) r1 = resolve (s1, 4);
  g_assert_cmpuint (r1->n_generated, ==, 1);

  g_autofree char *path = g_build_filename (dir, "img2.png", NULL);
  g_autoptr (GFile) f = g_file_new_for_path (path);
  g_assert_true (g_file_set_attribute_uint64 (f, G_FILE_ATTRIBUTE_TIME_MODIFIED, (guint64) g_get_real_time () / G_USEC_PER_SEC + 50,
                                              G_FILE_QUERY_INFO_NONE, NULL, NULL));
  g_autoptr (FpScanResult) s2 = scan (dir, SCAN_WINDOW);
  g_assert_null (((FpCandidate *) g_ptr_array_index (s2->candidates, 0))->thumbnail_path);
  g_autoptr (FpResolved) r2 = resolve (s2, 4);
  g_assert_cmpuint (r2->n_generated, ==, 1);
}

/* A failure recorded after the scan (e.g. by Nautilus) is respected: skipped, nothing regenerated. */
static void
test_failure_recorded_after_scan (void)
{
  const char *fixtures[] = { "img2.png", "photo.jpg", NULL };
  g_autofree char *dir = folder_with ("late-fail", fixtures);
  g_autoptr (FpScanResult) s = scan (dir, SCAN_WINDOW);
  g_assert_cmpuint (s->candidates->len, ==, 2);

  FpCandidate *first = g_ptr_array_index (s->candidates, 0);
  g_autofree char *uri = g_file_get_uri (first->file);
  g_autoptr (GnomeDesktopThumbnailFactory) f = gnome_desktop_thumbnail_factory_new (GNOME_DESKTOP_THUMBNAIL_SIZE_LARGE);
  g_assert_true (gnome_desktop_thumbnail_factory_create_failed_thumbnail (f, uri, (time_t) first->mtime, NULL, NULL));

  g_autoptr (FpResolved) r = resolve (s, 4);
  g_autofree char *got = names_of (r);
  g_assert_cmpstr (got, ==, "photo.jpg");
  g_assert_cmpuint (r->n_failed, ==, 1);
  g_assert_cmpuint (r->n_generated, ==, 1);
}

/* If the cache can't be written, the file is skipped. gnome-desktop's save_thumbnail() then records a
 * failure itself (verified separately; PLAN.md §6.4), so a rescan excludes the file until it changes. */
static void
test_unwritable_cache (void)
{
  const char *fixtures[] = { "img10.png", NULL };
  g_autofree char *dir = folder_with ("readonly-cache", fixtures);
  g_autofree char *large = g_build_filename (cache_dir, "thumbnails", "large", NULL);
  g_assert_cmpint (g_mkdir_with_parents (large, 0755), ==, 0);
  g_assert_cmpint (g_chmod (large, 0555), ==, 0);
  if (g_access (large, W_OK) == 0)
    {
      g_chmod (large, 0755);
      g_test_skip ("running as root: permissions are not enforced");
      return;
    }

  g_autoptr (FpScanResult) s = scan (dir, SCAN_WINDOW);
  g_autoptr (FpResolved) r = resolve (s, 4);
  g_assert_cmpint (g_chmod (large, 0755), ==, 0);
  g_assert_cmpuint (r->thumbs->len, ==, 0);
  g_assert_cmpuint (r->n_failed, ==, 1);
  g_assert_cmpuint (r->n_generated, ==, 0);

  g_autoptr (FpScanResult) s2 = scan (dir, SCAN_WINDOW);
  g_assert_cmpuint (s2->candidates->len, ==, 0);
}

static void
test_cancelled (void)
{
  const char *fixtures[] = { "photo.jpg", NULL };
  g_autofree char *dir = folder_with ("cancel", fixtures);
  g_autoptr (FpScanResult) s = scan (dir, SCAN_WINDOW);
  g_autoptr (GCancellable) c = g_cancellable_new ();
  g_cancellable_cancel (c);
  g_autoptr (GError) error = NULL;
  g_autoptr (FpResolved) r = fp_thumbs_resolve (thumbs, s, 4, c, &error);
  g_assert_null (r);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

static void
test_free_null (void)
{
  fp_thumb_free (NULL);
  fp_resolved_free (NULL);
  fp_thumbs_free (NULL);
}

/* ---- async ---- */

typedef struct
{
  guint done;
  guint ok;
  guint cancelled;
} Tally;

static void
on_resolved (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) source;
  Tally *t = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (FpResolved) r = fp_thumbs_resolve_finish (res, &error);
  if (r != NULL)
    t->ok++;
  else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    t->cancelled++;
  else
    g_error ("unexpected resolve error: %s", error->message);
  t->done++;
}

/* Several folders at once on the shared pool; each gets its own correct result. */
static void
test_async_concurrent (void)
{
  g_autoptr (FpThumbs) own = fp_thumbs_new (WORKER_THREADS);
  FpWorkers *workers = fp_workers_new (WORKER_THREADS);
  Tally t = { 0 };
  g_autoptr (GPtrArray) scans = g_ptr_array_new_with_free_func ((GDestroyNotify) fp_scan_result_unref);
  for (guint i = 0; i < 6; i++)
    {
      g_autofree char *name = g_strdup_printf ("conc%u", i);
      g_autofree char *dir = tu_mkdir (run_dir, name);
      tu_copy (data_dir, "img2.png", dir, "x.png");
      tu_copy (data_dir, "photo.jpg", dir, "y.jpg");
      FpScanResult *s = scan (dir, SCAN_WINDOW);
      g_ptr_array_add (scans, s);
      fp_thumbs_resolve_async (own, workers, s, 4, 0, NULL, on_resolved, &t);
    }
  tu_wait_for (&t.done, 6);
  g_assert_cmpuint (t.ok, ==, 6);
  fp_workers_free (workers); /* before fp_thumbs_free (autoptr at scope end) */
}

/* Cancel resolves while thumbnailers run, and shut the pool down with work queued. */
static void
test_async_cancel_and_shutdown (void)
{
  g_autoptr (FpThumbs) own = fp_thumbs_new (WORKER_THREADS);
  FpWorkers *workers = fp_workers_new (WORKER_THREADS);
  g_autofree char *dir = tu_mkdir (run_dir, "busy");
  for (guint i = 1; i <= 12; i++)
    {
      g_autofree char *name = g_strdup_printf ("p%02u.jpg", i);
      tu_copy (data_dir, "photo.jpg", dir, name);
    }
  g_autoptr (FpScanResult) s = scan (dir, SCAN_WINDOW);

  Tally t = { 0 };
  g_autoptr (GPtrArray) cancellables = g_ptr_array_new_with_free_func (g_object_unref);
  for (guint i = 0; i < 10; i++)
    {
      GCancellable *c = g_cancellable_new ();
      g_ptr_array_add (cancellables, c);
      fp_thumbs_resolve_async (own, workers, s, MAX_PREVIEW_FILES, 0, c, on_resolved, &t);
    }
  /* Let the first ones start generating, then cancel everything. */
  g_usleep (150 * 1000);
  for (guint i = 0; i < cancellables->len; i++)
    g_cancellable_cancel (g_ptr_array_index (cancellables, i));
  /* Queue more and shut down immediately: the queued ones must come back CANCELLED. */
  for (guint i = 0; i < 10; i++)
    fp_thumbs_resolve_async (own, workers, s, MAX_PREVIEW_FILES, 0, NULL, on_resolved, &t);
  fp_workers_free (workers);
  tu_wait_for (&t.done, 20);
  g_test_message ("ok=%u cancelled=%u", t.ok, t.cancelled);
  g_assert_cmpuint (t.ok + t.cancelled, ==, 20);
  g_assert_cmpuint (t.cancelled, >=, 10);
}

int
main (int argc, char **argv)
{
  setlocale (LC_ALL, "");
  /* Must be outside /tmp (gnome-desktop sandbox issue) and set before GLib computes the cache dir. */
  const char *builddir = g_getenv ("G_TEST_BUILDDIR");
  const char *srcdir = g_getenv ("G_TEST_SRCDIR");
  g_assert_nonnull (builddir);
  g_assert_nonnull (srcdir);
  run_dir = tu_make_tmpdir_in (builddir, "thumbs-run");
  cache_dir = g_build_filename (run_dir, "xdg-cache", NULL);
  g_setenv ("XDG_CACHE_HOME", cache_dir, TRUE);
  data_dir = g_build_filename (srcdir, "data", NULL);

  g_test_init (&argc, &argv, NULL);
  thumbs = fp_thumbs_new (1);

  g_test_add_func ("/thumbs/acceptance-mixed", test_acceptance_mixed);
  g_test_add_func ("/thumbs/stops-at-wanted", test_stops_at_wanted);
  g_test_add_func ("/thumbs/needs-rescan", test_needs_rescan);
  g_test_add_func ("/thumbs/regenerates-after-change", test_regenerates_after_change);
  g_test_add_func ("/thumbs/failure-recorded-after-scan", test_failure_recorded_after_scan);
  g_test_add_func ("/thumbs/unwritable-cache", test_unwritable_cache);
  g_test_add_func ("/thumbs/cancelled", test_cancelled);
  g_test_add_func ("/thumbs/free-null", test_free_null);
  g_test_add_func ("/thumbs/async-concurrent", test_async_concurrent);
  g_test_add_func ("/thumbs/async-cancel-and-shutdown", test_async_cancel_and_shutdown);
  int ret = g_test_run ();

  g_clear_pointer (&thumbs, fp_thumbs_free);
  tu_rm_rf (run_dir);
  g_clear_pointer (&run_dir, g_free);
  g_clear_pointer (&cache_dir, g_free);
  g_clear_pointer (&data_dir, g_free);
  return ret;
}
