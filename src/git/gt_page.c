/* Git view of the details pane. See gt_page.h.
 *
 * Header (instead of the preview): the branch checked out with a HEAD chip, the last commit's message, author and
 * time, the mainline (origin's default branch) with ahead / behind, and the selected item's state.
 * Body (instead of the details): the history from HEAD — with a graph of the branch lines, or as a plain list
 * (setting) — each commit's message, author, time and branch / tag chips. Hovering a commit shows the whole message,
 * the hashes, the author and how much it changed in a native tooltip (setting; a tooltip never takes the pointer, so
 * the other rows still highlight under it). */
#include "gt_page.h"

#include <adwaita.h>

#include <math.h>
#include <stdio.h>  /* sscanf */
#include <stdlib.h> /* atoi */
#include <string.h>

#include "gt_info.h"
#include "gt_repo.h"
#include "gt_run.h"

#define READ_TIMEOUT_MS 120000 /* a cold file cache on a big repository is slow (owner, 2026-11-15) */
#define HISTORY_MAX     "300"
#define MAX_PATHS       200
#define LANE_W          14.0   /* px between graph lanes */
#define GRAPH_PAD       10.0   /* px left of the first lane */
#define NODE_Y          17.0   /* px from a row's top to its node (the middle of the message line) */
#define ROWS_PER_STEP   40     /* history rows added per main-loop turn (216 at once took 20 ms) */

static const char *const lane_colors[] = { "#33d17a", "#62a0ea", "#ffa348", "#c061cb", "#f66151", "#f8e45c", "#2ec27e", "#99c1f1" };

typedef struct
{
  GtkWidget    *page;       /* borrowed (we are its data) */
  GtkWidget    *header;     /* borrowed (the pane holds it) */
  gboolean      graph;
  GtInfo       *info;
  gulong        info_id;
  char         *root;
  GPtrArray    *rels;       /* root-relative paths of what is selected (nothing selected: empty) */
  gboolean      sel_is_dir;
  guint         sel_gen;    /* bumped by every new selection: only the newest selection read is applied */
  /* header */
  GtkWidget    *branch, *subject, *meta, *mainline, *selection;
  GtkWidget    *card_spinner, *refresh, *head_chip;
  gulong        reading_id;
  char         *mainline_ref;
  /* history */
  GtkWidget    *list;
  GtkWidget    *list_spinner; /* while the history is read and nothing is shown yet */
  GPtrArray    *entries;    /* GtLogEntry* */
  GPtrArray    *rows;       /* GtGraphRow* */
  int           max_lanes;
  GCancellable *cancel;     /* everything this page reads */
  guint         append_id;  /* adding history rows in steps, or 0 */
  guint         appended;
  /* tooltips: what was read for a commit's tooltip (full hash → TipInfo*) */
  GHashTable   *tips;
  int           described;  /* commit whose tooltip the debug driver asked for (gt_page_open_card), or -1 */
} Page;

/* A commit's tooltip extras, read when it is first hovered. */
typedef struct
{
  char    *body;      /* the message after its first paragraph, or NULL */
  char    *subject;   /* the whole first paragraph (the list shows git's one-line %s), or NULL */
  int      files, add, del;
  gboolean has_stats;
  gboolean asked;
} TipInfo;

static void
tip_info_free (gpointer data)
{
  TipInfo *t = data;
  g_free (t->body);
  g_free (t->subject);
  g_free (t);
}

static gboolean tooltips_on = TRUE;

static void read_selection (Page *p);

static void
page_free (gpointer data)
{
  Page *p = data;
  g_clear_handle_id (&p->append_id, g_source_remove);
  g_clear_pointer (&p->tips, g_hash_table_unref);
  if (p->cancel != NULL)
    g_cancellable_cancel (p->cancel);
  g_clear_object (&p->cancel);
  if (p->info != NULL)
    {
      g_clear_signal_handler (&p->info_id, p->info);
      g_clear_signal_handler (&p->reading_id, p->info);
    }
  g_clear_object (&p->info);
  g_clear_pointer (&p->entries, g_ptr_array_unref);
  g_clear_pointer (&p->rows, g_ptr_array_unref);
  g_ptr_array_unref (p->rels);
  g_free (p->root);
  g_free (p->mainline_ref);
  g_free (p);
}

static GtkCssProvider *css;

static const char style_sheet[] =
  ".gt-card { background-color: alpha(currentColor, 0.05); border-radius: 12px; padding: 12px; }\n"
  ".gt-chip { border-radius: 9999px; padding: 1px 8px; font-size: 0.85em; font-weight: bold;"
  " background-color: alpha(currentColor, 0.12); }\n"
  ".gt-chip.gt-current { background-color: alpha(#33d17a, 0.22); color: #57e389; }\n"
  ".gt-chip.gt-local { background-color: alpha(#e01b24, 0.22); color: #f66151; }\n"
  ".gt-chip.gt-tag { background-color: alpha(#f6d32d, 0.22); color: #f8e45c; }\n"
  ".gt-history { background: none; }\n"
  ".gt-history > row { padding: 0; border-radius: 8px; }\n"
  ".gt-history > row.gt-head { background-color: alpha(currentColor, 0.08); }\n";

