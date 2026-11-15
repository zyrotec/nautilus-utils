/* Git parsers (PLAN.md §14.4 Phase 1): porcelain v2 status, branches, log, errors, progress, URLs. */
#include <string.h>

#include "gt_parse.h"

/* Builds NUL-separated test data from @parts (NULL-terminated), each part followed by '\0'. */
static GBytes *
z (const char *const *parts)
{
  GByteArray *a = g_byte_array_new ();
  for (int i = 0; parts[i] != NULL; i++)
    g_byte_array_append (a, (const guint8 *) parts[i], (guint) strlen (parts[i]) + 1);
  return g_byte_array_free_to_bytes (a);
}

static GtState
state (GtStatus *st, const char *path, gboolean dir)
{
  return gt_status_state (st, path, dir);
}

static void
test_status (void)
{
  const char *const parts[] = {
    "# branch.oid 1234567890abcdef1234567890abcdef12345678",
    "# branch.head main",
    "# branch.upstream origin/main",
    "# branch.ab +2 -1",
    "1 .M N... 100644 100644 100644 aaaa bbbb kept.txt",
    "1 A. N... 000000 100644 100644 0000 cccc src/new file.c",
    "1 D. N... 100644 000000 000000 dddd 0000 gone.txt",
    "2 R. N... 100644 100644 100644 eeee eeee R100 docs/after.md", "docs/before.md",
    "u UU N... 100644 100644 100644 100644 ffff ffff ffff both.txt",
    "? untracked dir/",
    "? notes.txt",
    "! node_modules/",
    "! build.log",
    NULL,
  };
  g_autoptr (GBytes) data = z (parts);
  gsize len = 0;
  const char *d = g_bytes_get_data (data, &len);
  g_autoptr (GtStatus) st = gt_status_parse (d, len);
  g_assert_cmpstr (st->oid, ==, "1234567890abcdef1234567890abcdef12345678");
  g_assert_cmpstr (st->head, ==, "main");
  g_assert_cmpstr (st->upstream, ==, "origin/main");
  g_assert_true (st->has_ab);
  g_assert_cmpint (st->ahead, ==, 2);
  g_assert_cmpint (st->behind, ==, 1);
  g_assert_cmpint (state (st, "kept.txt", FALSE), ==, GT_STATE_MODIFIED);
  g_assert_cmpint (state (st, "src/new file.c", FALSE), ==, GT_STATE_ADDED);
  g_assert_cmpint (state (st, "gone.txt", FALSE), ==, GT_STATE_DELETED);
  g_assert_cmpint (state (st, "docs/after.md", FALSE), ==, GT_STATE_RENAMED);
  g_assert_cmpint (state (st, "docs/before.md", FALSE), ==, GT_STATE_NONE); /* the old name isn't an entry */
  g_assert_cmpint (state (st, "both.txt", FALSE), ==, GT_STATE_CONFLICT);
  g_assert_cmpint (state (st, "untracked dir", TRUE), ==, GT_STATE_UNTRACKED);
  g_assert_cmpint (state (st, "untracked dir/inner/x.c", FALSE), ==, GT_STATE_UNTRACKED);
  g_assert_cmpint (state (st, "notes.txt", FALSE), ==, GT_STATE_UNTRACKED);
  g_assert_cmpint (state (st, "node_modules", TRUE), ==, GT_STATE_IGNORED);
  g_assert_cmpint (state (st, "node_modules/a/b.js", FALSE), ==, GT_STATE_IGNORED);
  g_assert_cmpint (state (st, "build.log", FALSE), ==, GT_STATE_IGNORED);
  g_assert_cmpint (state (st, "src", TRUE), ==, GT_STATE_CONTAINS);
  g_assert_cmpint (state (st, "docs", TRUE), ==, GT_STATE_CONTAINS);
  g_assert_cmpint (state (st, "README.md", FALSE), ==, GT_STATE_NONE);
  g_assert_cmpint (state (st, "other", TRUE), ==, GT_STATE_NONE);
  g_assert_cmpstr (gt_state_label (GT_STATE_CONTAINS), ==, "Contains changes");
  g_assert_cmpstr (gt_state_label (GT_STATE_NONE), ==, "");
}

