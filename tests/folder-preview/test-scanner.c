/* Unit + stress tests for src/scanner.c (PLAN.md §6.5 Phase 2). */
#define GNOME_DESKTOP_USE_UNSTABLE_API
#include <glib/gstdio.h>
#include <libgnome-desktop/gnome-desktop-thumbnail.h>

#include <locale.h>

#include "scanner.h"
#include "test-util.h"

static char *root; /* Owned temp dir for this test run; removed at the end of main. */

/* ---- helpers ---- */

/* Caller owns the result. Asserts success. */
static FpScanResult *
scan_path (const char *path, guint window)
{
  g_autoptr (GFile) folder = g_file_new_for_path (path);
  g_autoptr (GError) error = NULL;
  FpScanResult *r = fp_scan_folder (folder, window, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (r);
  return r;
}

/* Returns the candidates' names joined with spaces; caller frees. */
static char *
names_of (FpScanResult *r)
{
  g_autoptr (GString) s = g_string_new (NULL);
  for (guint i = 0; i < r->candidates->len; i++)
    {
      FpCandidate *c = g_ptr_array_index (r->candidates, i);
      g_string_append_printf (s, "%s%s", i ? " " : "", c->name);
    }
  return g_string_free (g_steal_pointer (&s), FALSE);
}

static guint64
mtime_of (GFile *file)
{
  g_autoptr (GFileInfo) info = g_file_query_info (file, G_FILE_ATTRIBUTE_TIME_MODIFIED, 0, NULL, NULL);
  g_assert_nonnull (info);
  return g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
}

static GnomeDesktopThumbnailFactory *factory; /* Owned; created in main. */

static void
mark_failed (const char *dir, const char *name)
{
  g_autofree char *path = g_build_filename (dir, name, NULL);
  g_autoptr (GFile) f = g_file_new_for_path (path);
  g_autofree char *uri = g_file_get_uri (f);
  g_autoptr (GError) error = NULL;
  g_assert_true (gnome_desktop_thumbnail_factory_create_failed_thumbnail (factory, uri, (time_t) mtime_of (f), NULL, &error));
  g_assert_no_error (error);
}

static void
save_thumbnail (const char *dir, const char *name, time_t mtime)
{
  g_autofree char *path = g_build_filename (dir, name, NULL);
  g_autoptr (GFile) f = g_file_new_for_path (path);
  g_autofree char *uri = g_file_get_uri (f);
  g_autoptr (GdkPixbuf) pb = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 8, 8);
  gdk_pixbuf_fill (pb, 0xff0000ff);
  g_autoptr (GError) error = NULL;
  g_assert_true (gnome_desktop_thumbnail_factory_save_thumbnail (factory, pb, uri, mtime, NULL, &error));
  g_assert_no_error (error);
}

/* ---- classification ---- */

static GFileInfo *
info_new (const char *content_type, GFileType type, gboolean hidden, gboolean backup)
{
  GFileInfo *info = g_file_info_new ();
  if (content_type)
    g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE, content_type);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_STANDARD_TYPE, type);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN, hidden);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP, backup);
  return info;
}

