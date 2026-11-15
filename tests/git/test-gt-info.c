/* Git repository info and operations (PLAN.md §14.7): branch and counts, a change made outside (git folder
 * monitor), operations (fetch, pull, a failing one) and their effect on the counts. Skipped without git. */
#include <glib/gstdio.h>
#include <string.h>

#include "gt_info.h"
#include "gt_ops.h"
#include "gt_repo.h"

static char *tmp;

static void
sh (const char *cwd, const char *cmd)
{
  const char *argv[] = { "sh", "-c", cmd, NULL };
  int status = 0;
  g_autofree char *err = NULL;
  if (!g_spawn_sync (cwd, (char **) argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, NULL,
                     &err, &status, NULL) ||
      !g_spawn_check_wait_status (status, NULL))
    g_error ("`%s` failed: %s", cmd, err != NULL ? err : "");
}

/* Runs the main loop until @cond is true or 15 s pass. */
static gboolean
wait_for (gboolean (*cond) (gpointer), gpointer data)
{
  gint64 end = g_get_monotonic_time () + 15 * G_USEC_PER_SEC;
  while (!cond (data) && g_get_monotonic_time () < end)
    g_main_context_iteration (NULL, TRUE);
  return cond (data);
}

typedef struct
{
  GtInfo *info;
  guint   reads;
} Seen;

static gboolean
read_after (gpointer data)
{
  Seen *s = data;
  return gt_info_get_n_reads (s->info) > s->reads;
}

static gboolean
op_finished (gpointer data)
{
  return gt_op_get_state (data) != GT_OP_RUNNING;
}

static gboolean
detached (gpointer data)
{
  return gt_info_get_loaded (data) && gt_info_get_branch (data) == NULL;
}

/* Waits for the next read of @info to complete. */
static void
next_read (GtInfo *info)
{
  Seen s = { info, gt_info_get_n_reads (info) };
  g_assert_true (wait_for (read_after, &s));
}

typedef struct
{
  GtInfo *info;
  int     ahead, behind;
} Want;

static gboolean
counts_are (gpointer data)
{
  Want *w = data;
  int a = -1, b = -1;
  return gt_info_get_counts (w->info, &a, &b) && a == w->ahead && b == w->behind;
}

/* Waits until @info shows these counts: a read that started before a change may complete first (and under
 * valgrind's slowdown it often does), so this waits for the state, not for "the next read". */
static void
counts (GtInfo *info, int ahead, int behind)
{
  Want w = { info, ahead, behind };
  if (!wait_for (counts_are, &w))
    {
      int a = -1, b = -1;
      gt_info_get_counts (info, &a, &b);
      g_error ("expected ↑%d ↓%d, have ↑%d ↓%d", ahead, behind, a, b);
    }
}

