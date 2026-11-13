/* Tests for src/cache.c: request pipeline, memory LRU, disk cache (PLAN.md §6.5 Phase 4).
 * Uses the real thumbnailers on tests/data fixtures; files live under the build dir (not /tmp). */
#include <glib/gstdio.h>
#include <locale.h>
#include <string.h>

#include "cache.h"
#include "config.h"
#include "test-util.h"

static char *run_dir;   /* Owned. */
static char *data_dir;  /* Owned. */
static char *cache_dir; /* Owned: $XDG_CACHE_HOME (thumbnails) for this run. */
static char *disk_dir;  /* Owned: our frame disk cache for this run. */

/* One "Nautilus session": pool + factories + cache. */
typedef struct
{
  FpWorkers *workers;
  FpThumbs  *thumbs;
  FpCache   *cache;
} Session;

static void
session_start (Session *s, guint max_folders, gsize max_bytes)
{
  s->workers = fp_workers_new (WORKER_THREADS);
  s->thumbs = fp_thumbs_new (WORKER_THREADS);
  s->cache = fp_cache_new (s->workers, s->thumbs, disk_dir, max_folders, max_bytes);
}

/* Shutdown order from cache.h: workers, then cache, then thumbs. Then deliver pending callbacks. */
static void
session_stop (Session *s)
{
  g_clear_pointer (&s->workers, fp_workers_free);
  g_clear_pointer (&s->cache, fp_cache_unref);
  g_clear_pointer (&s->thumbs, fp_thumbs_free);
  while (g_main_context_iteration (NULL, FALSE))
    ;
}

typedef struct
{
  guint    done;
  FpFrame *frame;   /* Owned result (last one). */
  GError  *error;   /* Owned. */
} Wait;

static void
on_frame (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) source;
  Wait *w = user_data;
  g_clear_pointer (&w->frame, fp_frame_free);
  g_clear_error (&w->error);
  w->frame = fp_cache_get_frame_finish (res, &w->error);
  w->done++;
}

/* Synchronously requests a frame; caller owns the result. Asserts success. */
static FpFrame *
get_frame (FpCache *cache, const char *path, guint frame, guint px)
{
  g_autoptr (GFile) folder = g_file_new_for_path (path);
  Wait w = { 0 };
  fp_cache_get_frame_async (cache, folder, frame, px, 0, NULL, on_frame, &w);
  tu_wait_for (&w.done, 1);
  g_assert_no_error (w.error);
  g_assert_nonnull (w.frame);
  return w.frame;
}

static char *
folder_of_images (const char *name, guint n)
{
  const char *src[] = { "img2.png", "img10.png", "photo.jpg" };
  char *dir = tu_mkdir (run_dir, name);
  for (guint i = 1; i <= n; i++)
    {
      g_autofree char *dst = g_strdup_printf ("p%02u.%s", i, i % 3 == 2 ? "jpg" : "png");
      tu_copy (data_dir, src[i % 3], dir, dst);
    }
  return dir;
}

/* Files in our disk cache for @path in look @style (sorted, space-separated); caller frees. */
static char *
disk_files_for_style (const char *path, const FpStyle *style)
{
  g_autoptr (GFile) f = g_file_new_for_path (path);
  g_autofree char *uri = g_file_get_uri (f);
  g_autofree char *key = fp_style_key (style);
  g_autofree char *name = g_strconcat (key, "|", uri, NULL);
  g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA1, name, -1);
  g_autofree char *dir = g_build_filename (disk_dir, CACHE_DISK_VERSION, hash, NULL);
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  const char *n;
  while (d != NULL && (n = g_dir_read_name (d)) != NULL)
    {
      /* Strip the "<sec>.<usec>-" prefix so the result is predictable. */
      const char *dash = strchr (n, '-');
      g_ptr_array_add (names, g_strdup (dash != NULL ? dash + 1 : n));
    }
  g_ptr_array_sort_values (names, (GCompareFunc) g_strcmp0);
  g_ptr_array_add (names, NULL);
  return g_strjoinv (" ", (char **) names->pdata);
}

/* Same, for the default (v1 grid) look. Caller frees. */
static char *
disk_files_for (const char *path)
{
  FpStyle grid;
  fp_style_init_default (&grid);
  return disk_files_for_style (path, &grid);
}

