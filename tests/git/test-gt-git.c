/* Git core against the real `git` (PLAN.md §14.4 Phase 1): repository detection (worktrees too), the runner
 * (status, log, branches, failures, timeout, cancel, progress) and credentials against a local smart-HTTP server
 * with basic auth. The tests' git ignores the user's configuration (meson env). Skipped without git. */
#include <glib/gstdio.h>
#include <string.h>

#include "gt_parse.h"
#include "gt_repo.h"
#include "gt_run.h"

static char *tmp; /* this run's folder */

/* Runs shell @cmd in @cwd; fails the test if it fails. */
static void
sh (const char *cwd, const char *cmd)
{
  const char *argv[] = { "sh", "-c", cmd, NULL };
  int status = 0;
  g_autofree char *err = NULL;
  g_autoptr (GError) error = NULL;
  gboolean ok = g_spawn_sync (cwd, (char **) argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL,
                              NULL, &err, &status, &error);
  if (!ok || !g_spawn_check_wait_status (status, NULL))
    g_error ("`%s` in %s failed: %s %s", cmd, cwd, error != NULL ? error->message : "", err != NULL ? err : "");
}

typedef struct
{
  GMainLoop   *loop;
  GtRunResult *result;
  GError      *error;
  guint        n_progress;
  int          last_percent;
  char        *phases;
} Wait;

static void
on_done (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  Wait *w = data;
  w->result = gt_run_finish (res, &w->error);
  g_main_loop_quit (w->loop);
}

static void
on_progress (const char *phase, int percent, const char *line, gpointer data)
{
  (void) line;
  Wait *w = data;
  w->n_progress++;
  w->last_percent = percent;
  if (w->phases == NULL || strstr (w->phases, phase) == NULL)
    {
      char *p = g_strconcat (w->phases != NULL ? w->phases : "", phase, ";", NULL);
      g_free (w->phases);
      w->phases = p;
    }
}

static GtRunResult *
run (const char *cwd, const char *const *args, GtRunFlags flags, const GtCredentials *cred, guint timeout_ms,
     GCancellable *cancel, Wait *w)
{
  w->loop = g_main_loop_new (NULL, FALSE);
  gt_run_async (cwd, args, flags, cred, timeout_ms, cancel, on_progress, w, on_done, w);
  g_main_loop_run (w->loop);
  g_main_loop_unref (w->loop);
  return w->result;
}

static void
wait_clear (Wait *w)
{
  g_clear_pointer (&w->result, gt_run_result_free);
  g_clear_error (&w->error);
  g_clear_pointer (&w->phases, g_free);
}

/* A repository with every kind of change; returns its path. */
static char *
make_repo (const char *name)
{
  char *repo = g_build_filename (tmp, name, NULL);
  g_mkdir_with_parents (repo, 0755);
  sh (repo, "git init -q && mkdir -p src/sub && printf 'node_modules/\\n*.log\\n' > .gitignore && echo 1 > README.md"
            " && echo a > src/a.c && echo b > src/b.c && echo x > src/sub/x.c && echo k > kept.txt"
            " && git add -A && git commit -qm 'Initial commit'"
            " && echo 2 >> README.md && git commit -qam 'Update the readme'"
            " && echo changed >> kept.txt && echo new > added.txt && git add added.txt && echo u > untracked.txt"
            " && echo l > debug.log && mkdir -p node_modules/m && echo m > node_modules/m/i.js && echo c >> src/a.c");
  return repo;
}

