/* Git runner. See gt_run.h. */
#define _GNU_SOURCE 1 /* setsid */
#include "gt_run.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ERROR_MAX (64 * 1024)
#define READ_CHUNK 4096

/* The one-shot credential helper (PLAN.md §14.5 item 5): answers "get" from the child's environment. */
#define CRED_HELPER "credential.helper=!f() { test \"$1\" = get && printf 'username=%s\\npassword=%s\\n' " \
                    "\"$NU_GIT_USER\" \"$NU_GIT_TOKEN\"; }; f"

typedef struct
{
  GSubprocess    *proc;
  GOutputStream  *out;          /* GMemoryOutputStream */
  GInputStream   *err;          /* borrowed from @proc */
  GString        *line;         /* stderr line being read */
  GString        *error_text;
  GtProgressFunc  progress;
  gpointer        progress_data;
  GCancellable   *cancellable;  /* caller's, or NULL */
  gulong          cancel_id;
  guint           timeout_id;
  gboolean        timed_out;
  gboolean        reaped;       /* git has exited and been waited for */
  long            pid;          /* git's pid = its process group (child_setup) */
  guint           pending;      /* stdout splice, stderr reader, wait */
} Job;

static void
job_free (gpointer data)
{
  Job *job = data;
  g_clear_handle_id (&job->timeout_id, g_source_remove);
  if (job->cancel_id != 0)
    g_cancellable_disconnect (job->cancellable, job->cancel_id);
  g_clear_object (&job->cancellable);
  g_clear_object (&job->out);
  g_clear_object (&job->proc);
  if (job->line != NULL)
    g_string_free (job->line, TRUE);
  if (job->error_text != NULL)
    g_string_free (job->error_text, TRUE);
  g_free (job);
}

void
gt_run_result_free (GtRunResult *result)
{
  if (result == NULL)
    return;
  g_clear_pointer (&result->out, g_bytes_unref);
  g_free (result->error_text);
  g_free (result);
}

gboolean
gt_run_have_git (void)
{
  /* Asked for every folder's background menu: looked up once. */
  static int have = -1;
  if (have < 0)
    {
      g_autofree char *path = g_find_program_in_path ("git");
      have = path != NULL;
    }
  return have;
}

static void
child_setup (gpointer data)
{
  (void) data;
  setsid (); /* no controlling terminal: ssh and git can't prompt on one */
}

static void
finish_if_done (GTask *task)
{
  Job *job = g_task_get_task_data (task);
  if (--job->pending > 0)
    return;
  g_clear_handle_id (&job->timeout_id, g_source_remove);
  if (job->cancellable != NULL && g_cancellable_is_cancelled (job->cancellable))
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "git was cancelled");
      return;
    }
  if (job->timed_out)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "git took too long and was stopped");
      return;
    }
  GtRunResult *r = g_new0 (GtRunResult, 1);
  r->exit_status = g_subprocess_get_if_exited (job->proc) ? g_subprocess_get_exit_status (job->proc) : -1;
  r->out = g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (job->out));
  r->error_text = g_strstrip (g_strdup (job->error_text->str));
  r->kind = r->exit_status != 0 ? gt_error_kind (r->error_text) : GT_ERROR_OTHER;
  g_task_return_pointer (task, r, (GDestroyNotify) gt_run_result_free);
}

/* One complete stderr line: a progress line goes to the callback, anything else to the error text. */
static void
take_line (Job *job, const char *line)
{
  if (*line == '\0')
    return;
  g_autofree char *phase = NULL;
  int percent = -1;
  if (gt_progress_parse (line, &phase, &percent))
    {
      if (job->progress != NULL)
        job->progress (phase, percent, line, job->progress_data);
      return;
    }
  if (job->error_text->len + strlen (line) + 1 <= ERROR_MAX)
    {
      g_string_append (job->error_text, line);
      g_string_append_c (job->error_text, '\n');
    }
}

static void
on_err_read (GObject *src, GAsyncResult *res, gpointer data)
{
  GTask *task = data;
  Job *job = g_task_get_task_data (task);
  g_autoptr (GBytes) bytes = g_input_stream_read_bytes_finish (G_INPUT_STREAM (src), res, NULL);
  gsize n = 0;
  const char *p = bytes != NULL ? g_bytes_get_data (bytes, &n) : NULL;
  if (p == NULL || n == 0)
    {
      take_line (job, job->line->str);
      g_string_truncate (job->line, 0);
      finish_if_done (task);
      g_object_unref (task);
      return;
    }
  for (gsize i = 0; i < n; i++)
    {
      /* git rewrites progress lines with '\r'. */
      if (p[i] == '\n' || p[i] == '\r')
        {
          take_line (job, job->line->str);
          g_string_truncate (job->line, 0);
        }
      else if (job->line->len < 4096)
        g_string_append_c (job->line, p[i]);
    }
  g_input_stream_read_bytes_async (job->err, READ_CHUNK, G_PRIORITY_DEFAULT, NULL, on_err_read, task);
}

static void
on_spliced (GObject *src, GAsyncResult *res, gpointer data)
{
  GTask *task = data;
  g_output_stream_splice_finish (G_OUTPUT_STREAM (src), res, NULL);
  finish_if_done (task);
  g_object_unref (task);
}

static void
on_waited (GObject *src, GAsyncResult *res, gpointer data)
{
  GTask *task = data;
  g_subprocess_wait_finish (G_SUBPROCESS (src), res, NULL);
  ((Job *) g_task_get_task_data (task))->reaped = TRUE;
  finish_if_done (task);
  g_object_unref (task);
}