/* Downloads @t; caller owns the bytes. */
static GBytes *
pixels_of (GdkTexture *t)
{
  g_autoptr (GdkTextureDownloader) d = gdk_texture_downloader_new (t);
  gdk_texture_downloader_set_format (d, GDK_MEMORY_R8G8B8A8);
  gsize stride = 0;
  return gdk_texture_downloader_download_bytes (d, &stride);
}

static void
debug_save (GdkTexture *t, const char *name)
{
  const char *out = g_getenv ("FP_DEBUG_DIR");
  if (out == NULL || t == NULL)
    return;
  g_mkdir_with_parents (out, 0755);
  g_autofree char *path = g_build_filename (out, name, NULL);
  gdk_texture_save_to_png (t, path);
}

static guint64
mtime_of (const char *path)
{
  g_autoptr (GFile) f = g_file_new_for_path (path);
  g_autoptr (GFileInfo) i = g_file_query_info (f, "time::modified,time::modified-usec", 0, NULL, NULL);
  return g_file_info_get_attribute_uint64 (i, "time::modified") * 1000000 +
         g_file_info_get_attribute_uint32 (i, "time::modified-usec");
}

/* ---- tests ---- */

/* PLAN.md §6.5 Phase 4 acceptance: second launch loads frame 0 from disk without any thumbnail decoding. */
static void
test_second_launch_from_disk (void)
{
  g_autofree char *dir = folder_of_images ("photos", 6);

  Session a = { 0 };
  session_start (&a, LRU_FOLDERS, CACHE_MAX_BYTES);
  g_autoptr (FpFrame) f1 = get_frame (a.cache, dir, 0, 96);
  g_assert_nonnull (f1->texture);
  g_assert_false (f1->from_disk);
  g_assert_cmpuint (f1->n_frames_hint, ==, 2);
  g_assert_cmpint (gdk_texture_get_width (f1->texture), ==, 96);
  debug_save (f1->texture, "photos-frame0-96.png");
  g_autofree char *files = disk_files_for (dir);
  g_assert_cmpstr (files, ==, "96-2.png");

  /* Memory hit. */
  g_autoptr (GdkTexture) peeked = NULL;
  guint hint = 0;
  g_autofree char *uri = g_filename_to_uri (dir, NULL, NULL);
  g_assert_true (fp_cache_peek_frame (a.cache, uri, 0, 96, &peeked, &hint));
  session_stop (&a);
  g_assert_true (peeked == f1->texture);
  g_assert_cmpuint (hint, ==, 2);

  /* "Second launch": wipe every thumbnail and make the media unreadable. Neither changes the folder's
   * mtime, so the disk entry is still valid — and it is now the only way to produce this frame. */
  guint64 before = mtime_of (dir);
  g_autofree char *thumbs_dir = g_build_filename (cache_dir, "thumbnails", NULL);
  tu_rm_rf (thumbs_dir);
  for (guint i = 1; i <= 6; i++)
    {
      g_autofree char *name = g_strdup_printf ("p%02u.%s", i, i % 3 == 2 ? "jpg" : "png");
      g_autofree char *p = g_build_filename (dir, name, NULL);
      g_assert_cmpint (g_chmod (p, 0), ==, 0);
    }
  g_assert_cmpuint (mtime_of (dir), ==, before);

  Session b = { 0 };
  session_start (&b, LRU_FOLDERS, CACHE_MAX_BYTES);
  g_autoptr (FpFrame) f2 = get_frame (b.cache, dir, 0, 96);
  session_stop (&b);
  g_assert_true (f2->from_disk);
  g_assert_nonnull (f2->texture);
  g_assert_cmpuint (f2->n_frames_hint, ==, 2);
  g_autoptr (GBytes) p1 = pixels_of (f1->texture);
  g_autoptr (GBytes) p2 = pixels_of (f2->texture);
  g_assert_true (g_bytes_equal (p1, p2));
  g_assert_false (g_file_test (thumbs_dir, G_FILE_TEST_EXISTS)); /* nothing was regenerated */

  for (guint i = 1; i <= 6; i++)
    {
      g_autofree char *name = g_strdup_printf ("p%02u.%s", i, i % 3 == 2 ? "jpg" : "png");
      g_autofree char *p = g_build_filename (dir, name, NULL);
      g_chmod (p, 0644);
    }
}

