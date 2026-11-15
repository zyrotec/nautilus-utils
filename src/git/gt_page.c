/* Git view of the details pane. See gt_page.h. */
#include "gt_page.h"

#include <string.h>

#include "dp_pane.h"
#include "gt_info.h"
#include "gt_repo.h"
#include "gt_run.h"

#define READ_TIMEOUT_MS 20000
#define MAX_PATHS       200 /* a status read for at most this many selected items */

/* Kept as the page's data ("gt-page"); freed with the page. */
typedef struct
{
  GtkWidget    *page;      /* borrowed (we are its data) */
  GtInfo       *info;      /* ref */
  gulong        info_id;
  char         *root;
  GPtrArray    *rels;      /* root-relative paths of what is shown (a folder with nothing selected: itself) */
  gboolean      is_dir;    /* one item that is a folder */
  gboolean      own_folder; /* nothing selected: the folder itself */
  GtkWidget    *repo_w;    /* the "Repository" group (replaced on changes) */
  GtkWidget    *item_w;    /* the item group, or NULL */
  GCancellable *cancel;
  GtStatus     *status;    /* the selection's status, NULL until read */
  GtCommit     *last;      /* one item's last commit */
  gboolean      item_read;
} Page;

static void
page_free (gpointer data)
{
  Page *p = data;
  if (p->cancel != NULL)
    g_cancellable_cancel (p->cancel);
  g_clear_object (&p->cancel);
  if (p->info != NULL)
    g_clear_signal_handler (&p->info_id, p->info);
  g_clear_object (&p->info);
  g_clear_pointer (&p->status, gt_status_free);
  g_clear_pointer (&p->last, gt_commit_unref);
  g_ptr_array_unref (p->rels);
  g_free (p->root);
  g_free (p);
}

static char *
when_text (gint64 date)
{
  g_autofree char *rel = gt_relative_date (date, g_get_real_time () / G_USEC_PER_SEC);
  g_autoptr (GDateTime) dt = g_date_time_new_from_unix_local (date);
  g_autofree char *full = dt != NULL ? g_date_time_format (dt, "%-d %B %Y, %H:%M") : NULL;
  return full != NULL ? g_strdup_printf ("%s (%s)", rel, full) : g_strdup (rel);
}

/* Replaces group @old (may be NULL) in the page with @fresh, keeping its place. */
static GtkWidget *
replace (Page *p, GtkWidget *old, GtkWidget *fresh, GtkWidget *after)
{
  if (old != NULL)
    {
      gtk_box_insert_child_after (GTK_BOX (p->page), fresh, old);
      gtk_box_remove (GTK_BOX (p->page), old);
    }
  else
    gtk_box_insert_child_after (GTK_BOX (p->page), fresh, after);
  return fresh;
}

static void
render_repo (Page *p)
{
  GtInfo *info = p->info;
  const char *labels[8], *values[8];
  guint n = 0;
  g_autofree char *branch = NULL, *upstream = NULL, *commits = NULL, *hash = NULL, *when = NULL;
  labels[n] = "Repository", values[n++] = gt_info_get_name (info);
  labels[n] = "Location", values[n++] = gt_info_get_root (info);
  if (gt_info_get_error (info) != NULL)
    labels[n] = "Git", values[n++] = gt_info_get_error (info);
  else if (gt_info_get_loaded (info))
    {
      const char *b = gt_info_get_branch (info), *oid = gt_info_get_oid (info);
      branch = b != NULL ? g_strdup (b) : oid != NULL ? g_strdup_printf ("Detached at %.7s", oid) : g_strdup ("No commits yet");
      labels[n] = "Branch", values[n++] = branch;
      int ahead = 0, behind = 0;
      const char *up = gt_info_get_upstream (info);
      gboolean counts = gt_info_get_counts (info, &ahead, &behind);
      upstream = up != NULL ? g_strdup (up) : g_strdup ("None (not published)");
      labels[n] = "Upstream", values[n++] = upstream;
      if (counts)
        {
          commits = g_strdup_printf ("%d to push, %d to pull", ahead, behind);
          labels[n] = "Commits", values[n++] = commits;
        }
      GtCommit *c = gt_info_get_last (info);
      if (c != NULL)
        {
          hash = g_strndup (c->hash, 7);
          when = when_text (c->date);
          labels[n] = "Last commit", values[n++] = c->subject;
          labels[n] = "By", values[n] = c->author, n++;
          labels[n] = "When", values[n++] = when;
        }
    }
  else
    labels[n] = "Branch", values[n++] = "…";
  (void) hash;
  p->repo_w = replace (p, p->repo_w, dp_pane_build_group ("Repository", labels, values, n), NULL);
}