static void
test_classify (void)
{
  struct { const char *ct; GFileType type; gboolean hidden, backup; FpMediaKind want; } cases[] = {
    { "image/png",            G_FILE_TYPE_REGULAR,       FALSE, FALSE, FP_MEDIA_IMAGE },
    { "image/svg+xml",        G_FILE_TYPE_REGULAR,       FALSE, FALSE, FP_MEDIA_IMAGE },
    { "video/mp4",            G_FILE_TYPE_REGULAR,       FALSE, FALSE, FP_MEDIA_VIDEO },
    { "audio/mpeg",           G_FILE_TYPE_REGULAR,       FALSE, FALSE, FP_MEDIA_AUDIO },
    { "audio/flac",           G_FILE_TYPE_REGULAR,       FALSE, FALSE, FP_MEDIA_AUDIO },
    { "text/plain",           G_FILE_TYPE_REGULAR,       FALSE, FALSE, FP_MEDIA_NONE },
    { "application/pdf",      G_FILE_TYPE_REGULAR,       FALSE, FALSE, FP_MEDIA_NONE },
    { "application/zip",      G_FILE_TYPE_REGULAR,       FALSE, FALSE, FP_MEDIA_NONE },
    { "inode/directory",      G_FILE_TYPE_DIRECTORY,     FALSE, FALSE, FP_MEDIA_NONE },
    { "image/png",            G_FILE_TYPE_DIRECTORY,     FALSE, FALSE, FP_MEDIA_NONE },
    { "image/png",            G_FILE_TYPE_REGULAR,       TRUE,  FALSE, FP_MEDIA_NONE },
    { "image/png",            G_FILE_TYPE_REGULAR,       FALSE, TRUE,  FP_MEDIA_NONE },
    { "image/png",            G_FILE_TYPE_SYMBOLIC_LINK, FALSE, FALSE, FP_MEDIA_NONE },
    { "image/png",            G_FILE_TYPE_SPECIAL,       FALSE, FALSE, FP_MEDIA_NONE },
    { NULL,                   G_FILE_TYPE_REGULAR,       FALSE, FALSE, FP_MEDIA_NONE },
    { "imagex/png",           G_FILE_TYPE_REGULAR,       FALSE, FALSE, FP_MEDIA_NONE },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autoptr (GFileInfo) info = info_new (cases[i].ct, cases[i].type, cases[i].hidden, cases[i].backup);
      g_test_message ("case %zu: %s", i, cases[i].ct ? cases[i].ct : "(null)");
      g_assert_cmpint (fp_scanner_classify (info), ==, cases[i].want);
    }

  /* An info with no attributes at all must not emit criticals. */
  g_autoptr (GFileInfo) empty = g_file_info_new ();
  g_assert_cmpint (fp_scanner_classify (empty), ==, FP_MEDIA_NONE);
}

/* ---- ordering ---- */

static void
test_natural_order (void)
{
  g_autofree char *dir = tu_mkdir (root, "order");
  const char *files[] = { "img10.png", "img2.png", "img1.png", "Photo 10.jpg", "Photo 9.jpg",
                          "clip2.mp4", "clip10.mp4", "b.png", "A.png", "c.png" };
  for (gsize i = 0; i < G_N_ELEMENTS (files); i++)
    tu_touch (dir, files[i]);

  g_autoptr (FpScanResult) r = scan_path (dir, 32);
  g_autofree char *got = names_of (r);
  /* Numbers compare by value (2 < 10), letters case-insensitively (LC_ALL=en_US.utf8 in the test env). */
  g_assert_cmpstr (got, ==, "A.png b.png c.png clip2.mp4 clip10.mp4 img1.png img2.png img10.png Photo 9.jpg Photo 10.jpg");
  g_assert_false (r->more_available);
}

/* ---- filtering ---- */

static void
test_filtering (void)
{
  g_autofree char *dir = tu_mkdir (root, "mixed");
  const char *files[] = { "a.png", "b.mp4", "c.mp3", "d.txt", "e.pdf", ".hidden.png", "f.png~",
                          "g.unknownext", "h.svg", "i.zip" };
  for (gsize i = 0; i < G_N_ELEMENTS (files); i++)
    tu_touch (dir, files[i]);
  g_autofree char *sub = tu_mkdir (dir, "sub.png"); /* a folder named like an image */
  tu_touch (sub, "inside.png");                     /* no recursion */

  g_autofree char *target = g_build_filename (dir, "a.png", NULL);
  g_autofree char *link_ok = g_build_filename (dir, "link.png", NULL);
  g_autofree char *link_broken = g_build_filename (dir, "broken.png", NULL);
  g_autoptr (GFile) lf_ok = g_file_new_for_path (link_ok);
  g_autoptr (GFile) lf_broken = g_file_new_for_path (link_broken);
  g_assert_true (g_file_make_symbolic_link (lf_ok, target, NULL, NULL));
  g_assert_true (g_file_make_symbolic_link (lf_broken, "/nonexistent/x.png", NULL, NULL));

  g_autoptr (FpScanResult) r = scan_path (dir, 32);
  g_autofree char *got = names_of (r);
  g_assert_cmpstr (got, ==, "a.png b.mp4 c.mp3 h.svg link.png");
  g_assert_cmpuint (r->n_eligible, ==, 5);

  FpMediaKind want[] = { FP_MEDIA_IMAGE, FP_MEDIA_VIDEO, FP_MEDIA_AUDIO, FP_MEDIA_IMAGE, FP_MEDIA_IMAGE };
  for (guint i = 0; i < r->candidates->len; i++)
    {
      FpCandidate *c = g_ptr_array_index (r->candidates, i);
      g_assert_cmpint (c->kind, ==, want[i]);
      g_assert_nonnull (c->content_type);
      g_assert_nonnull (c->file);
      g_assert_cmpuint (c->mtime, >, 0);
      g_assert_true (g_file_has_parent (c->file, r->folder));
    }
}