static void
test_hover_frames (void)
{
  g_autofree char *dir = folder_of_images ("hover", 6);
  Session s = { 0 };
  session_start (&s, LRU_FOLDERS, CACHE_MAX_BYTES);
  g_autoptr (FpFrame) f0 = get_frame (s.cache, dir, 0, 128);
  g_autoptr (FpFrame) f1 = get_frame (s.cache, dir, 1, 128);
  g_autoptr (FpFrame) f2 = get_frame (s.cache, dir, 2, 128);
  g_autoptr (FpFrame) f9 = get_frame (s.cache, dir, 9, 128); /* beyond MAX_PREVIEW_FILES */
  g_assert_nonnull (f0->texture);
  g_assert_nonnull (f1->texture);
  g_assert_false (f1->from_disk);
  g_assert_null (f2->texture);
  g_assert_null (f9->texture);
  g_assert_cmpuint (f2->n_frames_hint, ==, 2);
  debug_save (f1->texture, "hover-frame1-128.png");

  /* Hover frames are memory-only (disk holds frame 0 only). */
  g_autofree char *files = disk_files_for (dir);
  g_assert_cmpstr (files, ==, "128-2.png");
  session_stop (&s);
}

/* PLAN.md §11: stacked frames step through the photos (1 or 3 per frame) and wrap around; a style change
 * forgets memory, drops results of older requests, and keeps one disk entry per look. */
static void
test_styles (void)
{
  g_autofree char *dir = folder_of_images ("styles", 6);
  g_autofree char *uri = g_filename_to_uri (dir, NULL, NULL);
  Session s = { 0 };
  session_start (&s, LRU_FOLDERS, CACHE_MAX_BYTES);
  FpStyle grid = *fp_cache_get_style (s.cache);
  g_assert_cmpint (grid.layout, ==, FP_LAYOUT_GRID);
  g_assert_false (fp_cache_set_style (s.cache, &grid)); /* same look: nothing changes */

  g_autoptr (FpFrame) g0 = get_frame (s.cache, dir, 0, 128);
  g_assert_cmpuint (g0->n_frames_hint, ==, 2);
  g_assert_cmpuint (fp_cache_get_n_folders (s.cache), ==, 1);

  /* A request still running when the style changes is dropped (CANCELLED), never cached. */
  FpStyle stack = grid;
  stack.layout = FP_LAYOUT_STACKED;
  stack.stack_step = 1;
  g_autoptr (GFile) folder = g_file_new_for_path (dir);
  Wait w = { 0 };
  fp_cache_get_frame_async (s.cache, folder, 1, 128, 0, NULL, on_frame, &w);
  g_assert_true (fp_cache_set_style (s.cache, &stack));
  g_assert_cmpuint (fp_cache_get_n_folders (s.cache), ==, 0);
  tu_wait_for (&w.done, 1);
  g_assert_error (w.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&w.error);
  g_assert_cmpuint (fp_cache_get_n_folders (s.cache), ==, 0);

  /* Stacked, one per step: a frame per photo, the last ones wrap around. */
  g_autoptr (FpFrame) s0 = get_frame (s.cache, dir, 0, 128);
  g_assert_nonnull (s0->texture);
  g_assert_cmpint (gdk_texture_get_height (s0->texture), ==, (int) fp_composite_frame_height (&stack, 128));
  g_assert_cmpuint (s0->n_frames_hint, ==, 6);
  debug_save (s0->texture, "styles-stack0.png");
  g_autoptr (FpFrame) s5 = get_frame (s.cache, dir, 5, 128);
  g_assert_nonnull (s5->texture);
  g_autoptr (FpFrame) s6 = get_frame (s.cache, dir, 6, 128);
  g_assert_null (s6->texture);
  g_autoptr (GdkTexture) peeked = NULL;
  g_assert_true (fp_cache_peek_frame (s.cache, uri, 5, 128, &peeked, NULL));
  g_assert_true (peeked == s5->texture);

  /* Stacked, three per step: 6 photos = 2 frames. */
  stack.stack_step = 3;
  g_assert_true (fp_cache_set_style (s.cache, &stack));
  g_autoptr (FpFrame) t0 = get_frame (s.cache, dir, 0, 128);
  g_assert_cmpuint (t0->n_frames_hint, ==, 2);
  g_autoptr (FpFrame) t1 = get_frame (s.cache, dir, 1, 128);
  g_assert_nonnull (t1->texture);
  g_autoptr (FpFrame) t2 = get_frame (s.cache, dir, 2, 128);
  g_assert_null (t2->texture);

  /* One disk entry per look; going back to a look is a disk hit. */
  g_autofree char *gfiles = disk_files_for (dir);
  g_assert_cmpstr (gfiles, ==, "128-2.png");
  stack.stack_step = 1;
  g_autofree char *sfiles = disk_files_for_style (dir, &stack);
  g_assert_cmpstr (sfiles, ==, "128-6.png");
  g_assert_true (fp_cache_set_style (s.cache, &grid));
  g_autoptr (FpFrame) back = get_frame (s.cache, dir, 0, 128);
  g_assert_true (back->from_disk);
  session_stop (&s);
}