static void
render_item (Page *p)
{
  if (p->rels->len == 0 || (p->own_folder && *(const char *) g_ptr_array_index (p->rels, 0) == '\0'))
    return; /* the repository root itself: the Repository group says it all */
  const char *labels[8], *values[8];
  guint n = 0;
  g_autofree char *count = NULL, *summary = NULL, *hash = NULL, *when = NULL;
  const char *title = p->own_folder ? "This Folder" : p->rels->len == 1 ? "Selected Item" : "Selected Items";
  if (!p->item_read)
    labels[n] = "Status", values[n++] = "…";
  else if (p->rels->len == 1)
    {
      const char *rel = g_ptr_array_index (p->rels, 0);
      GtState st = p->status != NULL ? gt_status_state (p->status, rel, p->is_dir) : GT_STATE_NONE;
      const char *label = gt_state_label (st);
      labels[n] = "Status", values[n++] = *label != '\0' ? label : p->last != NULL ? "Unchanged" : "Not in Git";
      labels[n] = "Path", values[n++] = rel;
      if (p->last != NULL)
        {
          hash = g_strndup (p->last->hash, 7);
          when = when_text (p->last->date);
          labels[n] = "Last commit", values[n++] = p->last->subject;
          labels[n] = "Commit", values[n++] = hash;
          labels[n] = "By", values[n] = p->last->author, n++;
          labels[n] = "When", values[n++] = when;
        }
    }
  else
    {
      /* How many of each state. */
      guint per[GT_STATE_CONTAINS + 1] = { 0 };
      for (guint i = 0; i < p->rels->len; i++)
        per[p->status != NULL ? gt_status_state (p->status, g_ptr_array_index (p->rels, i), FALSE) : GT_STATE_NONE]++;
      g_autoptr (GString) s = g_string_new (NULL);
      for (int st = GT_STATE_MODIFIED; st <= GT_STATE_CONTAINS; st++)
        if (per[st] > 0)
          g_string_append_printf (s, "%s%u %s", s->len > 0 ? ", " : "", per[st], gt_state_label (st));
      count = g_strdup_printf ("%u", p->rels->len);
      summary = s->len > 0 ? g_string_free (g_steal_pointer (&s), FALSE) : g_strdup ("No changes");
      labels[n] = "Items", values[n++] = count;
      labels[n] = "Changes", values[n++] = summary;
    }
  p->item_w = replace (p, p->item_w, dp_pane_build_group (title, labels, values, n), p->repo_w);
}

static Page *
page_of (gpointer weak)
{
  g_autoptr (GtkWidget) page = g_weak_ref_get (weak);
  return page != NULL ? g_object_get_data (G_OBJECT (page), "gt-page") : NULL;
}

typedef struct
{
  GWeakRef      page;
  GCancellable *cancel; /* ref: this read's; only the page's current read is applied */
  guint         parts;
} Read;

static void
read_free (Read *r)
{
  g_weak_ref_clear (&r->page);
  g_clear_object (&r->cancel);
  g_free (r);
}

/* The page, if @r is still its current read. */
static Page *
current (Read *r)
{
  Page *p = page_of (&r->page);
  return p != NULL && p->cancel == r->cancel && !g_cancellable_is_cancelled (r->cancel) ? p : NULL;
}

static void
read_part_done (Read *r)
{
  if (--r->parts > 0)
    return;
  Page *p = current (r);
  if (p != NULL)
    {
      p->item_read = TRUE;
      g_clear_object (&p->cancel);
      render_item (p);
    }
  read_free (r);
}

static void
on_status (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  Read *r = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GtRunResult) result = gt_run_finish (res, &error);
  Page *p = current (r);
  if (p != NULL && result != NULL && result->exit_status == 0)
    {
      gsize len = 0;
      const char *d = g_bytes_get_data (result->out, &len);
      g_clear_pointer (&p->status, gt_status_free);
      p->status = gt_status_parse (d, len);
    }
  read_part_done (r);
}

static void
on_log (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  Read *r = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GtRunResult) result = gt_run_finish (res, &error);
  Page *p = current (r);
  if (p != NULL && result != NULL && result->exit_status == 0)
    {
      gsize len = 0;
      const char *d = g_bytes_get_data (result->out, &len);
      g_clear_pointer (&p->last, gt_commit_unref);
      p->last = gt_log_first (d, len);
    }
  read_part_done (r);
}