static void
test_find_root (void)
{
  g_autofree char *repo = make_repo ("find");
  g_autofree char *deep = g_build_filename (repo, "src", "sub", NULL);
  g_autofree char *gd = NULL;
  g_autofree char *root = gt_repo_find_root (deep, &gd);
  g_assert_cmpstr (root, ==, repo);
  g_autofree char *expect_gd = g_build_filename (repo, ".git", NULL);
  g_assert_cmpstr (gd, ==, expect_gd);
  g_autofree char *rel = gt_repo_relative (root, deep);
  g_assert_cmpstr (rel, ==, "src/sub");
  g_autofree char *rel_root = gt_repo_relative (root, root);
  g_assert_cmpstr (rel_root, ==, "");
  g_assert_null (gt_repo_relative (root, "/elsewhere"));
  g_autofree char *sibling = g_strconcat (root, "x", NULL);
  g_assert_null (gt_repo_relative (root, sibling));
  /* Not a repository; not absolute; missing. */
  g_autofree char *plain = g_build_filename (tmp, "plain", NULL);
  g_mkdir_with_parents (plain, 0755);
  g_assert_null (gt_repo_find_root (plain, NULL));
  g_assert_null (gt_repo_find_root ("relative/path", NULL));
  g_autofree char *missing = g_build_filename (tmp, "missing", NULL);
  g_assert_null (gt_repo_find_root (missing, NULL));
  /* A linked worktree: ".git" is a file pointing into the main repository. */
  sh (repo, "git worktree add -q ../find-wt");
  g_autofree char *wt = g_build_filename (tmp, "find-wt", NULL);
  g_autofree char *wt_src = g_build_filename (wt, "src", NULL);
  g_autofree char *wt_gd = NULL;
  g_autofree char *wt_root = gt_repo_find_root (wt_src, &wt_gd);
  g_assert_cmpstr (wt_root, ==, wt);
  g_autofree char *expect_wt_gd = g_build_filename (repo, ".git", "worktrees", "find-wt", NULL);
  g_assert_cmpstr (wt_gd, ==, expect_wt_gd);
  /* A ".git" folder that isn't a repository (no HEAD) doesn't count. */
  g_autofree char *fake = g_build_filename (tmp, "fake", ".git", NULL);
  g_mkdir_with_parents (fake, 0755);
  g_autofree char *fake_parent = g_path_get_dirname (fake);
  g_assert_null (gt_repo_find_root (fake_parent, NULL));
}

static void
test_status_log_branches (void)
{
  g_autofree char *repo = make_repo ("status");
  Wait w = { 0 };
  const char *status_args[] = { "status", "--porcelain=v2", "-z", "--branch", "--untracked-files=normal",
                                "--ignored=matching", NULL };
  GtRunResult *r = run (repo, status_args, GT_RUN_READ, NULL, 10000, NULL, &w);
  g_assert_no_error (w.error);
  g_assert_cmpint (r->exit_status, ==, 0);
  gsize len = 0;
  const char *d = g_bytes_get_data (r->out, &len);
  g_autoptr (GtStatus) st = gt_status_parse (d, len);
  g_assert_cmpstr (st->head, ==, "main");
  g_assert_nonnull (st->oid);
  g_assert_cmpint (gt_status_state (st, "kept.txt", FALSE), ==, GT_STATE_MODIFIED);
  g_assert_cmpint (gt_status_state (st, "added.txt", FALSE), ==, GT_STATE_ADDED);
  g_assert_cmpint (gt_status_state (st, "untracked.txt", FALSE), ==, GT_STATE_UNTRACKED);
  g_assert_cmpint (gt_status_state (st, "debug.log", FALSE), ==, GT_STATE_IGNORED);
  g_assert_cmpint (gt_status_state (st, "node_modules", TRUE), ==, GT_STATE_IGNORED);
  g_assert_cmpint (gt_status_state (st, "src", TRUE), ==, GT_STATE_CONTAINS);
  g_assert_cmpint (gt_status_state (st, "src/a.c", FALSE), ==, GT_STATE_MODIFIED);
  g_assert_cmpint (gt_status_state (st, "src/b.c", FALSE), ==, GT_STATE_NONE);
  g_assert_cmpint (gt_status_state (st, "README.md", FALSE), ==, GT_STATE_NONE);
  /* A read takes no optional locks. */
  g_autofree char *lock = g_build_filename (repo, ".git", "index.lock", NULL);
  g_assert_false (g_file_test (lock, G_FILE_TEST_EXISTS));
  wait_clear (&w);

  const char *log_args[] = { "log", "--format=" GT_LOG_FORMAT, "--name-only", "-z", "-n", "2000", "--", ".", NULL };
  r = run (repo, log_args, GT_RUN_READ, NULL, 10000, NULL, &w);
  g_assert_no_error (w.error);
  d = g_bytes_get_data (r->out, &len);
  g_autoptr (GHashTable) root = gt_log_last_commits (d, len, "");
  GtCommit *readme = g_hash_table_lookup (root, "README.md"), *src = g_hash_table_lookup (root, "src");
  g_assert_nonnull (readme);
  g_assert_nonnull (src);
  g_assert_cmpstr (readme->subject, ==, "Update the readme");
  g_assert_cmpstr (readme->author, ==, "Test Author");
  g_assert_cmpstr (src->subject, ==, "Initial commit");
  g_assert_null (g_hash_table_lookup (root, "untracked.txt"));
  g_autoptr (GHashTable) in_src = gt_log_last_commits (d, len, "src");
  g_assert_nonnull (g_hash_table_lookup (in_src, "sub"));
  g_assert_nonnull (g_hash_table_lookup (in_src, "a.c"));
  wait_clear (&w);

  sh (repo, "git branch feature && git branch zz-old");
  const char *ref_args[] = { "for-each-ref", "--format=" GT_BRANCH_FORMAT, "refs/heads", "refs/remotes", NULL };
  r = run (repo, ref_args, GT_RUN_READ, NULL, 10000, NULL, &w);
  d = g_bytes_get_data (r->out, &len);
  g_autoptr (GPtrArray) branches = gt_branches_parse (d, len);
  g_assert_cmpuint (branches->len, ==, 3);
  g_assert_cmpstr (((GtBranch *) g_ptr_array_index (branches, 0))->name, ==, "main");
  g_assert_true (((GtBranch *) g_ptr_array_index (branches, 0))->current);
  g_assert_cmpstr (((GtBranch *) g_ptr_array_index (branches, 1))->name, ==, "feature");
  g_assert_cmpstr (((GtBranch *) g_ptr_array_index (branches, 2))->subject, ==, "Update the readme");
  wait_clear (&w);

  /* A failing command: the result with git's message, no GError. */
  const char *bad[] = { "checkout", "no-such-branch", NULL };
  r = run (repo, bad, 0, NULL, 10000, NULL, &w);
  g_assert_no_error (w.error);
  g_assert_cmpint (r->exit_status, !=, 0);
  g_assert_nonnull (strstr (r->error_text, "no-such-branch"));
  g_assert_cmpint (r->kind, ==, GT_ERROR_OTHER);
  wait_clear (&w);
}