/* ---- top-N bound ---- */

static void
test_top_n (void)
{
  g_autofree char *dir = tu_mkdir (root, "hundred");
  tu_touch_n (dir, "img%u.png", 100);

  g_autoptr (FpScanResult) r = scan_path (dir, 16);
  g_assert_cmpuint (r->candidates->len, ==, 16);
  g_assert_cmpuint (r->n_eligible, ==, 100);
  g_assert_true (r->more_available);
  for (guint i = 0; i < 16; i++)
    {
      g_autofree char *want = g_strdup_printf ("img%u.png", i + 1);
      g_assert_cmpstr (((FpCandidate *) g_ptr_array_index (r->candidates, i))->name, ==, want);
    }

  g_autoptr (FpScanResult) all = scan_path (dir, 500);
  g_assert_cmpuint (all->candidates->len, ==, 100);
  g_assert_false (all->more_available);

  g_autoptr (FpScanResult) one = scan_path (dir, 1);
  g_assert_cmpstr (((FpCandidate *) g_ptr_array_index (one->candidates, 0))->name, ==, "img1.png");
}

/* ---- freedesktop cache interplay ---- */

static void
test_cached_failures_are_skipped (void)
{
  g_autofree char *dir = tu_mkdir (root, "music");
  tu_touch_n (dir, "track%02u.mp3", 40);
  for (guint i = 1; i <= 20; i++)
    {
      g_autofree char *name = g_strdup_printf ("track%02u.mp3", i);
      mark_failed (dir, name);
    }

  /* The first 16 by name all failed: the scanner must widen its bound and return tracks 21–36. */
  g_autoptr (FpScanResult) r = scan_path (dir, 16);
  g_assert_cmpuint (r->candidates->len, ==, 16);
  g_assert_cmpstr (((FpCandidate *) g_ptr_array_index (r->candidates, 0))->name, ==, "track21.mp3");
  g_assert_cmpstr (((FpCandidate *) g_ptr_array_index (r->candidates, 15))->name, ==, "track36.mp3");
  g_assert_cmpuint (r->n_eligible, ==, 40);
  g_assert_true (r->more_available);

  /* Everything failed → empty result, no more available. */
  g_autofree char *dead = tu_mkdir (root, "dead");
  tu_touch_n (dead, "x%u.mp3", 50);
  for (guint i = 1; i <= 50; i++)
    {
      g_autofree char *name = g_strdup_printf ("x%u.mp3", i);
      mark_failed (dead, name);
    }
  g_autoptr (FpScanResult) none = scan_path (dead, 16);
  g_assert_cmpuint (none->candidates->len, ==, 0);
  g_assert_false (none->more_available);
}

static void
test_stale_failure_is_not_skipped (void)
{
  g_autofree char *dir = tu_mkdir (root, "stale-fail");
  tu_touch (dir, "song.mp3");
  mark_failed (dir, "song.mp3");
  /* Change the file's mtime: the failure record no longer applies (e.g. cover art was added). */
  g_autofree char *path = g_build_filename (dir, "song.mp3", NULL);
  g_autoptr (GFile) f = g_file_new_for_path (path);
  g_assert_true (g_file_set_attribute_uint64 (f, G_FILE_ATTRIBUTE_TIME_MODIFIED, mtime_of (f) + 100,
                                              G_FILE_QUERY_INFO_NONE, NULL, NULL));
  g_autoptr (FpScanResult) r = scan_path (dir, 16);
  g_assert_cmpuint (r->candidates->len, ==, 1);
}