/* Stacked with fewer photos than cards: the stack has as many cards as photos, and cycling still works. */
static void
test_stacked_few_photos (void)
{
  g_autofree char *dir = folder_of_images ("two", 2);
  Session s = { 0 };
  session_start (&s, LRU_FOLDERS, CACHE_MAX_BYTES);
  FpStyle stack = *fp_cache_get_style (s.cache);
  stack.layout = FP_LAYOUT_STACKED;
  fp_cache_set_style (s.cache, &stack);
  g_autoptr (FpFrame) f0 = get_frame (s.cache, dir, 0, 96);
  g_autoptr (FpFrame) f1 = get_frame (s.cache, dir, 1, 96);
  g_autoptr (FpFrame) f2 = get_frame (s.cache, dir, 2, 96);
  g_assert_nonnull (f0->texture);
  g_assert_nonnull (f1->texture);
  g_assert_null (f2->texture);
  g_assert_cmpuint (f0->n_frames_hint, ==, 2);
  debug_save (f1->texture, "two-stack1.png");
  session_stop (&s);
}

/* Old disk cache versions are deleted in the background; the current one and foreign entries stay. */
static void
test_purge_old_versions (void)
{
  g_autofree char *old_file = g_build_filename (disk_dir, "v3", "abc", "1.0-96-1.png", NULL);
  g_autofree char *old_dir = g_build_filename (disk_dir, "v3", NULL);
  g_autofree char *keep_file = g_build_filename (disk_dir, CACHE_DISK_VERSION, "keep", "x.png", NULL);
  g_autofree char *foreign = g_build_filename (disk_dir, "notes", NULL);
  for (const char *const *p = (const char *const[]) { old_file, keep_file, NULL }; *p; p++)
    {
      g_autofree char *parent = g_path_get_dirname (*p);
      g_assert_cmpint (g_mkdir_with_parents (parent, 0700), ==, 0);
      g_assert_true (g_file_set_contents_full (*p, "x", -1, G_FILE_SET_CONTENTS_NONE, 0600, NULL));
    }
  g_assert_true (g_file_set_contents_full (foreign, "x", -1, G_FILE_SET_CONTENTS_NONE, 0600, NULL));

  Session s = { 0 };
  session_start (&s, LRU_FOLDERS, CACHE_MAX_BYTES);
  fp_cache_purge_old_versions (s.cache);
  /* A job that hasn't started when the pool is freed is cancelled, so wait for it to run. */
  for (int i = 0; i < 500 && g_file_test (old_dir, G_FILE_TEST_EXISTS); i++)
    g_usleep (10 * 1000);
  session_stop (&s);
  g_assert_false (g_file_test (old_dir, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_test (keep_file, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_test (foreign, G_FILE_TEST_EXISTS));
  g_unlink (keep_file);
  g_unlink (foreign);
}

static void
test_no_media (void)
{
  g_autofree char *dir = tu_mkdir (run_dir, "docs");
  tu_copy (data_dir, "notes.txt", dir, "a.txt");
  Session s = { 0 };
  session_start (&s, LRU_FOLDERS, CACHE_MAX_BYTES);
  g_autoptr (FpFrame) f = get_frame (s.cache, dir, 0, 96);
  session_stop (&s);
  g_assert_null (f->texture);
  g_assert_cmpuint (f->n_frames_hint, ==, 0);
  g_assert_false (f->from_disk);
  g_autofree char *files = disk_files_for (dir);
  g_assert_cmpstr (files, ==, "none");

  /* Relaunch: answered by the marker, no scan. */
  Session s2 = { 0 };
  session_start (&s2, LRU_FOLDERS, CACHE_MAX_BYTES);
  g_autoptr (FpFrame) f2 = get_frame (s2.cache, dir, 0, 96);
  session_stop (&s2);
  g_assert_true (f2->from_disk);
  g_assert_null (f2->texture);
}

static void
test_invalidation_by_mtime (void)
{
  g_autofree char *dir = folder_of_images ("changing", 2);
  Session s = { 0 };
  session_start (&s, LRU_FOLDERS, CACHE_MAX_BYTES);
  g_autoptr (FpFrame) f1 = get_frame (s.cache, dir, 0, 64);
  g_assert_cmpuint (f1->n_frames_hint, ==, 1);
  g_autofree char *before = disk_files_for (dir);
  g_assert_cmpstr (before, ==, "64-1.png");

  g_usleep (20 * 1000);
  for (guint i = 3; i <= 5; i++)
    {
      g_autofree char *name = g_strdup_printf ("p%02u.png", i);
      tu_copy (data_dir, "img2.png", dir, name);
    }
  g_autoptr (FpFrame) f2 = get_frame (s.cache, dir, 0, 64);
  g_assert_false (f2->from_disk);
  g_assert_cmpuint (f2->n_frames_hint, ==, 2);
  g_autofree char *after = disk_files_for (dir);
  g_assert_cmpstr (after, ==, "64-2.png"); /* the stale-mtime file was removed */

  g_autofree char *uri = g_filename_to_uri (dir, NULL, NULL);
  g_autoptr (GdkTexture) t = NULL;
  g_assert_true (fp_cache_peek_frame (s.cache, uri, 0, 64, &t, NULL));
  g_clear_object (&t);
  g_assert_true (fp_cache_invalidate (s.cache, uri));
  g_assert_false (fp_cache_invalidate (s.cache, uri));
  g_assert_false (fp_cache_peek_frame (s.cache, uri, 0, 64, &t, NULL));
  session_stop (&s);
}

static void
test_sizes_and_corrupt_disk_file (void)
{
  g_autofree char *dir = folder_of_images ("sizes", 3);
  Session s = { 0 };
  session_start (&s, LRU_FOLDERS, CACHE_MAX_BYTES);
  g_autoptr (FpFrame) a = get_frame (s.cache, dir, 0, 64);
  g_autoptr (FpFrame) b = get_frame (s.cache, dir, 0, 192);
  session_stop (&s);
  g_assert_cmpint (gdk_texture_get_width (b->texture), ==, 192);
  {
    /* Largest cached size wins; unknown folder / frame → NULL. */
    Session p = { 0 };
    session_start (&p, LRU_FOLDERS, CACHE_MAX_BYTES);
    g_autoptr (FpFrame) p64 = get_frame (p.cache, dir, 0, 64);
    g_autoptr (FpFrame) p192 = get_frame (p.cache, dir, 0, 192);
    g_autofree char *u = g_filename_to_uri (dir, NULL, NULL);
    g_autoptr (GdkTexture) any = fp_cache_peek_frame_any_size (p.cache, u, 0);
    g_assert_nonnull (any);
    g_assert_cmpint (gdk_texture_get_width (any), ==, 192);
    g_assert_null (fp_cache_peek_frame_any_size (p.cache, u, 3));
    g_assert_null (fp_cache_peek_frame_any_size (p.cache, "file:///nowhere", 0));
    session_stop (&p);
  }
  g_autofree char *files = disk_files_for (dir);
  g_assert_cmpstr (files, ==, "192-1.png 64-1.png");

  /* Corrupt the 64 px file: next launch must rebuild it, not fail. */
  g_autoptr (GFile) f = g_file_new_for_path (dir);
  g_autofree char *uri = g_file_get_uri (f);
  FpStyle grid;
  fp_style_init_default (&grid);
  g_autofree char *key = fp_style_key (&grid);
  g_autofree char *name = g_strconcat (key, "|", uri, NULL);
  g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA1, name, -1);
  g_autofree char *hdir = g_build_filename (disk_dir, CACHE_DISK_VERSION, hash, NULL);
  g_autoptr (GDir) d = g_dir_open (hdir, 0, NULL);
  const char *n;
  while ((n = g_dir_read_name (d)) != NULL)
    if (g_str_has_suffix (n, "-64-1.png"))
      {
        g_autofree char *p = g_build_filename (hdir, n, NULL);
        /* No fsync: the default (durable) write can stall for > 30 s on the NTFS project drive. */
        g_assert_true (g_file_set_contents_full (p, "garbage", -1, G_FILE_SET_CONTENTS_NONE, 0644, NULL));
      }

  Session s2 = { 0 };
  session_start (&s2, LRU_FOLDERS, CACHE_MAX_BYTES);
  g_autoptr (FpFrame) c = get_frame (s2.cache, dir, 0, 64);
  session_stop (&s2);
  g_assert_false (c->from_disk);
  g_assert_nonnull (c->texture);
}

static void
test_lru_limits (void)
{
  Session s = { 0 };
  session_start (&s, 3, CACHE_MAX_BYTES);
  g_autoptr (GPtrArray) uris = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < 5; i++)
    {
      g_autofree char *name = g_strdup_printf ("lru%u", i);
      g_autofree char *dir = folder_of_images (name, 1);
      g_autoptr (FpFrame) f = get_frame (s.cache, dir, 0, 64);
      g_ptr_array_add (uris, g_filename_to_uri (dir, NULL, NULL));
    }
  g_assert_cmpuint (fp_cache_get_n_folders (s.cache), ==, 3);
  for (guint i = 0; i < 5; i++)
    {
      g_autoptr (GdkTexture) t = NULL;
      gboolean hit = fp_cache_peek_frame (s.cache, g_ptr_array_index (uris, i), 0, 64, &t, NULL);
      g_assert_cmpint (hit, ==, i >= 2);
    }
  session_stop (&s);

  /* Byte budget: room for two 64 px frames only. */
  gsize frame_bytes = 64 * 64 * 4;
  Session b = { 0 };
  session_start (&b, 100, 2 * frame_bytes);
  for (guint i = 0; i < 5; i++)
    {
      g_autofree char *name = g_strdup_printf ("lru%u", i);
      g_autofree char *dir = g_build_filename (run_dir, name, NULL);
      g_autoptr (FpFrame) f = get_frame (b.cache, dir, 0, 64);
      g_assert_cmpuint (fp_cache_get_n_bytes (b.cache), <=, 2 * frame_bytes);
    }
  g_assert_cmpuint (fp_cache_get_n_folders (b.cache), ==, 2);
  session_stop (&b);
}