/* Kills git and everything it started (ssh, remote helpers, hooks…): it leads its own session and process group
 * (child_setup), and a child left running would keep the pipes open, so the run would never end. */
static void
kill_group (Job *job)
{
  /* Its group id stays reserved while git runs or a member holds our pipes (pending > 0), so this never hits an
   * unrelated process; once everything has ended there is nothing to kill. */
  if (job->pending == 0)
    return;
  const char *id = g_subprocess_get_identifier (job->proc); /* NULL once reaped */
  long pid = id != NULL ? strtol (id, NULL, 10) : job->pid;
  if (pid > 1)
    kill ((pid_t) -pid, SIGKILL);
  if (!job->reaped)
    g_subprocess_force_exit (job->proc);
}

static gboolean
on_timeout (gpointer data)
{
  Job *job = data;
  job->timeout_id = 0;
  job->timed_out = TRUE;
  kill_group (job);
  return G_SOURCE_REMOVE;
}

static void
on_cancelled (GCancellable *cancellable, gpointer data)
{
  (void) cancellable;
  kill_group (data); /* the reads then end and the task finishes as cancelled */
}

void
gt_run_async (const char          *cwd,
              const char *const   *args,
              GtRunFlags           flags,
              const GtCredentials *credentials,
              guint                timeout_ms,
              GCancellable        *cancellable,
              GtProgressFunc       progress,
              gpointer             progress_data,
              GAsyncReadyCallback  callback,
              gpointer             user_data)
{
  g_return_if_fail (cwd != NULL && args != NULL && args[0] != NULL);
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, gt_run_async);

  g_autoptr (GStrvBuilder) argv = g_strv_builder_new ();
  g_strv_builder_add_many (argv, "git", "-c", "core.askPass=", NULL);
  if (flags & GT_RUN_READ)
    g_strv_builder_add (argv, "--no-optional-locks");
  gboolean creds = credentials != NULL && credentials->username != NULL && credentials->token != NULL;
  g_autofree char *store = NULL;
  if (creds)
    {
      g_strv_builder_add_many (argv, "-c", CRED_HELPER, NULL);
      /* After ours: asked for nothing ours answers, but told to store them when the run succeeds. */
      if (credentials->store_helper != NULL && *credentials->store_helper != '\0')
        {
          store = g_strdup_printf ("credential.helper=%s", credentials->store_helper);
          g_strv_builder_add_many (argv, "-c", store, NULL);
        }
    }
  for (int i = 0; args[i] != NULL; i++)
    g_strv_builder_add (argv, args[i]);
  g_auto (GStrv) av = g_strv_builder_end (argv);

  /* stdin: /dev/null (the default). */
  g_autoptr (GSubprocessLauncher) l = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                                 G_SUBPROCESS_FLAGS_STDERR_PIPE);
  g_subprocess_launcher_set_cwd (l, cwd);
  g_subprocess_launcher_setenv (l, "LC_ALL", "C", TRUE);
  g_subprocess_launcher_unsetenv (l, "LANGUAGE");
  g_subprocess_launcher_setenv (l, "GIT_TERMINAL_PROMPT", "0", TRUE);
  g_subprocess_launcher_unsetenv (l, "GIT_ASKPASS");
  g_subprocess_launcher_unsetenv (l, "SSH_ASKPASS");
  g_subprocess_launcher_setenv (l, "SSH_ASKPASS_REQUIRE", "never", TRUE);
  g_subprocess_launcher_unsetenv (l, "NU_GIT_USER");
  g_subprocess_launcher_unsetenv (l, "NU_GIT_TOKEN");
  if (creds)
    {
      g_subprocess_launcher_setenv (l, "NU_GIT_USER", credentials->username, TRUE);
      g_subprocess_launcher_setenv (l, "NU_GIT_TOKEN", credentials->token, TRUE);
    }
  g_subprocess_launcher_set_child_setup (l, child_setup, NULL, NULL);
  GError *error = NULL;
  GSubprocess *proc = g_subprocess_launcher_spawnv (l, (const char *const *) av, &error);
  if (proc == NULL)
    {
      g_task_return_error (task, error);
      return;
    }
  Job *job = g_new0 (Job, 1);
  job->proc = proc;
  job->out = g_memory_output_stream_new_resizable ();
  job->err = g_subprocess_get_stderr_pipe (proc);
  job->line = g_string_new (NULL);
  job->error_text = g_string_new (NULL);
  job->progress = progress;
  job->progress_data = progress_data;
  job->pending = 3;
  const char *ident = g_subprocess_get_identifier (proc);
  job->pid = ident != NULL ? strtol (ident, NULL, 10) : 0;
  g_task_set_task_data (task, job, job_free);
  if (cancellable != NULL)
    {
      job->cancellable = g_object_ref (cancellable);
      job->cancel_id = g_cancellable_connect (cancellable, G_CALLBACK (on_cancelled), job, NULL);
    }
  if (timeout_ms > 0)
    job->timeout_id = g_timeout_add (timeout_ms, on_timeout, job);
  /* No cancellable on the reads: a cancelled run is killed, which ends them. */
  g_output_stream_splice_async (job->out, g_subprocess_get_stdout_pipe (proc), G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                G_PRIORITY_DEFAULT, NULL, on_spliced, g_object_ref (task));
  g_input_stream_read_bytes_async (job->err, READ_CHUNK, G_PRIORITY_DEFAULT, NULL, on_err_read, g_object_ref (task));
  g_subprocess_wait_async (proc, NULL, on_waited, g_object_ref (task));
}

GtRunResult *
gt_run_finish (GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
  return g_task_propagate_pointer (G_TASK (result), error);
}