static void
test_thumbnail_path (void)
{
  g_autofree char *dir = tu_mkdir (root, "thumbs");
  tu_touch (dir, "has.png");
  tu_touch (dir, "none.png");
  tu_touch (dir, "stale.png");
  g_autofree char *has_path = g_build_filename (dir, "has.png", NULL);
  g_autofree char *stale_path = g_build_filename (dir, "stale.png", NULL);
  g_autoptr (GFile) has = g_file_new_for_path (has_path);
  g_autoptr (GFile) stale = g_file_new_for_path (stale_path);
  save_thumbnail (dir, "has.png", (time_t) mtime_of (has));
  save_thumbnail (dir, "stale.png", (time_t) mtime_of (stale) - 100);

  g_autoptr (FpScanResult) r = scan_path (dir, 16);
  g_autofree char *got = names_of (r);
  g_assert_cmpstr (got, ==, "has.png none.png stale.png");
  FpCandidate *c0 = g_ptr_array_index (r->candidates, 0);
  FpCandidate *c1 = g_ptr_array_index (r->candidates, 1);
  FpCandidate *c2 = g_ptr_array_index (r->candidates, 2);
  g_assert_nonnull (c0->thumbnail_path);
  g_assert_true (g_file_test (c0->thumbnail_path, G_FILE_TEST_IS_REGULAR));
  g_assert_true (g_str_has_prefix (c0->thumbnail_path, g_get_user_cache_dir ()));
  g_assert_null (c1->thumbnail_path);
  g_assert_null (c2->thumbnail_path); /* stale thumbnail must not be used */
}

/* ---- errors / edge cases ---- */

static void
expect_error (GFile *folder, GIOErrorEnum code)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (FpScanResult) r = fp_scan_folder (folder, 16, NULL, &error);
  g_assert_null (r);
  g_assert_error (error, G_IO_ERROR, (gint) code);
}