static void
test_status_detached_initial (void)
{
  const char *const parts[] = { "# branch.oid (initial)", "# branch.head (detached)", NULL };
  g_autoptr (GBytes) data = z (parts);
  gsize len = 0;
  const char *d = g_bytes_get_data (data, &len);
  g_autoptr (GtStatus) st = gt_status_parse (d, len);
  g_assert_null (st->oid);
  g_assert_null (st->head);
  g_assert_null (st->upstream);
  g_assert_false (st->has_ab);
  /* Empty and garbage input. */
  g_autoptr (GtStatus) empty = gt_status_parse (NULL, 0);
  g_assert_cmpuint (g_hash_table_size (empty->files), ==, 0);
  const char junk[] = "1\0001 X\0? \0u\0# branch.ab nonsense";
  g_autoptr (GtStatus) j = gt_status_parse (junk, sizeof junk);
  g_assert_false (j->has_ab);
}

static void
test_branches (void)
{
  /* for-each-ref: fields NUL-separated, one ref per line. */
  static const char data[] =
    "refs/remotes/origin/HEAD\0 \0\0abc1234\0" "1700000000\0Merge\n"
    "refs/remotes/origin/main\0 \0\0abc1234\0" "1700000000\0Merge\n"
    "refs/heads/zeta\0 \0\0def5678\0" "1700000100\0Work on zeta\n"
    "refs/heads/main\0*\0origin/main\0abc1234\0" "1700000000\0Merge\n"
    "refs/heads/alpha\0 \0origin/alpha\0" "0011223\0" "1600000000\0Old\n";
  g_autoptr (GPtrArray) b = gt_branches_parse (data, sizeof data - 1);
  g_assert_cmpuint (b->len, ==, 4);
  GtBranch *b0 = g_ptr_array_index (b, 0), *b1 = g_ptr_array_index (b, 1), *b2 = g_ptr_array_index (b, 2),
           *b3 = g_ptr_array_index (b, 3);
  g_assert_cmpstr (b0->name, ==, "main");
  g_assert_true (b0->current);
  g_assert_cmpstr (b0->upstream, ==, "origin/main");
  g_assert_cmpstr (b1->name, ==, "alpha");
  g_assert_cmpstr (b2->name, ==, "zeta");
  g_assert_null (b2->upstream);
  g_assert_cmpstr (b2->subject, ==, "Work on zeta");
  g_assert_cmpint (b2->date, ==, 1700000100);
  g_assert_cmpstr (b3->name, ==, "origin/main");
  g_assert_true (b3->remote);
  g_autoptr (GPtrArray) none = gt_branches_parse (NULL, 0);
  g_assert_cmpuint (none->len, ==, 0);
}

static void
test_log (void)
{
  /* GT_LOG_FORMAT with --name-only -z: \x1e hash \0 author \0 date \0 subject \0 \n name \0 name \0 … */
  static const char data[] =
    "\x1e" "c3\0Ann\0" "300\0Third\0\nsrc/a.c\0src/sub/x.c\0"
    "\x1e" "c2\0Bob\0" "200\0Second\0\nsrc/b.c\0README.md\0"
    "\x1e" "c1\0Cy\0" "100\0First\0\nsrc/a.c\0src/b.c\0README.md\0top.txt\0";
  g_autoptr (GHashTable) src = gt_log_last_commits (data, sizeof data - 1, "src");
  g_assert_cmpuint (g_hash_table_size (src), ==, 3);
  GtCommit *a = g_hash_table_lookup (src, "a.c"), *sub = g_hash_table_lookup (src, "sub"), *b = g_hash_table_lookup (src, "b.c");
  g_assert_cmpstr (a->hash, ==, "c3");
  g_assert_cmpstr (sub->hash, ==, "c3");
  g_assert_true (a == sub); /* one commit, two references */
  g_assert_cmpstr (b->author, ==, "Bob");
  g_assert_cmpint (b->date, ==, 200);
  g_assert_cmpstr (b->subject, ==, "Second");
  g_autoptr (GHashTable) root = gt_log_last_commits (data, sizeof data - 1, "");
  g_assert_cmpstr (((GtCommit *) g_hash_table_lookup (root, "src"))->hash, ==, "c3");
  g_assert_cmpstr (((GtCommit *) g_hash_table_lookup (root, "README.md"))->hash, ==, "c2");
  g_assert_cmpstr (((GtCommit *) g_hash_table_lookup (root, "top.txt"))->hash, ==, "c1");
  g_autoptr (GtCommit) first = gt_log_first (data, sizeof data - 1);
  g_assert_cmpstr (first->subject, ==, "Third");
  g_assert_null (gt_log_first ("garbage", 7));
  g_autoptr (GHashTable) none = gt_log_last_commits (NULL, 0, "x");
  g_assert_cmpuint (g_hash_table_size (none), ==, 0);
}