static void
test_timeout_cancel (void)
{
  g_autofree char *repo = make_repo ("slow");
  const char *slow[] = { "-c", "alias.slow=!sleep 20", "slow", NULL };
  Wait w = { 0 };
  gint64 t0 = g_get_monotonic_time ();
  run (repo, slow, 0, NULL, 300, NULL, &w);
  g_assert_error (w.error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  g_assert_null (w.result);
  g_assert_cmpint (g_get_monotonic_time () - t0, <, 10 * G_USEC_PER_SEC);
  wait_clear (&w);

  g_autoptr (GCancellable) cancel = g_cancellable_new ();
  guint id = g_timeout_add_once (300, (GSourceOnceFunc) g_cancellable_cancel, cancel);
  t0 = g_get_monotonic_time ();
  run (repo, slow, 0, NULL, 0, cancel, &w);
  g_assert_error (w.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpint (g_get_monotonic_time () - t0, <, 10 * G_USEC_PER_SEC);
  wait_clear (&w);
  (void) id;

  /* Cancelled before it starts. */
  g_autoptr (GCancellable) early = g_cancellable_new ();
  g_cancellable_cancel (early);
  run (repo, slow, 0, NULL, 0, early, &w);
  g_assert_error (w.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  wait_clear (&w);
}

static void
test_progress (void)
{
  /* A bare remote with enough objects for git to report progress over file://. */
  g_autofree char *src = g_build_filename (tmp, "progress-src", NULL);
  g_mkdir_with_parents (src, 0755);
  sh (src, "git init -q && for i in $(seq 1 300); do echo $i > f$i; done && git add -A && git commit -qm many"
           " && git clone -q --bare . ../progress.git");
  g_autofree char *url = g_strconcat ("file://", tmp, "/progress.git", NULL);
  const char *clone[] = { "clone", "--progress", url, "progress-clone", NULL };
  Wait w = { 0 };
  GtRunResult *r = run (tmp, clone, GT_RUN_NETWORK, NULL, 60000, NULL, &w);
  g_assert_no_error (w.error);
  g_assert_cmpint (r->exit_status, ==, 0);
  g_assert_cmpuint (w.n_progress, >, 0);
  g_assert_nonnull (strstr (w.phases, "Receiving objects"));
  g_assert_cmpint (w.last_percent, ==, 100);
  /* Progress lines aren't part of the error text. */
  g_assert_null (strstr (r->error_text, "Receiving objects"));
  g_autofree char *cloned = g_build_filename (tmp, "progress-clone", "f300", NULL);
  g_assert_true (g_file_test (cloned, G_FILE_TEST_IS_REGULAR));
  wait_clear (&w);
}

static void
test_credentials (void)
{
  g_autofree char *python = g_find_program_in_path ("python3");
  if (python == NULL)
    {
      g_test_skip ("python3 is needed for the test HTTP server");
      return;
    }
  g_autofree char *srv = g_build_filename (tmp, "srv", NULL);
  g_mkdir_with_parents (srv, 0755);
  sh (srv, "git init -q --bare repo.git && git -C repo.git config http.receivepack true");
  g_autofree char *script = g_test_build_filename (G_TEST_DIST, "data", "authserver.py", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GSubprocess) server = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error, python, "-I", script, srv,
                                                     NULL);
  g_assert_no_error (error);
  g_autoptr (GDataInputStream) in = g_data_input_stream_new (g_subprocess_get_stdout_pipe (server));
  g_autofree char *port = g_data_input_stream_read_line (in, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (port);
  g_autofree char *url = g_strdup_printf ("http://127.0.0.1:%s/repo.git", port);

  Wait w = { 0 };
  const char *clone_a[] = { "clone", url, "cred-a", NULL };
  GtRunResult *r = run (tmp, clone_a, GT_RUN_NETWORK, NULL, 30000, NULL, &w);
  g_assert_no_error (w.error);
  g_assert_cmpint (r->exit_status, !=, 0);
  g_assert_cmpint (r->kind, ==, GT_ERROR_NEEDS_CREDENTIALS); /* fails at once instead of prompting */
  wait_clear (&w);

  GtCredentials wrong = { "u", "bad", NULL };
  const char *clone_b[] = { "clone", url, "cred-b", NULL };
  r = run (tmp, clone_b, GT_RUN_NETWORK, &wrong, 30000, NULL, &w);
  g_assert_cmpint (r->exit_status, !=, 0);
  g_assert_cmpint (r->kind, ==, GT_ERROR_AUTH_FAILED);
  wait_clear (&w);

  GtCredentials right = { "u", "tok", NULL };
  const char *clone_c[] = { "clone", url, "cred-c", NULL };
  r = run (tmp, clone_c, GT_RUN_NETWORK, &right, 30000, NULL, &w);
  g_assert_cmpint (r->exit_status, ==, 0);
  wait_clear (&w);

  /* Push with credentials; then without: nothing is kept between runs (no helper configured in tests). */
  g_autofree char *clone_dir = g_build_filename (tmp, "cred-c", NULL);
  sh (clone_dir, "echo hi > f && git add f && git commit -qm one");
  const char *push[] = { "push", "origin", "HEAD:main", NULL };
  r = run (clone_dir, push, GT_RUN_NETWORK, &right, 30000, NULL, &w);
  g_assert_cmpint (r->exit_status, ==, 0);
  wait_clear (&w);
  const char *fetch[] = { "fetch", NULL };
  r = run (clone_dir, fetch, GT_RUN_NETWORK, NULL, 30000, NULL, &w);
  g_assert_cmpint (r->kind, ==, GT_ERROR_NEEDS_CREDENTIALS);
  wait_clear (&w);
  g_subprocess_force_exit (server);
  g_subprocess_wait (server, NULL, NULL);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  if (!gt_run_have_git ())
    {
      g_test_message ("git not found: skipping");
      return 77;
    }
  g_autoptr (GError) error = NULL;
  tmp = g_dir_make_tmp ("gt-test-XXXXXX", &error);
  g_assert_no_error (error);
  g_test_add_func ("/gt-git/find-root", test_find_root);
  g_test_add_func ("/gt-git/status-log-branches", test_status_log_branches);
  g_test_add_func ("/gt-git/timeout-cancel", test_timeout_cancel);
  g_test_add_func ("/gt-git/progress", test_progress);
  g_test_add_func ("/gt-git/credentials", test_credentials);
  int rc = g_test_run ();
  g_autofree char *rm = g_strdup_printf ("rm -rf '%s'", tmp);
  int ignored = system (rm);
  (void) ignored;
  g_free (tmp);
  return rc;
}