static void
test_errors (void)
{
  g_autoptr (GFile) remote = g_file_new_for_uri ("sftp://example.invalid/photos");
  expect_error (remote, G_IO_ERROR_NOT_SUPPORTED);

  tu_touch (root, "plain.png");
  g_autofree char *plain_path = g_build_filename (root, "plain.png", NULL);
  g_autoptr (GFile) plain = g_file_new_for_path (plain_path);
  expect_error (plain, G_IO_ERROR_NOT_DIRECTORY);

  g_autofree char *missing_path = g_build_filename (root, "does-not-exist", NULL);
  g_autoptr (GFile) missing = g_file_new_for_path (missing_path);
  expect_error (missing, G_IO_ERROR_NOT_FOUND);

  g_autofree char *empty = tu_mkdir (root, "empty");
  g_autoptr (FpScanResult) r = scan_path (empty, 16);
  g_assert_cmpuint (r->candidates->len, ==, 0);
  g_assert_cmpuint (r->n_eligible, ==, 0);

  {
    g_autofree char *locked = tu_mkdir (root, "locked");
    tu_touch (locked, "a.png");
    g_assert_cmpint (g_chmod (locked, 0), ==, 0);
    if (g_access (locked, R_OK) != 0) /* skip when running as root, which ignores permissions */
      {
        g_autoptr (GFile) lf = g_file_new_for_path (locked);
        expect_error (lf, G_IO_ERROR_PERMISSION_DENIED);
      }
    g_chmod (locked, 0755);
  }

  g_autoptr (GCancellable) cancelled = g_cancellable_new ();
  g_cancellable_cancel (cancelled);
  g_autoptr (GFile) ef = g_file_new_for_path (empty);
  g_autoptr (GError) error = NULL;
  g_autoptr (FpScanResult) none = fp_scan_folder (ef, 16, cancelled, &error);
  g_assert_null (none);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

static void
test_folder_mtime_changes (void)
{
  g_autofree char *dir = tu_mkdir (root, "mtime");
  tu_touch (dir, "a.png");
  g_autoptr (FpScanResult) r1 = scan_path (dir, 16);
  g_assert_cmpuint (r1->mtime_sec, >, 0);

  g_usleep (20 * 1000);
  tu_touch (dir, "b.png");
  g_autoptr (FpScanResult) r2 = scan_path (dir, 16);
  g_assert_true (r2->mtime_sec != r1->mtime_sec || r2->mtime_usec != r1->mtime_usec);
}

/* Two names that look the same but are different bytes (é as NFC vs NFD) may get equal collate keys;
 * the order must still be total and stable (raw-name tie-break). */
/* GLib's convention: names listed in a folder's ".hidden" file are hidden. */
static void
test_hidden_list (void)
{
  g_autofree char *dir = tu_mkdir (root, "hidden-list");
  tu_touch (dir, "shown.png");
  tu_touch (dir, "secret.png");
  tu_touch (dir, "also secret.jpg");
  g_autofree char *list = g_build_filename (dir, ".hidden", NULL);
  g_assert_true (g_file_set_contents (list, "secret.png\nalso secret.jpg\n", -1, NULL));
  g_autoptr (FpScanResult) r = scan_path (dir, 16);
  g_autofree char *got = names_of (r);
  g_assert_cmpstr (got, ==, "shown.png");
}

static void
test_equal_collate_keys (void)
{
  g_autofree char *dir = tu_mkdir (root, "unicode");
  const char *nfc = "\xc3\xa9.png";  /* é precomposed */
  const char *nfd = "e\xcc\x81.png"; /* e + combining acute */
  tu_touch (dir, nfc);
  tu_touch (dir, nfd);
  g_autoptr (FpScanResult) r1 = scan_path (dir, 16);
  g_autoptr (FpScanResult) r2 = scan_path (dir, 16);
  g_assert_cmpuint (r1->candidates->len, ==, 2);
  g_autofree char *n1 = names_of (r1);
  g_autofree char *n2 = names_of (r2);
  g_assert_cmpstr (n1, ==, n2);
  g_autofree char *k1 = g_utf8_collate_key_for_filename (nfc, -1);
  g_autofree char *k2 = g_utf8_collate_key_for_filename (nfd, -1);
  if (g_strcmp0 (k1, k2) == 0)
    {
      g_autofree char *want = g_strdup_printf ("%s %s", nfd, nfc); /* byte order: 'e' < 0xc3 */
      g_assert_cmpstr (n1, ==, want);
    }
  else
    g_test_message ("collate keys differ in this locale; tie-break not exercised");
}

static void
test_free_null (void)
{
  fp_candidate_free (NULL);
  fp_scan_result_unref (NULL);
  fp_workers_free (NULL);
}

/* Cancel synchronous scans of a 10k-entry folder at random moments from another thread, so the
 * cancellation points inside enumeration and candidate building are hit and leak-checked. */
typedef struct
{
  GFile        *folder;       /* Borrowed. */
  GCancellable *cancellable;  /* Borrowed. */
  gboolean      cancelled;
  gboolean      ok;
} SyncScan;

static gpointer
sync_scan_thread (gpointer data)
{
  SyncScan *ss = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (FpScanResult) r = fp_scan_folder (ss->folder, 16, ss->cancellable, &error);
  if (r != NULL)
    ss->ok = TRUE;
  else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    ss->cancelled = TRUE;
  else
    g_error ("unexpected: %s", error->message);
  return NULL;
}

static char *huge_dir; /* Owned; 10 000 files, created lazily by test_cancel_midway. */

static void
test_cancel_midway (void)
{
  huge_dir = tu_mkdir (root, "huge");
  tu_touch_n (huge_dir, "f%05u.png", 10000);
  g_autoptr (GFile) folder = g_file_new_for_path (huge_dir);
  guint n_cancelled = 0, n_ok = 0;
  for (guint i = 0; i < 60; i++)
    {
      g_autoptr (GCancellable) c = g_cancellable_new ();
      SyncScan ss = { .folder = folder, .cancellable = c };
      GThread *t = g_thread_new ("scan", sync_scan_thread, &ss);
      g_usleep ((gulong) g_test_rand_int_range (0, 25000));
      g_cancellable_cancel (c);
      g_thread_join (t);
      n_cancelled += ss.cancelled;
      n_ok += ss.ok;
    }
  g_test_message ("mid-way cancel: cancelled=%u completed=%u", n_cancelled, n_ok);
  g_assert_cmpuint (n_cancelled + n_ok, ==, 60);
  g_assert_cmpuint (n_cancelled, >, 0);
}

/* ---- async ---- */

typedef struct
{
  guint         done;
  guint         ok;
  guint         cancelled;
  GPtrArray    *order;   /* Owned: g_strdup'd labels in completion order (may be NULL). */
  const char   *label;   /* Borrowed static label of this request. */
} Tally;

typedef struct
{
  Tally      *tally;   /* Borrowed; outlives the request. */
  const char *label;   /* Borrowed static string. */
} Req;

static void
on_scanned (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) source;
  Req *req = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (FpScanResult) r = fp_scan_finish (res, &error);
  if (r != NULL)
    req->tally->ok++;
  else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    req->tally->cancelled++;
  else
    g_error ("unexpected scan error: %s", error->message);
  if (req->tally->order != NULL)
    g_ptr_array_add (req->tally->order, g_strdup (req->label));
  req->tally->done++;
  g_free (req);
}