static void
test_info_ops (void)
{
  sh (tmp, "git init -q --bare -b main remote.git && git clone -q remote.git seed 2>/dev/null && cd seed"
           " && git checkout -q -b main && echo 1 > a && git add a && git commit -qm first && git push -q origin main"
           " && cd .. && git clone -q -b main remote.git repo && git clone -q -b main remote.git other"
           " && cd other && echo 2 >> a && git commit -qam second && git push -q origin main");
  g_autofree char *repo = g_build_filename (tmp, "repo", NULL);
  g_autofree char *git_dir = NULL;
  g_autofree char *root = gt_repo_find_root (repo, &git_dir);
  g_assert_cmpstr (root, ==, repo);

  g_autoptr (GtInfo) info = gt_info_get (root, git_dir);
  g_autoptr (GtInfo) same = gt_info_get (root, git_dir);
  g_assert_true (info == same); /* shared */
  if (!gt_info_get_loaded (info))
    next_read (info);
  g_assert_null (gt_info_get_error (info));
  g_assert_cmpstr (gt_info_get_branch (info), ==, "main");
  g_assert_cmpstr (gt_info_get_upstream (info), ==, "origin/main");
  g_assert_cmpstr (gt_info_get_name (info), ==, "repo");
  g_assert_cmpstr (gt_info_get_last (info)->subject, ==, "first");
  counts (info, 0, 0);

  /* A commit made outside: seen through the git folder monitor. */
  sh (repo, "echo x > b && git add b && git commit -qm local");
  counts (info, 1, 0);
  g_assert_cmpstr (gt_info_get_last (info)->subject, ==, "local");

  /* Sync (fetch): behind by one. */
  const char *fetch[] = { "fetch", "--progress", NULL };
  GtOp *op = gt_ops_start (root, "Sync", fetch, GT_RUN_NETWORK, "Up to date", NULL, NULL);
  g_assert_cmpuint (gt_ops_count_running (root), ==, 1);
  g_assert_true (wait_for (op_finished, op));
  g_assert_cmpint (gt_op_get_state (op), ==, GT_OP_DONE);
  g_assert_cmpstr (gt_op_get_message (op), ==, "Up to date");
  counts (info, 1, 1);

  /* Pull --ff-only can't (diverged): failed with git's message; nothing changed. */
  const char *pull[] = { "pull", "--ff-only", NULL };
  op = gt_ops_start (root, "Pull", pull, GT_RUN_NETWORK, "Pulled", NULL, NULL);
  g_assert_true (wait_for (op_finished, op));
  g_assert_cmpint (gt_op_get_state (op), ==, GT_OP_FAILED);
  g_assert_nonnull (gt_op_get_message (op));
  g_assert_cmpuint (strlen (gt_op_get_message (op)), >, 0);
  counts (info, 1, 1);

  /* Rebase onto it, push: in sync. */
  sh (repo, "git rebase -q origin/main");
  const char *push[] = { "push", "--progress", NULL };
  op = gt_ops_start (root, "Push", push, GT_RUN_NETWORK, "Pushed", NULL, NULL);
  g_assert_true (wait_for (op_finished, op));
  g_assert_cmpint (gt_op_get_state (op), ==, GT_OP_DONE);
  counts (info, 0, 0);

  /* The list: newest first, three operations. */
  GListModel *ops = gt_ops_get_model ();
  g_assert_cmpuint (g_list_model_get_n_items (ops), ==, 3);
  g_autoptr (GtOp) first = g_list_model_get_item (ops, 0);
  g_assert_cmpstr (gt_op_get_title (first), ==, "Push");
  g_assert_cmpuint (gt_ops_count_running (NULL), ==, 0);

  /* Detached HEAD; then the info is released and forgotten. */
  sh (repo, "git checkout -q --detach HEAD~1");
  g_assert_true (wait_for (detached, info));
  g_assert_null (gt_info_get_branch (info));
  g_assert_nonnull (gt_info_get_oid (info));
  g_clear_object (&same);
  GtInfo *weak = info;
  g_object_add_weak_pointer (G_OBJECT (info), (gpointer *) &weak);
  g_clear_object (&info);
  /* A read may still hold it for a moment. */
  gint64 end = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;
  while (weak != NULL && g_get_monotonic_time () < end)
    g_main_context_iteration (NULL, TRUE);
  g_assert_null (weak);
  g_assert_null (gt_info_lookup (root));
  gt_ops_shutdown ();
}

static void
test_not_a_repo (void)
{
  /* An info for a folder that isn't (or no longer is) a repository: an error, no crash. */
  g_autofree char *dir = g_build_filename (tmp, "gone", NULL);
  g_mkdir_with_parents (dir, 0755);
  g_autofree char *fake_git = g_build_filename (dir, ".git", NULL);
  g_autoptr (GtInfo) info = gt_info_get (dir, fake_git);
  next_read (info);
  g_assert_nonnull (gt_info_get_error (info));
  g_assert_null (gt_info_get_branch (info));
  g_assert_false (gt_info_get_counts (info, NULL, NULL));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  if (!gt_run_have_git ())
    return 77;
  g_autoptr (GError) error = NULL;
  tmp = g_dir_make_tmp ("gt-info-XXXXXX", &error);
  g_assert_no_error (error);
  g_test_add_func ("/gt-info/info-ops", test_info_ops);
  g_test_add_func ("/gt-info/not-a-repo", test_not_a_repo);
  int rc = g_test_run ();
  g_autofree char *rm = g_strdup_printf ("rm -rf '%s'", tmp);
  int ignored = system (rm);
  (void) ignored;
  g_free (tmp);
  return rc;
}