/* More failing files than the scan window: the cache rescans (failures now recorded) and still finds the
 * good image behind them. */
static void
test_rescan_past_failures (void)
{
  g_autofree char *dir = tu_mkdir (run_dir, "mostly-broken");
  for (guint i = 1; i <= SCAN_WINDOW + 1; i++)
    {
      g_autofree char *name = g_strdup_printf ("a%02u.png", i);
      tu_copy (data_dir, "broken.png", dir, name);
    }
  tu_copy (data_dir, "img2.png", dir, "b.png");
  Session s = { 0 };
  session_start (&s, LRU_FOLDERS, CACHE_MAX_BYTES);
  g_autoptr (FpFrame) f = get_frame (s.cache, dir, 0, 64);
  g_assert_nonnull (f->texture);
  g_assert_cmpuint (f->n_frames_hint, ==, 1);

  /* Asking again for the same frame and size replaces the stored texture (byte count stays exact). */
  gsize bytes = fp_cache_get_n_bytes (s.cache);
  g_autoptr (FpFrame) again = get_frame (s.cache, dir, 0, 64);
  g_assert_cmpuint (fp_cache_get_n_bytes (s.cache), ==, bytes);
  session_stop (&s);
}

/* An unwritable disk cache only costs the disk hit; frames are still produced and kept in memory. */
static void
test_unwritable_disk_cache (void)
{
  g_autofree char *dir = folder_of_images ("ro-disk", 2);
  g_autofree char *ro = g_build_filename (run_dir, "ro-frames", NULL);
  g_assert_cmpint (g_mkdir_with_parents (ro, 0755), ==, 0);
  g_assert_cmpint (g_chmod (ro, 0555), ==, 0);
  if (g_access (ro, W_OK) == 0)
    {
      g_chmod (ro, 0755);
      g_test_skip ("running as root");
      return;
    }
  FpWorkers *workers = fp_workers_new (WORKER_THREADS);
  FpThumbs *thumbs = fp_thumbs_new (WORKER_THREADS);
  FpCache *cache = fp_cache_new (workers, thumbs, ro, LRU_FOLDERS, CACHE_MAX_BYTES);
  g_autoptr (FpFrame) f = get_frame (cache, dir, 0, 64);
  g_assert_nonnull (f->texture);
  g_autoptr (FpFrame) f2 = get_frame (cache, dir, 0, 64);
  g_assert_false (f2->from_disk);
  fp_workers_free (workers);
  fp_cache_unref (cache);
  fp_thumbs_free (thumbs);
  g_chmod (ro, 0755);
}

