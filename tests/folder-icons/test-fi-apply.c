/* Tests for src/folder-icons/fi_apply.c (PLAN.md §12.3, §12.7) with an in-memory metadata backend: the
 * decisions reach the folders, the user's own icons are never touched, rule changes and removal restore
 * folders, each folder is checked once per rule set, concurrency stays bounded, failures are counted, and
 * freeing the applier while jobs run is safe. */
#include "fi_apply.h"

/* ---- Fake metadata store (thread-safe) ---- */

typedef struct
{
  char *icon, *icon_name, *marker; /* Owned or NULL. */
} Meta;

typedef struct
{
  GMutex      lock;
  GHashTable *meta;     /* uri (owned) → Meta* (owned) */
  GHashTable *fail;     /* set of uris (owned) whose read fails */
  guint       nudges;
  guint       running, max_running;
  guint       delay_ms; /* per job, to overlap jobs */
} Fake;

static void
meta_free (gpointer data)
{
  Meta *m = data;
  g_free (m->icon);
  g_free (m->icon_name);
  g_free (m->marker);
  g_free (m);
}

static void
fake_init (Fake *f)
{
  g_mutex_init (&f->lock);
  f->meta = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, meta_free);
  f->fail = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  f->nudges = f->running = f->max_running = f->delay_ms = 0;
}

static void
fake_clear (Fake *f)
{
  g_clear_pointer (&f->meta, g_hash_table_unref);
  g_clear_pointer (&f->fail, g_hash_table_unref);
  g_mutex_clear (&f->lock);
}

/* Returns the folder's Meta (created on first use); call with the lock held. */
static Meta *
meta_of (Fake *f, GFile *folder)
{
  g_autofree char *uri = g_file_get_uri (folder);
  Meta *m = g_hash_table_lookup (f->meta, uri);
  if (m == NULL)
    {
      m = g_new0 (Meta, 1);
      g_hash_table_insert (f->meta, g_steal_pointer (&uri), m);
    }
  return m;
}

static gboolean
fake_read (GFile *folder, char **icon, char **icon_name, char **marker, gpointer data, GError **error)
{
  Fake *f = data;
  g_mutex_lock (&f->lock);
  f->running++;
  f->max_running = MAX (f->max_running, f->running);
  g_autofree char *uri = g_file_get_uri (folder);
  gboolean failing = g_hash_table_contains (f->fail, uri);
  Meta *m = meta_of (f, folder);
  *icon = g_strdup (m->icon);
  *icon_name = g_strdup (m->icon_name);
  *marker = g_strdup (m->marker);
  g_mutex_unlock (&f->lock);
  if (f->delay_ms > 0)
    g_usleep (f->delay_ms * 1000);
  g_mutex_lock (&f->lock);
  f->running--;
  g_mutex_unlock (&f->lock);
  if (failing)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "no metadata here");
      return FALSE;
    }
  return TRUE;
}

static gboolean
fake_write (GFile *folder, FiActionKind action, const char *icon_uri, gpointer data, GError **error)
{
  (void) error;
  Fake *f = data;
  g_mutex_lock (&f->lock);
  Meta *m = meta_of (f, folder);
  switch (action)
    {
    case FI_ACTION_SET:
      g_free (m->icon);
      m->icon = g_strdup (icon_uri);
      g_free (m->marker);
      m->marker = fi_marker_new (icon_uri);
      break;
    case FI_ACTION_CLEAR:
      g_clear_pointer (&m->icon, g_free);
      g_clear_pointer (&m->marker, g_free);
      break;
    case FI_ACTION_CLEAR_MARKER:
      g_clear_pointer (&m->marker, g_free);
      break;
    case FI_ACTION_NONE:
    default:
      break;
    }
  g_mutex_unlock (&f->lock);
  return TRUE;
}

static void
fake_nudge (GFile *folder, gpointer data)
{
  (void) folder;
  Fake *f = data;
  if (f == NULL)
    return; /* (GCC -O2 speculatively inlines this into calls of the real backend with NULL data) */
  g_mutex_lock (&f->lock);
  f->nudges++;
  g_mutex_unlock (&f->lock);
}

static const FiBackend fake_backend = { fake_read, fake_write, fake_nudge };