static Req *
req_new (Tally *t, const char *label)
{
  Req *r = g_new0 (Req, 1);
  r->tally = t;
  r->label = label;
  return r;
}

static char *big_dir; /* Owned; 3000 files, created in main. */

static void
test_async_basic (void)
{
  g_autoptr (FpWorkers) workers = fp_workers_new (2);
  g_autoptr (GFile) folder = g_file_new_for_path (big_dir);
  Tally t = { 0 };
  fp_scan_async (workers, folder, 16, 0, NULL, on_scanned, req_new (&t, "a"));
  tu_wait_for (&t.done, 1);
  g_assert_cmpuint (t.ok, ==, 1);
}

static void
test_async_priority (void)
{
  /* One thread: the first (highest-priority) job occupies it; the queue must then run Y (0) before X (10). */
  g_autoptr (FpWorkers) workers = fp_workers_new (1);
  g_autoptr (GFile) folder = g_file_new_for_path (big_dir);
  Tally t = { 0 };
  t.order = g_ptr_array_new_with_free_func (g_free);
  fp_scan_async (workers, folder, 16, -100, NULL, on_scanned, req_new (&t, "first"));
  fp_scan_async (workers, folder, 16, 10, NULL, on_scanned, req_new (&t, "X"));
  fp_scan_async (workers, folder, 16, 10, NULL, on_scanned, req_new (&t, "X2"));
  fp_scan_async (workers, folder, 16, 0, NULL, on_scanned, req_new (&t, "Y"));
  tu_wait_for (&t.done, 4);
  g_assert_cmpstr (g_ptr_array_index (t.order, 0), ==, "first");
  g_assert_cmpstr (g_ptr_array_index (t.order, 1), ==, "Y");
  g_assert_cmpstr (g_ptr_array_index (t.order, 2), ==, "X");   /* FIFO among equal priorities */
  g_assert_cmpstr (g_ptr_array_index (t.order, 3), ==, "X2");
  g_ptr_array_unref (t.order);
}

/* PLAN.md §4.2: repeated scan → cancel → rescan cycles. */
static void
test_async_cancel_stress (void)
{
  g_autoptr (FpWorkers) workers = fp_workers_new (2);
  g_autoptr (GFile) folder = g_file_new_for_path (big_dir);
  Tally t = { 0 };
  const guint cycles = 300;
  for (guint i = 0; i < cycles; i++)
    {
      g_autoptr (GCancellable) c = g_cancellable_new ();
      fp_scan_async (workers, folder, 16, 0, c, on_scanned, req_new (&t, "c"));
      if (i % 3 == 1)
        g_main_context_iteration (NULL, FALSE); /* let some get further before cancelling */
      g_cancellable_cancel (c);
      fp_scan_async (workers, folder, 16, 0, NULL, on_scanned, req_new (&t, "r"));
    }
  tu_wait_for (&t.done, 2 * cycles);
  g_assert_cmpuint (t.ok + t.cancelled, ==, 2 * cycles);
  g_assert_cmpuint (t.ok, >=, cycles); /* every rescan succeeds */
  g_test_message ("ok=%u cancelled=%u", t.ok, t.cancelled);
}