static void
ensure_style (void)
{
  if (css != NULL)
    return;
  css = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (css, style_sheet);
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/* "18:36" today, "Yesterday", "12 Nov", "12 Nov 2025". */
static char *
short_when (gint64 date)
{
  g_autoptr (GDateTime) t = g_date_time_new_from_unix_local (date);
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  if (t == NULL || now == NULL)
    return g_strdup ("");
  int ty, tm, td, ny, nm, nd;
  g_date_time_get_ymd (t, &ty, &tm, &td);
  g_date_time_get_ymd (now, &ny, &nm, &nd);
  if (ty == ny && tm == nm && td == nd)
    return g_date_time_format (t, "%H:%M");
  g_autoptr (GDateTime) yday = g_date_time_add_days (now, -1);
  int yy, ym, yd;
  g_date_time_get_ymd (yday, &yy, &ym, &yd);
  if (ty == yy && tm == ym && td == yd)
    return g_strdup ("Yesterday");
  return g_date_time_format (t, ty == ny ? "%-d %b" : "%-d %b %Y");
}

static GtkWidget *
label (const char *text, const char *css_class, gboolean wrap)
{
  GtkWidget *l = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  if (css_class != NULL)
    gtk_widget_add_css_class (l, css_class);
  if (wrap)
    {
      gtk_label_set_wrap (GTK_LABEL (l), TRUE);
      gtk_label_set_wrap_mode (GTK_LABEL (l), PANGO_WRAP_WORD_CHAR);
    }
  else
    gtk_label_set_ellipsize (GTK_LABEL (l), PANGO_ELLIPSIZE_END);
  return l;
}

static GtkWidget *
chip (const char *text, const char *kind, gboolean branch_icon)
{
  GtkWidget *b = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class (b, "gt-chip");
  if (kind != NULL)
    gtk_widget_add_css_class (b, kind);
  if (branch_icon)
    {
      GtkWidget *i = gtk_image_new_from_icon_name ("nu-git-branch-symbolic");
      gtk_image_set_pixel_size (GTK_IMAGE (i), 12);
      gtk_box_append (GTK_BOX (b), i);
    }
  GtkWidget *l = gtk_label_new (text);
  gtk_label_set_ellipsize (GTK_LABEL (l), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_append (GTK_BOX (b), l);
  gtk_widget_set_valign (b, GTK_ALIGN_CENTER);
  return b;
}

/* ---- reading ---- */

typedef void (*ReadDone) (Page *p, GtRunResult *r);

typedef struct
{
  GWeakRef      page;
  GCancellable *cancel;
  ReadDone      done;
  guint         sel_gen; /* a selection read: the selection it was for (0: not one) */
} Read;

static Page *
page_of (GtkWidget *page)
{
  return page != NULL ? g_object_get_data (G_OBJECT (page), "gt-page") : NULL;
}

static void
on_read (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  Read *r = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GtRunResult) result = gt_run_finish (res, &error);
  g_autoptr (GtkWidget) widget = g_weak_ref_get (&r->page);
  Page *p = page_of (widget);
  /* Only this page's current reads are applied. */
  if (p != NULL && p->cancel == r->cancel && !g_cancellable_is_cancelled (r->cancel) && result != NULL &&
      (r->sel_gen == 0 || r->sel_gen == p->sel_gen))
    r->done (p, result);
  g_weak_ref_clear (&r->page);
  g_clear_object (&r->cancel);
  g_free (r);
}

static void
page_read_full (Page *p, const char *const *args, ReadDone done, guint sel_gen)
{
  Read *r = g_new0 (Read, 1);
  r->sel_gen = sel_gen;
  g_weak_ref_init (&r->page, p->page);
  r->cancel = g_object_ref (p->cancel);
  r->done = done;
  gt_run_async (p->root, args, GT_RUN_READ, NULL, READ_TIMEOUT_MS, p->cancel, NULL, NULL, on_read, r);
}

static void
page_read (Page *p, const char *const *args, ReadDone done)
{
  page_read_full (p, args, done, 0);
}

/* ---- header ---- */

static void
render_header (Page *p)
{
  GtInfo *info = p->info;
  const char *b = gt_info_get_branch (info), *oid = gt_info_get_oid (info);
  const char *error = gt_info_get_error (info);
  gboolean reading = gt_info_is_reading (info);
  gboolean loading = reading && (!gt_info_get_loaded (info) || error != NULL);
  g_autofree char *name = loading           ? g_strdup ("Loading…")
                          : error != NULL   ? g_strdup ("Git error")
                          : b != NULL       ? g_strdup (b)
                          : oid != NULL     ? g_strdup_printf ("Detached at %.7s", oid)
                                            : g_strdup ("No commits yet");
  gtk_label_set_text (GTK_LABEL (p->branch), name);
  /* Loading: a spinner; failed: git's message and a refresh button (owner, 2026-11-15). */
  gtk_widget_set_visible (p->card_spinner, loading);
  gtk_widget_set_visible (p->refresh, error != NULL && !reading);
  gtk_widget_set_visible (p->head_chip, !loading && error == NULL);
  if (error != NULL && !loading)
    {
      gtk_label_set_text (GTK_LABEL (p->subject), error);
      gtk_widget_set_visible (p->subject, TRUE);
      gtk_widget_set_visible (p->meta, FALSE);
      return;
    }
  GtCommit *c = loading ? NULL : gt_info_get_last (info);
  gtk_label_set_text (GTK_LABEL (p->subject), c != NULL ? c->subject : "");
  gtk_widget_set_visible (p->subject, c != NULL);
  g_autofree char *when = c != NULL ? short_when (c->date) : NULL;
  g_autofree char *meta = c != NULL ? g_strdup_printf ("%s · %s", c->author, when) : NULL;
  gtk_label_set_text (GTK_LABEL (p->meta), meta != NULL ? meta : "");
  gtk_widget_set_visible (p->meta, meta != NULL);
}

static void
on_mainline_counts (Page *p, GtRunResult *r)
{
  int ahead = 0, behind = 0;
  gsize len = 0;
  const char *d = g_bytes_get_data (r->out, &len);
  g_autofree char *text = d != NULL ? g_strndup (d, len) : g_strdup ("");
  if (r->exit_status != 0 || sscanf (text, "%d %d", &ahead, &behind) != 2)
    return;
  g_autofree char *line = g_strdup_printf ("Mainline: %s · %d ahead, %d behind", p->mainline_ref, ahead, behind);
  gtk_label_set_text (GTK_LABEL (p->mainline), line);
  gtk_widget_set_visible (p->mainline, TRUE);
}

/* The mainline: origin's default branch (origin/HEAD), else origin/main, origin/master, main or master — not the
 * branch checked out itself. */
static void
on_mainline_refs (Page *p, GtRunResult *r)
{
  gsize len = 0;
  const char *d = g_bytes_get_data (r->out, &len);
  g_autofree char *text = d != NULL ? g_strndup (d, len) : g_strdup ("");
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  const char *current = gt_info_get_branch (p->info);
  g_autofree char *pick = NULL;
  for (int i = 0; lines[i] != NULL && pick == NULL; i++)
    {
      char *tab = strchr (lines[i], '\t');
      if (tab != NULL && tab[1] != '\0')
        pick = g_strdup (tab + 1); /* origin/HEAD → its target */
    }
  for (int i = 0; lines[i] != NULL && pick == NULL; i++)
    {
      char *tab = strchr (lines[i], '\t');
      if (tab != NULL)
        *tab = '\0';
      if (*lines[i] != '\0' && !g_str_has_suffix (lines[i], "/HEAD") && g_strcmp0 (lines[i], current) != 0)
        pick = g_strdup (lines[i]);
    }
  if (pick == NULL || g_strcmp0 (pick, current) == 0)
    return;
  g_free (p->mainline_ref);
  p->mainline_ref = g_steal_pointer (&pick);
  g_autofree char *range = g_strdup_printf ("HEAD...%s", p->mainline_ref);
  const char *args[] = { "rev-list", "--left-right", "--count", range, "--", NULL };
  page_read (p, args, on_mainline_counts);
}

static void
on_selection_status (Page *p, GtRunResult *r)
{
  if (r->exit_status != 0 || p->rels->len == 0)
    return;
  gsize len = 0;
  const char *d = g_bytes_get_data (r->out, &len);
  g_autoptr (GtStatus) st = gt_status_parse (d, len);
  g_autofree char *text = NULL;
  if (p->rels->len == 1)
    {
      const char *rel = g_ptr_array_index (p->rels, 0);
      const char *base = strrchr (rel, '/');
      const char *state = gt_state_label (gt_status_state (st, rel, p->sel_is_dir));
      text = g_strdup_printf ("%s · %s", base != NULL ? base + 1 : rel, *state != '\0' ? state : "Unchanged");
    }
  else
    {
      guint changed = 0;
      for (guint i = 0; i < p->rels->len; i++)
        changed += gt_status_state (st, g_ptr_array_index (p->rels, i), FALSE) != GT_STATE_NONE;
      text = g_strdup_printf ("%u selected · %u with changes", p->rels->len, changed);
    }
  gtk_label_set_text (GTK_LABEL (p->selection), text);
  gtk_widget_set_visible (p->selection, TRUE);
}

static GtkWidget *
build_header (Page *p)
{
  GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class (card, "gt-card");
  GtkWidget *top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  p->card_spinner = adw_spinner_new ();
  gtk_widget_set_size_request (p->card_spinner, 20, 20);
  gtk_widget_set_visible (p->card_spinner, FALSE);
  p->branch = label ("…", "title-3", FALSE);
  gtk_widget_set_hexpand (p->branch, TRUE);
  p->refresh = gtk_button_new_from_icon_name ("view-refresh-symbolic");
  gtk_widget_add_css_class (p->refresh, "flat");
  gtk_widget_set_valign (p->refresh, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (p->refresh, "Read the repository again");
  gtk_widget_set_visible (p->refresh, FALSE);
  p->head_chip = chip ("HEAD", NULL, FALSE);
  gtk_box_append (GTK_BOX (top), p->card_spinner);
  gtk_box_append (GTK_BOX (top), p->branch);
  gtk_box_append (GTK_BOX (top), p->refresh);
  gtk_box_append (GTK_BOX (top), p->head_chip);
  gtk_box_append (GTK_BOX (card), top);
  p->subject = label (NULL, "dim-label", TRUE);
  gtk_label_set_lines (GTK_LABEL (p->subject), 3);
  gtk_label_set_ellipsize (GTK_LABEL (p->subject), PANGO_ELLIPSIZE_END);
  p->meta = label (NULL, "dim-label", FALSE);
  gtk_widget_add_css_class (p->meta, "caption");
  p->mainline = label (NULL, "dim-label", FALSE);
  gtk_widget_add_css_class (p->mainline, "caption");
  gtk_widget_set_visible (p->mainline, FALSE);
  p->selection = label (NULL, "dim-label", FALSE);
  gtk_widget_add_css_class (p->selection, "caption");
  gtk_widget_set_visible (p->selection, FALSE);
  gtk_box_append (GTK_BOX (card), p->subject);
  gtk_box_append (GTK_BOX (card), p->meta);
  gtk_box_append (GTK_BOX (card), p->mainline);
  gtk_box_append (GTK_BOX (card), p->selection);
  return card;
}

/* ---- graph ---- */

static void
set_color (cairo_t *cr, int index, double alpha)
{
  GdkRGBA c;
  gdk_rgba_parse (&c, lane_colors[(guint) index % G_N_ELEMENTS (lane_colors)]);
  cairo_set_source_rgba (cr, c.red, c.green, c.blue, alpha);
}

static double
lane_x (int lane)
{
  return GRAPH_PAD + lane * LANE_W;
}

static void
draw_graph (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  (void) area, (void) width;
  const GtGraphRow *row = data;
  cairo_set_line_width (cr, 2);
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
  for (guint i = 0; i < row->segs->len; i++)
    {
      const GtGraphSeg *s = &g_array_index (row->segs, GtGraphSeg, i);
      double x1 = lane_x (s->from), x2 = lane_x (s->to);
      double y1 = s->top ? 0 : NODE_Y, y2 = s->top ? NODE_Y : height;
      set_color (cr, s->color, 1);
      cairo_move_to (cr, x1, y1);
      if (s->from == s->to)
        cairo_line_to (cr, x2, y2);
      else
        {
          double mid = (y1 + y2) / 2;
          cairo_curve_to (cr, x1, mid, x2, mid, x2, y2);
        }
      cairo_stroke (cr);
    }
  double x = lane_x (row->node);
  cairo_arc (cr, x, NODE_Y, row->merge ? 4.5 : 4, 0, 2 * G_PI);
  if (row->merge)
    {
      /* A merge: a ring. */
      set_color (cr, row->node_color, 1);
      cairo_set_line_width (cr, 2);
      cairo_stroke (cr);
    }
  else
    {
      set_color (cr, row->node_color, 1);
      cairo_fill (cr);
    }
}

/* ---- commit tooltips ---- */

void
gt_page_set_tooltips (gboolean on)
{
  tooltips_on = on;
}

/* Reads finish after the page or the row may be gone: they find both through weak refs and the page's data. */
typedef struct
{
  GWeakRef page;   /* the page widget (its Page is its data) */
  GWeakRef row;    /* the row whose tooltip to refresh */
  char    *hash;
} TipRead;

static void
tip_read_free (TipRead *r)
{
  g_weak_ref_clear (&r->page);
  g_weak_ref_clear (&r->row);
  g_free (r->hash);
  g_free (r);
}

static TipRead *
tip_read_new (Page *p, GtkWidget *row, const char *hash)
{
  TipRead *r = g_new0 (TipRead, 1);
  g_weak_ref_init (&r->page, p->page);
  g_weak_ref_init (&r->row, row);
  r->hash = g_strdup (hash);
  return r;
}

/* The TipInfo a read is for (NULL: the page is gone) and, in *@row_out, a new ref on its row or NULL. */
static TipInfo *
tip_lookup (TipRead *r, GtkWidget **row_out)
{
  g_autoptr (GtkWidget) page = g_weak_ref_get (&r->page);
  Page *p = page != NULL ? page_of (page) : NULL;
  *row_out = g_weak_ref_get (&r->row);
  return p != NULL && p->tips != NULL ? g_hash_table_lookup (p->tips, r->hash) : NULL;
}

static int
number_before (const char *text, const char *word)
{
  const char *s = strstr (text, word);
  if (s == NULL)
    return 0;
  const char *b = s;
  while (b > text && g_ascii_isdigit (b[-1]))
    b--;
  return atoi (b);
}

static void
on_message (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  TipRead *r = data;
  g_autoptr (GtRunResult) out = gt_run_finish (res, NULL);
  GtkWidget *row = NULL;
  TipInfo *t = tip_lookup (r, &row);
  if (out != NULL && out->exit_status == 0 && t != NULL)
    {
      gsize len = 0;
      const char *d = g_bytes_get_data (out->out, &len);
      g_autofree char *msg = d != NULL ? g_strndup (d, len) : g_strdup ("");
      g_strstrip (msg);
      char *nl = strstr (msg, "\n\n");
      if (nl != NULL)
        {
          *nl = '\0';
          t->body = g_strdup (g_strstrip (nl + 2));
        }
      t->subject = g_strdup (msg);
      if (row != NULL)
        gtk_widget_trigger_tooltip_query (row);
    }
  g_clear_object (&row);
  tip_read_free (r);
}

static void
on_stats (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  TipRead *r = data;
  g_autoptr (GtRunResult) out = gt_run_finish (res, NULL);
  GtkWidget *row = NULL;
  TipInfo *t = tip_lookup (r, &row);
  if (out != NULL && out->exit_status == 0 && t != NULL)
    {
      gsize len = 0;
      const char *d = g_bytes_get_data (out->out, &len);
      g_autofree char *text = d != NULL ? g_strndup (d, len) : g_strdup ("");
      /* " 10 files changed, 108 insertions(+), 22 deletions(-)" (parts may be missing) */
      t->files = number_before (text, " file");
      t->add = number_before (text, " insertion");
      t->del = number_before (text, " deletion");
      t->has_stats = TRUE;
      if (row != NULL)
        gtk_widget_trigger_tooltip_query (row);
    }
  g_clear_object (&row);
  tip_read_free (r);
}

/* The tooltip's extras for commit @e, read once (the whole message, and how much it changed — a merge: against its
 * first parent). Borrowed (the page's). */
static TipInfo *
tip_info (Page *p, GtLogEntry *e, GtkWidget *row)
{
  if (p->tips == NULL)
    p->tips = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, tip_info_free);
  TipInfo *t = g_hash_table_lookup (p->tips, e->hash);
  if (t == NULL)
    {
      t = g_new0 (TipInfo, 1);
      g_hash_table_insert (p->tips, g_strdup (e->hash), t);
    }
  if (t->asked)
    return t;
  t->asked = TRUE;
  const char *msg_args[] = { "show", "-s", "--format=%B", e->hash, "--", NULL };
  gt_run_async (p->root, msg_args, GT_RUN_READ, NULL, READ_TIMEOUT_MS, p->cancel, NULL, NULL, on_message,
                tip_read_new (p, row, e->hash));
  if (e->parents[0] != NULL)
    {
      const char *diff_args[] = { "diff", "--shortstat", e->parents[0], e->hash, "--", NULL };
      gt_run_async (p->root, diff_args, GT_RUN_READ, NULL, READ_TIMEOUT_MS, p->cancel, NULL, NULL, on_stats,
                    tip_read_new (p, row, e->hash));
    }
  else
    {
      const char *show_args[] = { "show", "--shortstat", "--format=", e->hash, "--", NULL };
      gt_run_async (p->root, show_args, GT_RUN_READ, NULL, READ_TIMEOUT_MS, p->cancel, NULL, NULL, on_stats,
                    tip_read_new (p, row, e->hash));
    }
  return t;
}

/* The tooltip's parts as plain text, in order (debug driver): title, body, hash, parents…, author · time, then the
 * statistics once read. A new NULL-terminated array. */
static GStrv
tip_parts (GtLogEntry *e, TipInfo *t)
{
  g_autoptr (GStrvBuilder) b = g_strv_builder_new ();
  g_strv_builder_add (b, t != NULL && t->subject != NULL ? t->subject : e->subject);
  if (t != NULL && t->body != NULL && *t->body != '\0')
    g_strv_builder_add (b, t->body);
  g_strv_builder_add (b, e->hash);
  for (int i = 0; e->parents[i] != NULL; i++)
    g_strv_builder_take (b, g_strdup_printf ("↑ %s", e->parents[i]));
  g_autofree char *when = short_when (e->date);
  g_strv_builder_take (b, g_strdup_printf ("%s · %s", e->author, when));
  if (t != NULL && t->has_stats)
    {
      g_strv_builder_take (b, g_strdup_printf ("%d file%s changed ·", t->files, t->files == 1 ? "" : "s"));
      g_strv_builder_take (b, g_strdup_printf ("+%d", t->add));
      g_strv_builder_take (b, g_strdup_printf ("−%d", t->del));
    }
  return g_strv_builder_end (b);
}

/* A long message is cut, so the tooltip stays smaller than the window. New string. */
static char *
clip_lines (const char *text, guint max_lines)
{
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  if (g_strv_length (lines) <= max_lines)
    return g_strdup (text);
  g_autoptr (GString) out = g_string_new (NULL);
  for (guint i = 0; i < max_lines; i++)
    g_string_append_printf (out, "%s\n", lines[i]);
  g_string_append (out, "…");
  return g_string_free (g_steal_pointer (&out), FALSE);
}

static gboolean
on_row_query_tooltip (GtkWidget *row, int x, int y, gboolean keyboard, GtkTooltip *tooltip, gpointer data)
{
  (void) x, (void) y, (void) keyboard;
  Page *p = data;
  int index = gtk_list_box_row_get_index (GTK_LIST_BOX_ROW (row));
  if (!tooltips_on || p->entries == NULL || index < 0 || (guint) index >= p->entries->len)
    return FALSE;
  GtLogEntry *e = g_ptr_array_index (p->entries, index);
  TipInfo *t = tip_info (p, e, row);
  g_autofree char *subject = g_markup_escape_text (t->subject != NULL ? t->subject : e->subject, -1);
  g_autoptr (GString) m = g_string_new (NULL);
  g_string_append_printf (m, "<b>%s</b>", subject);
  if (t->body != NULL && *t->body != '\0')
    {
      g_autofree char *clipped = clip_lines (t->body, 16);
      g_autofree char *body = g_markup_escape_text (clipped, -1);
      g_string_append_printf (m, "\n\n%s", body);
    }
  g_string_append_printf (m, "\n\n<tt>%s</tt>", e->hash);
  for (int i = 0; e->parents[i] != NULL; i++)
    g_string_append_printf (m, "\n<span alpha=\"70%%\"><tt>↑ %s</tt></span>", e->parents[i]);
  g_autofree char *when = short_when (e->date);
  g_autofree char *who = g_markup_printf_escaped ("\n%s · %s", e->author, when);
  g_string_append (m, who);
  if (t->has_stats)
    g_string_append_printf (m, "\n%d file%s changed · <span foreground=\"#2ec27e\">+%d</span> "
                               "<span foreground=\"#e01b24\">−%d</span>",
                            t->files, t->files == 1 ? "" : "s", t->add, t->del);
  gtk_tooltip_set_markup (tooltip, m->str);
  return TRUE;
}

/* ---- history ---- */

static GtkWidget *
commit_row (Page *p, guint index)
{
  GtLogEntry *e = g_ptr_array_index (p->entries, index);
  GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  if (p->graph)
    {
      GtkWidget *area = gtk_drawing_area_new ();
      gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (area), (int) ceil (GRAPH_PAD * 2 + (p->max_lanes - 1) * LANE_W));
      gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (area), draw_graph, g_ptr_array_index (p->rows, index), NULL);
      gtk_widget_set_vexpand (area, TRUE);
      gtk_box_append (GTK_BOX (hbox), area);
    }
  GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (text, TRUE);
  gtk_widget_set_margin_top (text, 7);
  gtk_widget_set_margin_bottom (text, 7);
  gtk_widget_set_margin_end (text, 8);
  if (!p->graph)
    gtk_widget_set_margin_start (text, 8);
  GtkWidget *subject = label (e->subject, NULL, FALSE);
  g_autofree char *when = short_when (e->date);
  g_autofree char *meta_text = g_strdup_printf ("%s · %s", e->author, when);
  GtkWidget *meta = label (meta_text, "dim-label", FALSE);
  gtk_widget_add_css_class (meta, "caption");
  gtk_box_append (GTK_BOX (text), subject);
  gtk_box_append (GTK_BOX (text), meta);
  if (e->refs->len > 0)
    {
      /* Wrapping: every chip stays readable, however many a commit has. */
      GtkWidget *chips = adw_wrap_box_new ();
      adw_wrap_box_set_child_spacing (ADW_WRAP_BOX (chips), 4);
      adw_wrap_box_set_line_spacing (ADW_WRAP_BOX (chips), 4);
      gtk_widget_set_margin_top (chips, 2);
      if (!p->graph)
        adw_wrap_box_set_justify (ADW_WRAP_BOX (chips), ADW_JUSTIFY_NONE),
          adw_wrap_box_set_pack_direction (ADW_WRAP_BOX (chips), ADW_PACK_END_TO_START);
      for (guint i = 0; i < e->refs->len; i++)
        {
          GtRef *r = g_ptr_array_index (e->refs, i);
          const char *kind = r->kind == GT_REF_CURRENT ? "gt-current"
                             : r->kind == GT_REF_LOCAL ? "gt-local"
                             : r->kind == GT_REF_TAG   ? "gt-tag"
                                                       : NULL;
          adw_wrap_box_append (ADW_WRAP_BOX (chips), chip (r->name, kind, r->kind != GT_REF_TAG && r->kind != GT_REF_HEAD));
        }
      gtk_box_append (GTK_BOX (text), chips);
    }
  gtk_box_append (GTK_BOX (hbox), text);
  GtkWidget *row = gtk_list_box_row_new ();
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), hbox);
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
  if (index == 0)
    gtk_widget_add_css_class (row, "gt-head"); /* the commit checked out */
  gtk_widget_set_has_tooltip (row, TRUE); /* on_row_query_tooltip answers nothing while the setting is off */
  g_signal_connect (row, "query-tooltip", G_CALLBACK (on_row_query_tooltip), p);
  gtk_accessible_update_property (GTK_ACCESSIBLE (row), GTK_ACCESSIBLE_PROPERTY_LABEL, e->subject, -1);
  return row;
}