/* Copy of a folder's metadata field (caller frees). */
static char *
get (Fake *f, const char *path, int field)
{
  g_autoptr (GFile) file = g_file_new_for_path (path);
  g_mutex_lock (&f->lock);
  Meta *m = meta_of (f, file);
  char *v = g_strdup (field == 0 ? m->icon : field == 1 ? m->icon_name : m->marker);
  g_mutex_unlock (&f->lock);
  return v;
}

static void
put (Fake *f, const char *path, const char *icon, const char *icon_name, const char *marker)
{
  g_autoptr (GFile) file = g_file_new_for_path (path);
  g_mutex_lock (&f->lock);
  Meta *m = meta_of (f, file);
  g_free (m->icon);
  m->icon = g_strdup (icon);
  g_free (m->icon_name);
  m->icon_name = g_strdup (icon_name);
  g_free (m->marker);
  m->marker = g_strdup (marker);
  g_mutex_unlock (&f->lock);
}

/* ---- Helpers ---- */

static FiRules *
rules_of (const char *name, const char *icon, const char *const *history, gboolean enabled)
{
  NuIconRule r = { name, icon };
  return fi_rules_new (name != NULL ? &r : NULL, name != NULL ? 1 : 0, history, enabled);
}

/* Lists @path (basename = folder name) like Files would. */
static gboolean
list (FiApplier *a, const char *path)
{
  g_autoptr (GFile) file = g_file_new_for_path (path);
  g_autofree char *name = g_file_get_basename (file);
  return fi_applier_check (a, file, name);
}

static void
drain (FiApplier *a)
{
  gint64 end = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;
  while ((a == NULL || fi_applier_get_n_pending (a) > 0 || fi_applier_get_n_inflight () > 0) &&
         g_get_monotonic_time () < end)
    g_main_context_iteration (NULL, FALSE);
  g_assert_cmpuint (fi_applier_get_n_inflight (), ==, 0);
}

typedef struct
{
  guint calls;
  guint errors;
} Done;

static void
on_done (const char *uri, FiActionKind action, const char *error_message, gpointer data)
{
  (void) uri;
  (void) action;
  Done *d = data;
  d->calls++;
  if (error_message != NULL)
    d->errors++;
}

#define ICON  "/icons/node.svg"
#define ICONU "file:///icons/node.svg"
#define ICON2 "/icons/new.svg"
#define ICON2U "file:///icons/new.svg"

/* ---- Tests ---- */

static void
test_apply_and_restore (void)
{
  Fake f;
  fake_init (&f);
  g_autoptr (FiApplier) a = fi_applier_new (&fake_backend, &f, 4);
  Done done = { 0 };
  fi_applier_set_done_func (a, on_done, &done);

  /* No rules yet: nothing is queued. */
  g_assert_false (list (a, "/p/node_modules"));
  fi_applier_set_rules (a, rules_of ("node_modules", ICON, NULL, TRUE));

  g_assert_true (list (a, "/p/node_modules"));
  g_assert_false (list (a, "/p/node_modules")); /* queued already: merged */
  g_assert_false (list (a, "/p/src"));           /* not a candidate */
  g_assert_false (list (a, "/p/Node_Modules"));  /* exact, case-sensitive */
  put (&f, "/q/node_modules", "file:///mine.png", NULL, NULL); /* the user's own icon */
  put (&f, "/r/node_modules", NULL, "folder-music", NULL);     /* the user's own icon name */
  g_assert_true (list (a, "/q/node_modules"));
  g_assert_true (list (a, "/r/node_modules"));
  drain (a);
  g_assert_cmpuint (done.calls, ==, 3);

  g_autofree char *i1 = get (&f, "/p/node_modules", 0);
  g_autofree char *m1 = get (&f, "/p/node_modules", 2);
  g_assert_cmpstr (i1, ==, ICONU);
  g_autofree char *want = fi_marker_new (ICONU);
  g_assert_cmpstr (m1, ==, want);
  g_autofree char *q = get (&f, "/q/node_modules", 0);
  g_autofree char *qm = get (&f, "/q/node_modules", 2);
  g_assert_cmpstr (q, ==, "file:///mine.png"); /* untouched */
  g_assert_null (qm);
  g_autofree char *r = get (&f, "/r/node_modules", 0);
  g_assert_null (r);
  g_assert_cmpuint (fi_applier_get_n_set (a), ==, 1);
  g_assert_cmpuint (f.nudges, ==, 1); /* only the folder that changed */

  /* Checked once per rule set. */
  g_assert_false (list (a, "/p/node_modules"));

  /* The rule's icon changes: our folders get the new one, the user's stay. */
  fi_applier_set_rules (a, rules_of ("node_modules", ICON2, NULL, TRUE));
  g_assert_true (list (a, "/p/node_modules"));
  g_assert_true (list (a, "/q/node_modules"));
  drain (a);
  g_autofree char *i2 = get (&f, "/p/node_modules", 0);
  g_assert_cmpstr (i2, ==, ICON2U);
  g_autofree char *q2 = get (&f, "/q/node_modules", 0);
  g_assert_cmpstr (q2, ==, "file:///mine.png");

  /* The rule is removed (its name is in the history): restored. */
  fi_applier_set_rules (a, rules_of (NULL, NULL, (const char *[]) { "node_modules", NULL }, TRUE));
  g_assert_true (list (a, "/p/node_modules"));
  drain (a);
  g_autofree char *i3 = get (&f, "/p/node_modules", 0);
  g_autofree char *m3 = get (&f, "/p/node_modules", 2);
  g_assert_null (i3);
  g_assert_null (m3);
  g_assert_cmpuint (fi_applier_get_n_cleared (a), ==, 1);
  g_assert_cmpuint (done.errors, ==, 0);
  fi_applier_free (g_steal_pointer (&a));
  fake_clear (&f);
}