static void
test_errors (void)
{
  Session s = { 0 };
  session_start (&s, LRU_FOLDERS, CACHE_MAX_BYTES);
  struct { GFile *f; GIOErrorEnum code; } cases[] = {
    { g_file_new_for_uri ("sftp://example.invalid/x"), G_IO_ERROR_NOT_SUPPORTED },
    { g_file_new_build_filename (run_dir, "nope", NULL), G_IO_ERROR_NOT_FOUND },
    { g_file_new_build_filename (data_dir, "img2.png", NULL), G_IO_ERROR_NOT_DIRECTORY },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      Wait w = { 0 };
      fp_cache_get_frame_async (s.cache, cases[i].f, 0, 64, 0, NULL, on_frame, &w);
      tu_wait_for (&w.done, 1);
      g_assert_error (w.error, G_IO_ERROR, (gint) cases[i].code);
      g_assert_null (w.frame);
      g_clear_error (&w.error);
      g_object_unref (cases[i].f);
    }
  session_stop (&s);
}

/* Many requests, half cancelled, then shutdown with work in flight: every callback arrives once, nothing
 * leaks or crashes (the cache outlives its in-flight requests through their references). */
static void
test_cancel_and_shutdown (void)
{
  g_autofree char *dir = folder_of_images ("busy", 8);
  Session s = { 0 };
  session_start (&s, LRU_FOLDERS, CACHE_MAX_BYTES);
  g_autoptr (GFile) folder = g_file_new_for_path (dir);
  Wait w = { 0 };
  g_autoptr (GPtrArray) cancellables = g_ptr_array_new_with_free_func (g_object_unref);
  for (guint i = 0; i < 40; i++)
    {
      GCancellable *c = g_cancellable_new ();
      g_ptr_array_add (cancellables, c);
      fp_cache_get_frame_async (s.cache, folder, i % 3, 48 + (i % 4) * 16, 0, c, on_frame, &w);
      if (i % 2 == 0)
        g_cancellable_cancel (c);
    }
  /* Shut down immediately; callbacks are delivered by session_stop's loop. */
  session_stop (&s);
  g_assert_cmpuint (w.done, ==, 40);
  g_clear_pointer (&w.frame, fp_frame_free);
  g_clear_error (&w.error);
}