/* Adds the next ROWS_PER_STEP history rows (one main-loop turn each, so a long history never stalls a frame).
 * TRUE while rows are left. */
static gboolean
append_rows (gpointer data)
{
  Page *p = data;
  gint64 t0 = g_get_monotonic_time ();
  guint end = p->entries != NULL ? MIN (p->entries->len, p->appended + ROWS_PER_STEP) : 0;
  for (; p->appended < end; p->appended++)
    gtk_list_box_append (GTK_LIST_BOX (p->list), commit_row (p, p->appended));
  gint64 dt = g_get_monotonic_time () - t0;
  if (dt > 16000)
    g_debug ("slow main-thread step: git history rows took %" G_GINT64_FORMAT " µs", dt);
  if (p->entries == NULL || p->appended >= p->entries->len)
    {
      p->append_id = 0;
      return G_SOURCE_REMOVE;
    }
  return G_SOURCE_CONTINUE;
}

static void
on_history (Page *p, GtRunResult *r)
{
  gtk_widget_set_visible (p->list_spinner, FALSE);
  GtkWidget *c;
  while ((c = gtk_widget_get_first_child (p->list)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (p->list), c);
  g_clear_pointer (&p->rows, g_ptr_array_unref);
  g_clear_pointer (&p->entries, g_ptr_array_unref);
  if (r->exit_status != 0)
    return; /* no commits yet */
  gsize len = 0;
  const char *d = g_bytes_get_data (r->out, &len);
  p->entries = gt_history_parse (d, len);
  p->rows = gt_graph_layout (p->entries);
  p->max_lanes = 1;
  for (guint i = 0; i < p->rows->len; i++)
    p->max_lanes = MAX (p->max_lanes, ((GtGraphRow *) g_ptr_array_index (p->rows, i))->n_lanes);
  p->appended = 0;
  g_clear_handle_id (&p->append_id, g_source_remove);
  if (append_rows (p))
    p->append_id = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, append_rows, p, NULL);
}