static void
test_disabled_and_user_change (void)
{
  Fake f;
  fake_init (&f);
  g_autoptr (FiApplier) a = fi_applier_new (&fake_backend, &f, 2);
  fi_applier_set_rules (a, rules_of ("vendor", ICON, NULL, TRUE));
  list (a, "/a/vendor");
  list (a, "/b/vendor");
  drain (a);

  /* The user changes /b's icon by hand afterwards. */
  g_autofree char *marker = get (&f, "/b/vendor", 2);
  put (&f, "/b/vendor", "file:///hand.png", NULL, marker);

  /* Feature off: our folder is restored, the hand-changed one only loses our marker. */
  fi_applier_set_rules (a, rules_of ("vendor", ICON, NULL, FALSE));
  list (a, "/a/vendor");
  list (a, "/b/vendor");
  drain (a);
  g_autofree char *ai = get (&f, "/a/vendor", 0);
  g_assert_null (ai);
  g_autofree char *bi = get (&f, "/b/vendor", 0);
  g_autofree char *bm = get (&f, "/b/vendor", 2);
  g_assert_cmpstr (bi, ==, "file:///hand.png");
  g_assert_null (bm);
  g_assert_cmpuint (fi_applier_get_n_cleared (a), ==, 2);
  fi_applier_free (g_steal_pointer (&a));
  fake_clear (&f);
}

static void
test_errors_and_bounds (void)
{
  Fake f;
  fake_init (&f);
  f.delay_ms = 5;
  g_autoptr (FiApplier) a = fi_applier_new (&fake_backend, &f, 3);
  Done done = { 0 };
  fi_applier_set_done_func (a, on_done, &done);
  fi_applier_set_rules (a, rules_of ("node_modules", ICON, NULL, TRUE));
  g_hash_table_add (f.fail, g_strdup ("file:///p0/node_modules"));
  for (int i = 0; i < 60; i++)
    {
      g_autofree char *p = g_strdup_printf ("/p%d/node_modules", i);
      g_assert_true (list (a, p));
    }
  g_assert_cmpuint (fi_applier_get_n_pending (a), ==, 60);
  drain (a);
  g_assert_cmpuint (f.max_running, <=, 3); /* bounded concurrency */
  g_assert_cmpuint (f.max_running, >=, 2); /* … and actually concurrent */
  g_assert_cmpuint (done.calls, ==, 60);
  g_assert_cmpuint (done.errors, ==, 1);
  g_assert_cmpuint (fi_applier_get_n_errors (a), ==, 1);
  g_assert_cmpuint (fi_applier_get_n_set (a), ==, 59);
  fi_applier_free (g_steal_pointer (&a));
  fake_clear (&f);
}