static void
read_item (Page *p)
{
  if (p->rels->len == 0 || (p->own_folder && *(const char *) g_ptr_array_index (p->rels, 0) == '\0'))
    return;
  if (p->cancel != NULL)
    g_cancellable_cancel (p->cancel);
  g_clear_object (&p->cancel);
  p->cancel = g_cancellable_new ();
  Read *r = g_new0 (Read, 1);
  g_weak_ref_init (&r->page, p->page);
  r->cancel = g_object_ref (p->cancel);
  r->parts = p->rels->len == 1 ? 2 : 1;
  g_autoptr (GStrvBuilder) b = g_strv_builder_new ();
  g_strv_builder_add_many (b, "--literal-pathspecs", "status", "--porcelain=v2", "-z", "--untracked-files=normal",
                           "--ignored=matching", "--", NULL);
  for (guint i = 0; i < p->rels->len; i++)
    {
      const char *rel = g_ptr_array_index (p->rels, i);
      g_strv_builder_add (b, *rel != '\0' ? rel : ".");
    }
  g_auto (GStrv) status = g_strv_builder_end (b);
  gt_run_async (p->root, (const char *const *) status, GT_RUN_READ, NULL, READ_TIMEOUT_MS, p->cancel, NULL, NULL,
                on_status, r);
  if (p->rels->len == 1)
    {
      const char *rel = g_ptr_array_index (p->rels, 0);
      const char *log[] = { "--literal-pathspecs", "log", "-1", "--format=" GT_LOG_FORMAT, "--", *rel != '\0' ? rel : ".", NULL };
      gt_run_async (p->root, log, GT_RUN_READ, NULL, READ_TIMEOUT_MS, p->cancel, NULL, NULL, on_log, r);
    }
}

static void
on_info_changed (GtInfo *info, gpointer data)
{
  (void) info;
  Page *p = data;
  render_repo (p);
  read_item (p); /* a commit or a checkout changes the item's state too */
}

GtkWidget *
gt_page_new (GFile *folder, GPtrArray *files, gpointer user_data)
{
  (void) user_data;
  gboolean own = files == NULL || files->len == 0;
  GFile *first = own ? folder : g_ptr_array_index (files, 0);
  if (first == NULL || !g_file_is_native (first))
    return NULL;
  /* The repository of what is shown: a folder itself, or the selection's folder. */
  g_autofree char *first_path = g_file_get_path (first);
  g_autofree char *dir = own ? g_strdup (first_path) : g_path_get_dirname (first_path);
  g_autofree char *git_dir = NULL;
  g_autofree char *root = dir != NULL ? gt_repo_find_root (dir, &git_dir) : NULL;
  if (root == NULL)
    return NULL;

  Page *p = g_new0 (Page, 1);
  p->root = g_strdup (root);
  p->own_folder = own;
  p->rels = g_ptr_array_new_with_free_func (g_free);
  guint n = own ? 1 : MIN (files->len, MAX_PATHS);
  for (guint i = 0; i < n; i++)
    {
      GFile *f = own ? folder : g_ptr_array_index (files, i);
      g_autofree char *path = g_file_get_path (f);
      char *rel = path != NULL ? gt_repo_relative (root, path) : NULL;
      if (rel != NULL)
        g_ptr_array_add (p->rels, rel);
    }
  p->is_dir = own || (n == 1 && first_path != NULL && g_file_test (first_path, G_FILE_TEST_IS_DIR));
  p->page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_add_css_class (p->page, "gt-pane-page");
  g_object_set_data_full (G_OBJECT (p->page), "gt-page", p, page_free);
  p->info = gt_info_get (root, git_dir);
  p->info_id = g_signal_connect (p->info, "changed", G_CALLBACK (on_info_changed), p);
  render_repo (p);
  render_item (p);
  read_item (p);
  return p->page;
}

char *
gt_page_describe (GtkWidget *page)
{
  Page *p = page != NULL ? g_object_get_data (G_OBJECT (page), "gt-page") : NULL;
  if (p == NULL)
    return g_strdup ("no git page");
  GString *s = g_string_new (NULL);
  for (GtkWidget *g = gtk_widget_get_first_child (page); g != NULL; g = gtk_widget_get_next_sibling (g))
    {
      GtkWidget *heading = gtk_widget_get_first_child (g);
      if (!GTK_IS_LABEL (heading))
        continue;
      g_string_append_printf (s, "[%s:", gtk_label_get_text (GTK_LABEL (heading)));
      GtkWidget *grid = gtk_widget_get_next_sibling (heading);
      for (GtkWidget *c = grid != NULL ? gtk_widget_get_first_child (grid) : NULL; c != NULL; c = gtk_widget_get_next_sibling (c))
        {
          int col = 0;
          gtk_grid_query_child (GTK_GRID (grid), c, &col, NULL, NULL, NULL);
          g_string_append_printf (s, col == 0 ? " %s=" : "%s;", gtk_label_get_text (GTK_LABEL (c)));
        }
      g_string_append (s, "] ");
    }
  return g_string_free (s, FALSE);
}