static void
read_all (Page *p)
{
  if (p->cancel != NULL)
    g_cancellable_cancel (p->cancel);
  g_clear_object (&p->cancel);
  p->cancel = g_cancellable_new ();
  const char *history[] = { "log", "--topo-order", "--decorate=full", "-n", HISTORY_MAX,
                            "--format=" GT_HISTORY_FORMAT, "HEAD", "--", NULL };
  /* A spinner until the history is in, unless some is already shown (a re-read after a commit). */
  gtk_widget_set_visible (p->list_spinner, p->entries == NULL || p->entries->len == 0);
  page_read (p, history, on_history);
  const char *refs[] = { "for-each-ref", "--format=%(refname:short)%09%(symref:short)", "refs/remotes/origin/HEAD",
                         "refs/remotes/origin/main", "refs/remotes/origin/master", "refs/heads/main", "refs/heads/master",
                         NULL };
  page_read (p, refs, on_mainline_refs);
  read_selection (p);
}

/* The selection line (a new selection reads it again; the history and the card are left alone). */
static void
read_selection (Page *p)
{
  if (p->rels->len > 0)
    {
      g_autoptr (GStrvBuilder) b = g_strv_builder_new ();
      g_strv_builder_add_many (b, "--literal-pathspecs", "status", "--porcelain=v2", "-z", "--untracked-files=normal",
                               "--ignored=matching", "--", NULL);
      for (guint i = 0; i < p->rels->len; i++)
        {
          const char *rel = g_ptr_array_index (p->rels, i);
          g_strv_builder_add (b, *rel != '\0' ? rel : ".");
        }
      g_auto (GStrv) status = g_strv_builder_end (b);
      page_read_full (p, (const char *const *) status, on_selection_status, p->sel_gen);
    }
}