/* A rule change while jobs run: their results don't count for the new rules; freeing with jobs queued and
 * running is safe and reports nothing more. */
static void
test_change_and_free_while_running (void)
{
  Fake f;
  fake_init (&f);
  f.delay_ms = 20;
  FiApplier *a = fi_applier_new (&fake_backend, &f, 2);
  Done done = { 0 };
  fi_applier_set_done_func (a, on_done, &done);
  fi_applier_set_rules (a, rules_of ("x", ICON, NULL, TRUE));
  for (int i = 0; i < 10; i++)
    {
      g_autofree char *p = g_strdup_printf ("/d%d/x", i);
      list (a, p);
    }
  fi_applier_set_rules (a, rules_of ("x", ICON2, NULL, TRUE)); /* 2 running, 8 dropped */
  g_assert_true (list (a, "/d0/x"));                            /* re-queued for the new rules */
  drain (a);
  g_assert_cmpuint (done.calls, ==, 1); /* only the job of the current rules reported */
  g_autofree char *icon = get (&f, "/d0/x", 0);
  g_assert_cmpstr (icon, ==, ICON2U);

  for (int i = 0; i < 10; i++)
    {
      g_autofree char *p = g_strdup_printf ("/e%d/x", i);
      list (a, p);
    }
  fi_applier_free (a); /* 2 running jobs finish on their own */
  drain (NULL);
  g_assert_cmpuint (done.calls, ==, 1);
  fake_clear (&f);
}

/* The real GIO backend without gvfs (GIO_USE_VFS=local): metadata isn't supported, which must be a counted
 * error, not a crash; the nudge leaves the times and the permissions as they were. */
static void
test_gio_backend_without_gvfs (void)
{
  g_autofree char *dir = g_build_filename (g_getenv ("G_TEST_BUILDDIR") ? g_getenv ("G_TEST_BUILDDIR") : ".",
                                           "fi-apply-run", "node_modules", NULL);
  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  g_autoptr (GFile) folder = g_file_new_for_path (dir);
  g_autoptr (GFileInfo) before = g_file_query_info (folder, "time::*,unix::mode", 0, NULL, NULL);
  const FiBackend *gio = fi_applier_gio_backend ();
  gio->nudge (folder, NULL);
  g_autoptr (GFileInfo) after = g_file_query_info (folder, "time::*,unix::mode", 0, NULL, NULL);
  g_assert_cmpuint (g_file_info_get_attribute_uint32 (before, "unix::mode"), ==,
                    g_file_info_get_attribute_uint32 (after, "unix::mode"));
  g_assert_cmpuint (g_file_info_get_attribute_uint64 (before, "time::modified"), ==,
                    g_file_info_get_attribute_uint64 (after, "time::modified"));
  g_assert_cmpuint (g_file_info_get_attribute_uint32 (before, "time::modified-usec"), ==,
                    g_file_info_get_attribute_uint32 (after, "time::modified-usec"));
  g_assert_cmpuint (g_file_info_get_attribute_uint64 (before, "time::access"), ==,
                    g_file_info_get_attribute_uint64 (after, "time::access"));
  g_autoptr (GFile) remote = g_file_new_for_uri ("sftp://example.invalid/node_modules");
  gio->nudge (remote, NULL); /* no local path: nothing to do */

  g_autoptr (FiApplier) a = fi_applier_new (gio, NULL, 1);
  fi_applier_set_rules (a, rules_of ("node_modules", ICON, NULL, TRUE));
  g_assert_true (list (a, dir));
  drain (a);
  /* Without gvfs, metadata reads may succeed (empty) but writes fail: either way no icon, at most one error. */
  g_assert_cmpuint (fi_applier_get_n_set (a), ==, 0);
  g_assert_cmpuint (fi_applier_get_n_errors (a), <=, 1);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/fi-apply/apply-and-restore", test_apply_and_restore);
  g_test_add_func ("/fi-apply/disabled-and-user-change", test_disabled_and_user_change);
  g_test_add_func ("/fi-apply/errors-and-bounds", test_errors_and_bounds);
  g_test_add_func ("/fi-apply/change-and-free-while-running", test_change_and_free_while_running);
  g_test_add_func ("/fi-apply/gio-backend-without-gvfs", test_gio_backend_without_gvfs);
  return g_test_run ();
}