static void
test_free_with_pending (void)
{
  g_autoptr (GFile) folder = g_file_new_for_path (big_dir);
  Tally t = { 0 };
  FpWorkers *workers = fp_workers_new (1);
  for (int i = 0; i < 50; i++)
    fp_scan_async (workers, folder, 16, 0, NULL, on_scanned, req_new (&t, "p"));
  fp_workers_free (workers); /* must not hang, crash or lose callbacks */
  tu_wait_for (&t.done, 50);
  g_assert_cmpuint (t.ok + t.cancelled, ==, 50);
  g_assert_cmpuint (t.cancelled, >=, 45); /* at most the one running (plus a race or two) finished */
}

static void
test_many_scanners (void)
{
  g_autoptr (GFile) folder = g_file_new_for_path (big_dir);
  Tally t = { 0 };
  for (int i = 0; i < 100; i++)
    {
      FpWorkers *workers = fp_workers_new (2);
      fp_scan_async (workers, folder, 16, 0, NULL, on_scanned, req_new (&t, "m"));
      fp_workers_free (workers);
    }
  tu_wait_for (&t.done, 100);
}

int
main (int argc, char **argv)
{
  root = tu_make_tmpdir ("fp-test-scanner");
  /* Isolate the freedesktop thumbnail cache from the user's: GLib and gnome-desktop read it from
   * g_get_user_cache_dir(), which is computed on first use, so this must happen first. */
  g_autofree char *cache = g_build_filename (root, "xdg-cache", NULL);
  g_setenv ("XDG_CACHE_HOME", cache, TRUE);

  /* Nautilus (a GTK app) runs in the user's locale; collation needs this to honour LC_ALL. */
  setlocale (LC_ALL, "");
  g_test_init (&argc, &argv, NULL);
  factory = gnome_desktop_thumbnail_factory_new (GNOME_DESKTOP_THUMBNAIL_SIZE_LARGE);
  big_dir = tu_mkdir (root, "big");
  tu_touch_n (big_dir, "file%u.jpg", 3000);

  g_test_add_func ("/scanner/classify", test_classify);
  g_test_add_func ("/scanner/natural-order", test_natural_order);
  g_test_add_func ("/scanner/filtering", test_filtering);
  g_test_add_func ("/scanner/top-n", test_top_n);
  g_test_add_func ("/scanner/cached-failures-are-skipped", test_cached_failures_are_skipped);
  g_test_add_func ("/scanner/stale-failure-is-not-skipped", test_stale_failure_is_not_skipped);
  g_test_add_func ("/scanner/thumbnail-path", test_thumbnail_path);
  g_test_add_func ("/scanner/errors", test_errors);
  g_test_add_func ("/scanner/folder-mtime-changes", test_folder_mtime_changes);
  g_test_add_func ("/scanner/hidden-list", test_hidden_list);
  g_test_add_func ("/scanner/equal-collate-keys", test_equal_collate_keys);
  g_test_add_func ("/scanner/free-null", test_free_null);
  g_test_add_func ("/scanner/cancel-midway", test_cancel_midway);
  g_test_add_func ("/scanner/async-basic", test_async_basic);
  g_test_add_func ("/scanner/async-priority", test_async_priority);
  g_test_add_func ("/scanner/async-cancel-stress", test_async_cancel_stress);
  g_test_add_func ("/scanner/free-with-pending", test_free_with_pending);
  g_test_add_func ("/scanner/many-scanners", test_many_scanners);
  int ret = g_test_run ();

  g_clear_object (&factory);
  tu_rm_rf (root);
  g_clear_pointer (&big_dir, g_free);
  g_clear_pointer (&huge_dir, g_free);
  g_clear_pointer (&root, g_free);
  return ret;
}