static void
on_info_changed (GtInfo *info, gpointer data)
{
  (void) info;
  Page *p = data;
  render_header (p);
  read_all (p); /* a commit, a checkout or a fetch changes the history and the counts */
}

/* The repository of what is shown (a folder itself, or the selection's folder), or NULL. */
static char *
repo_of (GFile *folder, GPtrArray *files, char **git_dir, char **first_path)
{
  gboolean own = files == NULL || files->len == 0;
  GFile *first = own ? folder : g_ptr_array_index (files, 0);
  if (first == NULL || !g_file_is_native (first))
    return NULL;
  char *path = g_file_get_path (first);
  g_autofree char *dir = path == NULL ? NULL : own ? g_strdup (path) : g_path_get_dirname (path);
  if (first_path != NULL)
    *first_path = path;
  else
    g_free (path);
  return dir != NULL ? gt_repo_find_root (dir, git_dir) : NULL;
}

gboolean
gt_page_available (GFile *folder, GPtrArray *files, gpointer user_data)
{
  (void) user_data;
  g_autofree char *root = repo_of (folder, files, NULL, NULL);
  return root != NULL;
}

/* What is selected, root-relative (nothing selected: empty). */
static void
set_selection (Page *p, GPtrArray *files, const char *first_path)
{
  gboolean own = files == NULL || files->len == 0;
  p->sel_gen++;
  g_ptr_array_set_size (p->rels, 0);
  for (guint i = 0; !own && i < MIN (files->len, MAX_PATHS); i++)
    {
      g_autofree char *path = g_file_get_path (g_ptr_array_index (files, i));
      char *rel = path != NULL ? gt_repo_relative (p->root, path) : NULL;
      if (rel != NULL)
        g_ptr_array_add (p->rels, rel);
    }
  p->sel_is_dir = !own && files->len == 1 && first_path != NULL && g_file_test (first_path, G_FILE_TEST_IS_DIR);
  gtk_widget_set_visible (p->selection, FALSE);
}

