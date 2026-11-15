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

/* Finds a segment in @row. */
static gboolean
has_seg (GtGraphRow *row, int from, int to, gboolean top)
{
  for (guint i = 0; i < row->segs->len; i++)
    {
      GtGraphSeg *g = &g_array_index (row->segs, GtGraphSeg, i);
      if (g->from == from && g->to == to && g->top == top)
        return TRUE;
    }
  return FALSE;
}

static void
test_history_graph (void)
{
  /* M merges X into B; both come from A (the root). Topo order: M, B, X, A. */
  static const char data[] =
    "mmm\0bbb xxx\0Ann\0" "400\0HEAD -> refs/heads/main, refs/remotes/origin/main, refs/remotes/origin/HEAD, tag: refs/tags/v1\0Merge X\x1e\n"
    "bbb\0aaa\0Bob\0" "300\0refs/heads/old\0On main\x1e\n"
    "xxx\0aaa\0Cy\0" "200\0\0On a branch\x1e\n"
    "aaa\0\0Dee\0" "100\0\0Root\x1e\n";
  g_autoptr (GPtrArray) entries = gt_history_parse (data, sizeof data - 1);
  g_assert_cmpuint (entries->len, ==, 4);
  GtLogEntry *m = g_ptr_array_index (entries, 0), *a = g_ptr_array_index (entries, 3);
  g_assert_cmpstr (m->hash, ==, "mmm");
  g_assert_cmpuint (g_strv_length (m->parents), ==, 2);
  g_assert_cmpstr (m->parents[1], ==, "xxx");
  g_assert_cmpstr (m->author, ==, "Ann");
  g_assert_cmpint (m->date, ==, 400);
  g_assert_cmpstr (m->subject, ==, "Merge X");
  g_assert_cmpuint (m->refs->len, ==, 3); /* origin/HEAD left out */
  GtRef *r0 = g_ptr_array_index (m->refs, 0), *r1 = g_ptr_array_index (m->refs, 1), *r2 = g_ptr_array_index (m->refs, 2);
  g_assert_cmpstr (r0->name, ==, "main");
  g_assert_cmpint (r0->kind, ==, GT_REF_CURRENT);
  g_assert_cmpstr (r1->name, ==, "origin/main");
  g_assert_cmpint (r1->kind, ==, GT_REF_REMOTE);
  g_assert_cmpstr (r2->name, ==, "v1");
  g_assert_cmpint (r2->kind, ==, GT_REF_TAG);
  g_assert_cmpint (((GtRef *) g_ptr_array_index (((GtLogEntry *) g_ptr_array_index (entries, 1))->refs, 0))->kind, ==,
                   GT_REF_LOCAL);
  g_assert_cmpuint (g_strv_length (a->parents), ==, 0);

  g_autoptr (GPtrArray) rows = gt_graph_layout (entries);
  g_assert_cmpuint (rows->len, ==, 4);
  GtGraphRow *rm = g_ptr_array_index (rows, 0), *rb = g_ptr_array_index (rows, 1), *rx = g_ptr_array_index (rows, 2),
             *ra = g_ptr_array_index (rows, 3);
  g_assert_cmpint (rm->node, ==, 0);
  g_assert_true (rm->merge);
  g_assert_true (has_seg (rm, 0, 0, FALSE)); /* to B */
  g_assert_true (has_seg (rm, 0, 1, FALSE)); /* to X, in a new lane */
  g_assert_false (has_seg (rm, 1, 1, FALSE));
  g_assert_cmpint (rm->n_lanes, ==, 2);
  g_assert_cmpint (rb->node, ==, 0);
  g_assert_true (has_seg (rb, 0, 0, TRUE));
  g_assert_true (has_seg (rb, 1, 1, TRUE)); /* X's lane passes */
  g_assert_true (has_seg (rb, 1, 1, FALSE));
  g_assert_cmpint (rx->node, ==, 1);
  g_assert_true (has_seg (rx, 1, 1, TRUE));
  g_assert_true (has_seg (rx, 0, 0, TRUE));
  g_assert_cmpint (ra->node, ==, 0);
  g_assert_true (has_seg (ra, 0, 0, TRUE));
  g_assert_true (has_seg (ra, 1, 0, TRUE)); /* X's line bends into the root */
  g_assert_false (ra->merge);
  g_assert_cmpuint (ra->segs->len, ==, 2); /* nothing below the root */
  g_assert_cmpint (rm->node_color, !=, rx->node_color); /* the branch line has its own colour */

  /* A straight line; empty and garbage input. */
  static const char line[] = "c3\0c2\0A\0" "3\0\0three\x1e" "c2\0c1\0A\0" "2\0\0two\x1e" "c1\0\0A\0" "1\0\0one\x1e";
  g_autoptr (GPtrArray) le = gt_history_parse (line, sizeof line - 1);
  g_autoptr (GPtrArray) lr = gt_graph_layout (le);
  for (guint i = 0; i < lr->len; i++)
    {
      GtGraphRow *r = g_ptr_array_index (lr, i);
      g_assert_cmpint (r->node, ==, 0);
      g_assert_cmpint (r->n_lanes, ==, 1);
    }
  g_autoptr (GPtrArray) none = gt_history_parse (NULL, 0);
  g_assert_cmpuint (none->len, ==, 0);
  g_autoptr (GPtrArray) none_rows = gt_graph_layout (none);
  g_assert_cmpuint (none_rows->len, ==, 0);
  g_autoptr (GPtrArray) junk = gt_history_parse ("x\x1e\0\0\x1e", 6);
  g_assert_cmpuint (junk->len, ==, 0);
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
  g_test_add_func ("/gt-parse/history-graph", test_history_graph);
  return g_test_run ();
}