int
main (int argc, char **argv)
{
  setlocale (LC_ALL, "");
  const char *builddir = g_getenv ("G_TEST_BUILDDIR");
  const char *srcdir = g_getenv ("G_TEST_SRCDIR");
  g_assert_nonnull (builddir);
  g_assert_nonnull (srcdir);
  run_dir = tu_make_tmpdir_in (builddir, "cache-run");
  cache_dir = g_build_filename (run_dir, "xdg-cache", NULL);
  g_setenv ("XDG_CACHE_HOME", cache_dir, TRUE);
  disk_dir = g_build_filename (run_dir, "frames", NULL);
  data_dir = g_build_filename (srcdir, "data", NULL);

  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cache/second-launch-from-disk", test_second_launch_from_disk);
  g_test_add_func ("/cache/hover-frames", test_hover_frames);
  g_test_add_func ("/cache/no-media", test_no_media);
  g_test_add_func ("/cache/styles", test_styles);
  g_test_add_func ("/cache/purge-old-versions", test_purge_old_versions);
  g_test_add_func ("/cache/stacked-few-photos", test_stacked_few_photos);
  g_test_add_func ("/cache/invalidation-by-mtime", test_invalidation_by_mtime);
  g_test_add_func ("/cache/sizes-and-corrupt-disk-file", test_sizes_and_corrupt_disk_file);
  g_test_add_func ("/cache/lru-limits", test_lru_limits);
  g_test_add_func ("/cache/rescan-past-failures", test_rescan_past_failures);
  g_test_add_func ("/cache/unwritable-disk-cache", test_unwritable_disk_cache);
  g_test_add_func ("/cache/errors", test_errors);
  g_test_add_func ("/cache/cancel-and-shutdown", test_cancel_and_shutdown);
  int ret = g_test_run ();

  tu_rm_rf (run_dir);
  g_clear_pointer (&run_dir, g_free);
  g_clear_pointer (&cache_dir, g_free);
  g_clear_pointer (&disk_dir, g_free);
  g_clear_pointer (&data_dir, g_free);
  return ret;
}