GtkWidget *
gt_page_new (GFile *folder, GPtrArray *files, GtkWidget *current, GtkWidget **header, gpointer user_data)
{
  g_autofree char *git_dir = NULL, *first_path = NULL;
  g_autofree char *root = repo_of (folder, files, &git_dir, &first_path);
  if (root == NULL)
    return NULL;
  /* The same repository (and look): keep the page — branch card and history stay; the selection line is read again. */
  Page *old = page_of (current);
  if (old != NULL && g_str_equal (old->root, root) && old->graph == GPOINTER_TO_INT (user_data))
    {
      set_selection (old, files, first_path);
      read_selection (old);
      return current;
    }
  ensure_style ();

  Page *p = g_new0 (Page, 1);
  p->described = -1;
  p->root = g_strdup (root);
  p->graph = GPOINTER_TO_INT (user_data);
  p->rels = g_ptr_array_new_with_free_func (g_free);
  p->page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class (p->page, "gt-pane-page");
  p->list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (p->list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (p->list, "gt-history");
  gtk_accessible_update_property (GTK_ACCESSIBLE (p->list), GTK_ACCESSIBLE_PROPERTY_LABEL, "History", -1);
  p->list_spinner = adw_spinner_new ();
  gtk_widget_set_size_request (p->list_spinner, 32, 32);
  gtk_widget_set_halign (p->list_spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (p->list_spinner, 24);
  gtk_box_append (GTK_BOX (p->page), p->list_spinner);
  gtk_box_append (GTK_BOX (p->page), p->list);
  g_object_set_data_full (G_OBJECT (p->page), "gt-page", p, page_free);
  p->header = build_header (p);
  set_selection (p, files, first_path);
  if (header != NULL)
    *header = p->header;
  p->info = gt_info_get (root, git_dir);
  p->info_id = g_signal_connect (p->info, "changed", G_CALLBACK (on_info_changed), p);
  p->reading_id = g_signal_connect_swapped (p->info, "reading-changed", G_CALLBACK (render_header), p);
  g_signal_connect_swapped (p->refresh, "clicked", G_CALLBACK (gt_info_refresh), p->info);
  render_header (p);
  read_all (p);
  return p->page;
}

gboolean
gt_page_open_card (GtkWidget *page, guint index)
{
  Page *p = page_of (page);
  GtkListBoxRow *row = p != NULL ? gtk_list_box_get_row_at_index (GTK_LIST_BOX (p->list), (int) index) : NULL;
  if (row == NULL)
    return FALSE;
  p->described = (int) index;
  tip_info (p, g_ptr_array_index (p->entries, index), GTK_WIDGET (row)); /* starts the reads */
  return TRUE;
}

char *
gt_page_describe (GtkWidget *page)
{
  Page *p = page_of (page);
  if (p == NULL)
    return g_strdup ("no git page");
  GString *s = g_string_new (NULL);
  g_string_append_printf (s, "loading=%d refresh=%d spinner=%d ", gtk_widget_get_visible (p->card_spinner),
                          gtk_widget_get_visible (p->refresh), gtk_widget_get_visible (p->list_spinner));
  g_string_append_printf (s, "card=[%s|%s|%s|%s|%s] graph=%d commits=%u lanes=%d", gtk_label_get_text (GTK_LABEL (p->branch)),
                          gtk_label_get_text (GTK_LABEL (p->subject)), gtk_label_get_text (GTK_LABEL (p->meta)),
                          gtk_widget_get_visible (p->mainline) ? gtk_label_get_text (GTK_LABEL (p->mainline)) : "",
                          gtk_widget_get_visible (p->selection) ? gtk_label_get_text (GTK_LABEL (p->selection)) : "",
                          p->graph, p->entries != NULL ? p->entries->len : 0, p->max_lanes);
  if (p->described >= 0 && p->entries != NULL && (guint) p->described < p->entries->len)
    {
      GtLogEntry *e = g_ptr_array_index (p->entries, p->described);
      g_auto (GStrv) parts = tip_parts (e, p->tips != NULL ? g_hash_table_lookup (p->tips, e->hash) : NULL);
      g_string_append_printf (s, " tooltips=%d hover=[", tooltips_on);
      for (char **k = parts; *k != NULL; k++)
        g_string_append_printf (s, "%s|", *k);
      g_string_append (s, "]");
    }
  for (guint i = 0; p->entries != NULL && i < MIN (p->entries->len, 3); i++)
    {
      GtLogEntry *e = g_ptr_array_index (p->entries, i);
      g_string_append_printf (s, " commit%u=%s", i, e->subject);
      for (guint j = 0; j < e->refs->len; j++)
        g_string_append_printf (s, "{%s}", ((GtRef *) g_ptr_array_index (e->refs, j))->name);
    }
  return g_string_free (s, FALSE);
}