static void
test_errors_progress (void)
{
  g_assert_cmpint (gt_error_kind ("fatal: could not read Username for 'https://github.com': terminal prompts disabled"),
                   ==, GT_ERROR_NEEDS_CREDENTIALS);
  g_assert_cmpint (gt_error_kind ("remote: Invalid username or token.\nfatal: Authentication failed for 'https://x/'"),
                   ==, GT_ERROR_AUTH_FAILED);
  g_assert_cmpint (gt_error_kind ("git@github.com: Permission denied (publickey).\nfatal: Could not read from remote"),
                   ==, GT_ERROR_SSH);
  g_assert_cmpint (gt_error_kind ("error: pathspec 'x' did not match"), ==, GT_ERROR_OTHER);
  g_assert_cmpint (gt_error_kind (NULL), ==, GT_ERROR_OTHER);
  g_autofree char *phase = NULL;
  int pc = -1;
  g_assert_true (gt_progress_parse ("Receiving objects:  45% (450/1000), 1.2 MiB | 3 MiB/s", &phase, &pc));
  g_assert_cmpstr (phase, ==, "Receiving objects");
  g_assert_cmpint (pc, ==, 45);
  g_autofree char *phase2 = NULL;
  g_assert_true (gt_progress_parse ("remote: Counting objects: 100% (12/12), done.", &phase2, &pc));
  g_assert_cmpstr (phase2, ==, "Counting objects");
  g_assert_cmpint (pc, ==, 100);
  g_assert_false (gt_progress_parse ("fatal: something: else", NULL, NULL));
  g_assert_false (gt_progress_parse ("To github.com:a/b.git", NULL, NULL));
  g_assert_false (gt_progress_parse (": 5%", NULL, NULL));
}

static void
test_relative_date (void)
{
  const gint64 now = 1800000000;
  static const struct { gint64 ago; const char *text; } cases[] = {
    { 5, "just now" }, { 90, "1 minute ago" }, { 600, "10 minutes ago" }, { 3700, "1 hour ago" },
    { 5 * 3600, "5 hours ago" }, { 30 * 3600, "yesterday" }, { 12 * 86400, "12 days ago" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree char *t = gt_relative_date (now - cases[i].ago, now);
      g_assert_cmpstr (t, ==, cases[i].text);
    }
  g_autofree char *old = gt_relative_date (now - 400 * 86400, now);
  g_assert_nonnull (strstr (old, "20")); /* a full date with the year */
}

static void
test_urls (void)
{
  static const struct { const char *url; const char *name; } ok[] = {
    { "https://github.com/zyrotec/nautilus-utils.git", "nautilus-utils" },
    { "https://github.com/zyrotec/nautilus-utils", "nautilus-utils" },
    { "https://gitlab.com/group/sub/project.git/", "project" },
    { "  git@github.com:zyrotec/nautilus-utils.git  ", "nautilus-utils" },
    { "ssh://git@host:2222/srv/repo.git", "repo" },
    { "git://host/repo", "repo" },
    { "file:///srv/git/thing.git", "thing" },
    { "host:repo.git", "repo" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (ok); i++)
    {
      g_assert_true (gt_url_is_valid (ok[i].url));
      g_autofree char *name = gt_url_dir_name (ok[i].url);
      g_assert_cmpstr (name, ==, ok[i].name);
    }
  static const char *const bad[] = { "", "   ", "https://", "https://host", "file://relative", "nothing",
                                     "/local/path", "-uoops:x", "has space:x", ":path", "host:", NULL };
  for (int i = 0; bad[i] != NULL; i++)
    {
      g_assert_false (gt_url_is_valid (bad[i]));
      g_assert_null (gt_url_dir_name (bad[i]));
    }
  g_assert_false (gt_url_is_valid (NULL));
  g_assert_null (gt_url_dir_name ("https://host/.."));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/gt-parse/status", test_status);
  g_test_add_func ("/gt-parse/status-detached-initial", test_status_detached_initial);
  g_test_add_func ("/gt-parse/branches", test_branches);
  g_test_add_func ("/gt-parse/log", test_log);
  g_test_add_func ("/gt-parse/errors-progress", test_errors_progress);
  g_test_add_func ("/gt-parse/urls", test_urls);
  g_test_add_func ("/gt-parse/relative-date", test_relative_date);
  return g_test_run ();
}
