/* Git parsers. See gt_parse.h. */
#define _POSIX_C_SOURCE 200809L /* strnlen */
#include "gt_parse.h"

#include <stdio.h>  /* sscanf */
#include <stdlib.h> /* strtol */
#include <string.h>

const char *
gt_state_label (GtState state)
{
  switch (state)
    {
    case GT_STATE_MODIFIED:     return "Modified";
    case GT_STATE_ADDED:        return "Added";
    case GT_STATE_DELETED:      return "Deleted";
    case GT_STATE_RENAMED:      return "Renamed";
    case GT_STATE_COPIED:       return "Copied";
    case GT_STATE_TYPE_CHANGED: return "Type changed";
    case GT_STATE_UNTRACKED:    return "Untracked";
    case GT_STATE_IGNORED:      return "Ignored";
    case GT_STATE_CONFLICT:     return "Conflict";
    case GT_STATE_CONTAINS:     return "Contains changes";
    case GT_STATE_NONE:
    default:                    return "";
    }
}

/* ---- status ---- */

/* Next NUL-terminated field of @data at *@pos (advanced past its NUL), or NULL at the end. */
static const char *
next_field (const char *data, gsize len, gsize *pos, gsize *flen)
{
  if (*pos >= len)
    return NULL;
  const char *f = data + *pos;
  gsize n = strnlen (f, len - *pos);
  *flen = n;
  *pos += n + 1;
  return f;
}

/* XY → state (porcelain v2 changed entries). */
static GtState
xy_state (char x, char y)
{
  char c[2] = { x, y };
  for (int i = 0; i < 2; i++)
    if (c[i] == 'U')
      return GT_STATE_CONFLICT;
  if (x == 'A' && y == 'A')
    return GT_STATE_CONFLICT;
  if (x == 'D' && y == 'D')
    return GT_STATE_CONFLICT;
  for (int i = 0; i < 2; i++)
    if (c[i] == 'A')
      return GT_STATE_ADDED;
  for (int i = 0; i < 2; i++)
    if (c[i] == 'R')
      return GT_STATE_RENAMED;
  for (int i = 0; i < 2; i++)
    if (c[i] == 'C')
      return GT_STATE_COPIED;
  for (int i = 0; i < 2; i++)
    if (c[i] == 'D')
      return GT_STATE_DELETED;
  for (int i = 0; i < 2; i++)
    if (c[i] == 'T')
      return GT_STATE_TYPE_CHANGED;
  return GT_STATE_MODIFIED;
}

/* The path after @n space-separated fields of @rec, or NULL. */
static const char *
after_fields (const char *rec, gsize rlen, int n)
{
  int seen = 0;
  for (gsize i = 0; i < rlen; i++)
    if (rec[i] == ' ' && ++seen == n)
      return i + 1 < rlen ? rec + i + 1 : NULL;
  return NULL;
}

static void
add_file (GtStatus *st, const char *path, gsize plen, GtState state)
{
  while (plen > 0 && path[plen - 1] == '/') /* untracked / ignored folders end in '/' */
    plen--;
  if (plen == 0)
    return;
  char *p = g_strndup (path, plen);
  if (state != GT_STATE_IGNORED)
    for (char *slash = strchr (p, '/'); slash != NULL; slash = strchr (slash + 1, '/'))
      g_hash_table_add (st->dirty, g_strndup (p, (gsize) (slash - p)));
  g_hash_table_replace (st->files, p, GINT_TO_POINTER (state));
}

GtStatus *
gt_status_parse (const char *data, gsize len)
{
  GtStatus *st = g_new0 (GtStatus, 1);
  st->files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  st->dirty = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  gsize pos = 0, rlen = 0;
  for (const char *rec; data != NULL && (rec = next_field (data, len, &pos, &rlen)) != NULL;)
    {
      if (rlen < 2)
        continue;
      if (rec[0] == '#')
        {
          g_autofree char *line = g_strndup (rec, rlen);
          if (g_str_has_prefix (line, "# branch.oid "))
            {
              const char *v = line + strlen ("# branch.oid ");
              g_clear_pointer (&st->oid, g_free);
              st->oid = g_str_equal (v, "(initial)") ? NULL : g_strdup (v);
            }
          else if (g_str_has_prefix (line, "# branch.head "))
            {
              const char *v = line + strlen ("# branch.head ");
              g_clear_pointer (&st->head, g_free);
              st->head = g_str_equal (v, "(detached)") ? NULL : g_strdup (v);
            }
          else if (g_str_has_prefix (line, "# branch.upstream "))
            {
              g_clear_pointer (&st->upstream, g_free);
              st->upstream = g_strdup (line + strlen ("# branch.upstream "));
            }
          else if (g_str_has_prefix (line, "# branch.ab "))
            {
              int a = 0, b = 0;
              if (sscanf (line + strlen ("# branch.ab "), "+%d -%d", &a, &b) == 2)
                st->has_ab = TRUE, st->ahead = a, st->behind = b;
            }
          continue;
        }
      const char *path = NULL;
      GtState state = GT_STATE_NONE;
      switch (rec[0])
        {
        case '1': /* 1 XY sub mH mI mW hH hI path */
          if (rlen > 4)
            path = after_fields (rec, rlen, 8), state = xy_state (rec[2], rec[3]);
          break;
        case '2': /* 2 XY sub mH mI mW hH hI Xscore path \0 origPath */
          if (rlen > 4)
            {
              path = after_fields (rec, rlen, 9), state = xy_state (rec[2], rec[3]);
              gsize olen = 0;
              (void) next_field (data, len, &pos, &olen); /* the original path */
            }
          break;
        case 'u': /* u XY sub m1 m2 m3 mW h1 h2 h3 path */
          path = after_fields (rec, rlen, 10), state = GT_STATE_CONFLICT;
          break;
        case '?':
          path = rec + 2, state = GT_STATE_UNTRACKED;
          break;
        case '!':
          path = rec + 2, state = GT_STATE_IGNORED;
          break;
        default:
          break;
        }
      if (path != NULL && path >= rec && path < rec + rlen)
        add_file (st, path, rlen - (gsize) (path - rec), state);
    }
  return st;
}

void
gt_status_free (GtStatus *status)
{
  if (status == NULL)
    return;
  g_free (status->oid);
  g_free (status->head);
  g_free (status->upstream);
  g_hash_table_unref (status->files);
  g_hash_table_unref (status->dirty);
  g_free (status);
}

GtState
gt_status_state (const GtStatus *status, const char *path, gboolean is_dir)
{
  g_return_val_if_fail (status != NULL && path != NULL, GT_STATE_NONE);
  gpointer v;
  if (g_hash_table_lookup_extended (status->files, path, NULL, &v))
    return (GtState) GPOINTER_TO_INT (v);
  if (is_dir && g_hash_table_contains (status->dirty, path))
    return GT_STATE_CONTAINS;
  /* Inside an untracked or ignored folder (git lists only the folder). */
  g_autofree char *p = g_strdup (path);
  for (char *slash = strrchr (p, '/'); slash != NULL; slash = strrchr (p, '/'))
    {
      *slash = '\0';
      if (g_hash_table_lookup_extended (status->files, p, NULL, &v))
        {
          GtState s = (GtState) GPOINTER_TO_INT (v);
          return s == GT_STATE_UNTRACKED || s == GT_STATE_IGNORED ? s : GT_STATE_NONE;
        }
    }
  return GT_STATE_NONE;
}

/* ---- branches ---- */

void
gt_branch_free (GtBranch *branch)
{
  if (branch == NULL)
    return;
  g_free (branch->name);
  g_free (branch->upstream);
  g_free (branch->hash);
  g_free (branch->subject);
  g_free (branch);
}

static int
branch_cmp (gconstpointer pa, gconstpointer pb)
{
  const GtBranch *a = *(GtBranch *const *) pa, *b = *(GtBranch *const *) pb;
  if (a->remote != b->remote)
    return a->remote ? 1 : -1;
  if (a->current != b->current)
    return a->current ? -1 : 1;
  return g_utf8_collate (a->name, b->name);
}

GPtrArray *
gt_branches_parse (const char *data, gsize len)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) gt_branch_free);
  /* for-each-ref prints %00 as NUL inside each line; lines end in '\n'. */
  gsize pos = 0;
  while (pos < len)
    {
      const char *line = data + pos;
      const char *nl = memchr (line, '\n', len - pos);
      gsize llen = nl != NULL ? (gsize) (nl - line) : len - pos;
      pos += llen + 1;
      const char *f[6] = { NULL };
      gsize fl[6] = { 0 };
      gsize p = 0;
      int n = 0;
      for (; n < 6 && p <= llen; n++)
        {
          const char *s = line + p;
          const char *z = memchr (s, '\0', llen - p);
          gsize l = z != NULL ? (gsize) (z - s) : llen - p;
          f[n] = s, fl[n] = l;
          p += l + 1;
          if (z == NULL)
            {
              n++;
              break;
            }
        }
      if (n < 6)
        continue;
      g_autofree char *ref = g_strndup (f[0], fl[0]);
      gboolean remote = g_str_has_prefix (ref, "refs/remotes/");
      const char *name = remote ? ref + strlen ("refs/remotes/") : g_str_has_prefix (ref, "refs/heads/") ? ref + strlen ("refs/heads/") : NULL;
      if (name == NULL || *name == '\0' || (remote && g_str_has_suffix (name, "/HEAD")))
        continue;
      GtBranch *b = g_new0 (GtBranch, 1);
      b->name = g_strdup (name);
      b->remote = remote;
      b->current = fl[1] == 1 && f[1][0] == '*';
      b->upstream = fl[2] > 0 ? g_strndup (f[2], fl[2]) : NULL;
      b->hash = g_strndup (f[3], fl[3]);
      g_autofree char *date = g_strndup (f[4], fl[4]);
      b->date = g_ascii_strtoll (date, NULL, 10);
      b->subject = g_strndup (f[5], fl[5]);
      g_ptr_array_add (out, b);
    }
  g_ptr_array_sort (out, branch_cmp);
  return out;
}

/* ---- log ---- */

GtCommit *
gt_commit_ref (GtCommit *commit)
{
  g_return_val_if_fail (commit != NULL, NULL);
  g_atomic_ref_count_inc (&commit->ref);
  return commit;
}

void
gt_commit_unref (GtCommit *commit)
{
  if (commit == NULL || !g_atomic_ref_count_dec (&commit->ref))
    return;
  g_free (commit->hash);
  g_free (commit->author);
  g_free (commit->subject);
  g_free (commit);
}

/* Parses the header of one record (after its \x1e): hash, author, date, subject; *rest = the name list. */
static GtCommit *
parse_header (const char *rec, gsize rlen, const char **rest, gsize *rest_len)
{
  const char *f[4] = { NULL };
  gsize fl[4] = { 0 }, p = 0;
  for (int i = 0; i < 4; i++)
    {
      if (p > rlen)
        return NULL;
      const char *s = rec + p;
      const char *z = memchr (s, '\0', rlen - p);
      if (z == NULL)
        {
          if (i < 3)
            return NULL;
          f[i] = s, fl[i] = rlen - p, p = rlen;
          break;
        }
      f[i] = s, fl[i] = (gsize) (z - s);
      p += fl[i] + 1;
    }
  GtCommit *c = g_new0 (GtCommit, 1);
  g_atomic_ref_count_init (&c->ref);
  c->hash = g_strndup (f[0], fl[0]);
  c->author = g_strndup (f[1], fl[1]);
  g_autofree char *date = g_strndup (f[2], fl[2]);
  c->date = g_ascii_strtoll (date, NULL, 10);
  c->subject = g_strndup (f[3], fl[3]);
  while (c->subject[0] != '\0' && c->subject[strlen (c->subject) - 1] == '\n')
    c->subject[strlen (c->subject) - 1] = '\0';
  *rest = rec + MIN (p, rlen);
  *rest_len = p <= rlen ? rlen - p : 0;
  return c;
}

/* Calls @func for every record in @data (it takes the commit's reference). */
static void
each_record (const char *data, gsize len, void (*func) (GtCommit *c, const char *names, gsize nlen, gpointer user),
             gpointer user)
{
  gsize pos = 0;
  while (data != NULL && pos < len)
    {
      const char *start = memchr (data + pos, '\x1e', len - pos);
      if (start == NULL)
        break;
      start++;
      gsize off = (gsize) (start - data);
      const char *end = off < len ? memchr (start, '\x1e', len - off) : NULL;
      gsize rlen = end != NULL ? (gsize) (end - start) : len - off;
      const char *names = NULL;
      gsize nlen = 0;
      GtCommit *c = parse_header (start, rlen, &names, &nlen);
      if (c != NULL)
        func (c, names, nlen, user);
      pos = off + rlen;
    }
}

typedef struct
{
  GHashTable *table; /* name → GtCommit* (a reference each) */
  const char *dir;
  gsize       dlen;
} LastCtx;

static void
last_func (GtCommit *c, const char *names, gsize nlen, gpointer user)
{
  LastCtx *ctx = user;
  gsize p = 0;
  while (p < nlen)
    {
      const char *s = names + p;
      const char *z = memchr (s, '\0', nlen - p);
      gsize l = z != NULL ? (gsize) (z - s) : nlen - p;
      p += l + 1;
      while (l > 0 && (*s == '\n' || *s == '\r'))
        s++, l--;
      if (l == 0)
        continue;
      /* Inside @dir? Then the item is the first component after it. */
      if (ctx->dlen > 0 && (l <= ctx->dlen || strncmp (s, ctx->dir, ctx->dlen) != 0 || s[ctx->dlen] != '/'))
        continue;
      const char *item = ctx->dlen > 0 ? s + ctx->dlen + 1 : s;
      gsize ilen = l - (gsize) (item - s);
      const char *slash = memchr (item, '/', ilen);
      g_autofree char *name = g_strndup (item, slash != NULL ? (gsize) (slash - item) : ilen);
      if (*name != '\0' && !g_hash_table_contains (ctx->table, name))
        g_hash_table_insert (ctx->table, g_steal_pointer (&name), gt_commit_ref (c));
    }
  gt_commit_unref (c);
}

GHashTable *
gt_log_last_commits (const char *data, gsize len, const char *dir)
{
  LastCtx ctx = {
    .table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) gt_commit_unref),
    .dir = dir != NULL ? dir : "",
  };
  while (ctx.dir[0] == '/')
    ctx.dir++;
  ctx.dlen = strlen (ctx.dir);
  while (ctx.dlen > 0 && ctx.dir[ctx.dlen - 1] == '/')
    ctx.dlen--;
  each_record (data, len, last_func, &ctx);
  return ctx.table;
}

static void
first_func (GtCommit *c, const char *names, gsize nlen, gpointer user)
{
  (void) names, (void) nlen;
  GtCommit **out = user;
  if (*out == NULL)
    *out = c;
  else
    gt_commit_unref (c);
}

GtCommit *
gt_log_first (const char *data, gsize len)
{
  GtCommit *c = NULL;
  each_record (data, len, first_func, &c);
  return c;
}

/* ---- errors, progress, URLs ---- */

GtErrorKind
gt_error_kind (const char *text)
{
  if (text == NULL)
    return GT_ERROR_OTHER;
  if (strstr (text, "could not read Username") != NULL || strstr (text, "could not read Password") != NULL ||
      strstr (text, "terminal prompts disabled") != NULL)
    return GT_ERROR_NEEDS_CREDENTIALS;
  if (strstr (text, "Authentication failed") != NULL || strstr (text, "The requested URL returned error: 401") != NULL ||
      strstr (text, "The requested URL returned error: 403") != NULL || strstr (text, "Invalid username or") != NULL)
    return GT_ERROR_AUTH_FAILED;
  if (strstr (text, "Permission denied (publickey") != NULL || strstr (text, "Host key verification failed") != NULL ||
      strstr (text, "ssh: Could not resolve hostname") != NULL || strstr (text, "ssh: connect to host") != NULL)
    return GT_ERROR_SSH;
  return GT_ERROR_OTHER;
}

gboolean
gt_progress_parse (const char *line, char **phase, int *percent)
{
  if (line == NULL)
    return FALSE;
  if (g_str_has_prefix (line, "remote: "))
    line += strlen ("remote: ");
  const char *colon = strchr (line, ':');
  if (colon == NULL || colon == line)
    return FALSE;
  const char *p = colon + 1;
  while (*p == ' ')
    p++;
  char *end = NULL;
  long v = strtol (p, &end, 10);
  if (end == p || *end != '%' || v < 0 || v > 100)
    return FALSE;
  if (phase != NULL)
    *phase = g_strndup (line, (gsize) (colon - line));
  if (percent != NULL)
    *percent = (int) v;
  return TRUE;
}

gboolean
gt_url_is_valid (const char *url)
{
  if (url == NULL)
    return FALSE;
  g_autofree char *u = g_strstrip (g_strdup (url));
  if (*u == '\0' || strpbrk (u, " \t\r\n") != NULL || u[0] == '-')
    return FALSE;
  static const char *const schemes[] = { "https://", "http://", "ssh://", "git://", "file://", NULL };
  for (int i = 0; schemes[i] != NULL; i++)
    if (g_str_has_prefix (u, schemes[i]))
      {
        const char *rest = u + strlen (schemes[i]);
        const char *slash = strchr (rest, '/');
        return g_str_equal (schemes[i], "file://") ? rest[0] == '/' && rest[1] != '\0'
                                                    : slash != NULL && slash > rest && slash[1] != '\0';
      }
  /* scp-like: [user@]host:path, host without '/' */
  const char *colon = strchr (u, ':');
  const char *slash = strchr (u, '/');
  return colon != NULL && colon > u && colon[1] != '\0' && (slash == NULL || slash > colon);
}

char *
gt_url_dir_name (const char *url)
{
  if (!gt_url_is_valid (url))
    return NULL;
  g_autofree char *u = g_strstrip (g_strdup (url));
  gsize n = strlen (u);
  while (n > 0 && u[n - 1] == '/')
    u[--n] = '\0';
  if (g_str_has_suffix (u, ".git"))
    u[n - 4] = '\0';
  n = strlen (u);
  while (n > 0 && u[n - 1] == '/')
    u[--n] = '\0';
  const char *slash = strrchr (u, '/');
  const char *colon = strrchr (u, ':');
  const char *start = slash != NULL && (colon == NULL || slash > colon) ? slash + 1 : colon != NULL ? colon + 1 : u;
  if (*start == '\0' || g_str_equal (start, ".") || g_str_equal (start, ".."))
    return NULL;
  return g_strdup (start);
}

char *
gt_relative_date (gint64 when, gint64 now)
{
  gint64 s = now - when;
  if (s < 60)
    return g_strdup ("just now");
  if (s < 3600)
    return s < 120 ? g_strdup ("1 minute ago") : g_strdup_printf ("%" G_GINT64_FORMAT " minutes ago", s / 60);
  if (s < 86400)
    return s < 7200 ? g_strdup ("1 hour ago") : g_strdup_printf ("%" G_GINT64_FORMAT " hours ago", s / 3600);
  if (s < 2 * 86400)
    return g_strdup ("yesterday");
  if (s < 30 * 86400)
    return g_strdup_printf ("%" G_GINT64_FORMAT " days ago", s / 86400);
  g_autoptr (GDateTime) then = g_date_time_new_from_unix_local (when);
  return then != NULL ? g_date_time_format (then, "%-d %B %Y") : g_strdup ("");
}

/* ---- history with a graph ---- */

void
gt_log_entry_free (GtLogEntry *e)
{
  if (e == NULL)
    return;
  g_free (e->hash);
  g_strfreev (e->parents);
  g_free (e->author);
  g_free (e->subject);
  g_clear_pointer (&e->refs, g_ptr_array_unref);
  g_free (e);
}

static void
ref_free (gpointer data)
{
  GtRef *r = data;
  g_free (r->name);
  g_free (r);
}

static void
add_ref (GPtrArray *refs, const char *name, GtRefKind kind)
{
  GtRef *r = g_new0 (GtRef, 1);
  r->name = g_strdup (name);
  r->kind = kind;
  g_ptr_array_add (refs, r);
}

/* "%D" with --decorate=full: "HEAD -> refs/heads/main, refs/remotes/origin/main, tag: refs/tags/v1". */
static GPtrArray *
parse_refs (const char *d, gsize len)
{
  GPtrArray *refs = g_ptr_array_new_with_free_func (ref_free);
  g_autofree char *text = g_strndup (d, len);
  g_auto (GStrv) parts = g_strsplit (text, ", ", -1);
  for (int i = 0; parts[i] != NULL; i++)
    {
      const char *p = g_strstrip (parts[i]);
      if (*p == '\0')
        continue;
      if (g_str_has_prefix (p, "HEAD -> "))
        {
          const char *b = p + strlen ("HEAD -> ");
          add_ref (refs, g_str_has_prefix (b, "refs/heads/") ? b + strlen ("refs/heads/") : b, GT_REF_CURRENT);
        }
      else if (g_str_equal (p, "HEAD"))
        add_ref (refs, "HEAD", GT_REF_HEAD);
      else if (g_str_has_prefix (p, "tag: "))
        {
          const char *t = p + strlen ("tag: ");
          add_ref (refs, g_str_has_prefix (t, "refs/tags/") ? t + strlen ("refs/tags/") : t, GT_REF_TAG);
        }
      else if (g_str_has_prefix (p, "refs/heads/"))
        add_ref (refs, p + strlen ("refs/heads/"), GT_REF_LOCAL);
      else if (g_str_has_prefix (p, "refs/remotes/"))
        {
          if (!g_str_has_suffix (p, "/HEAD"))
            add_ref (refs, p + strlen ("refs/remotes/"), GT_REF_REMOTE);
        }
    }
  return refs;
}

GPtrArray *
gt_history_parse (const char *data, gsize len)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) gt_log_entry_free);
  gsize pos = 0;
  while (data != NULL && pos < len)
    {
      const char *rec = data + pos;
      const char *end = memchr (rec, '\x1e', len - pos);
      gsize rlen = end != NULL ? (gsize) (end - rec) : len - pos;
      pos += rlen + 1;
      while (rlen > 0 && (*rec == '\n' || *rec == '\r'))
        rec++, rlen--;
      const char *f[6];
      gsize fl[6];
      gsize p = 0;
      int n = 0;
      for (; n < 6 && p <= rlen; n++)
        {
          const char *z = n < 5 ? memchr (rec + p, '\0', rlen - p) : NULL;
          f[n] = rec + p;
          fl[n] = z != NULL ? (gsize) (z - (rec + p)) : rlen - p;
          p += fl[n] + 1;
          if (z == NULL && n < 5)
            {
              n++;
              break;
            }
        }
      if (n < 6 || fl[0] == 0)
        continue;
      GtLogEntry *e = g_new0 (GtLogEntry, 1);
      e->hash = g_strndup (f[0], fl[0]);
      g_autofree char *parents = g_strndup (f[1], fl[1]);
      e->parents = *parents != '\0' ? g_strsplit (parents, " ", -1) : g_new0 (char *, 1);
      e->author = g_strndup (f[2], fl[2]);
      g_autofree char *date = g_strndup (f[3], fl[3]);
      e->date = g_ascii_strtoll (date, NULL, 10);
      e->refs = parse_refs (f[4], fl[4]);
      e->subject = g_strndup (f[5], fl[5]);
      g_ptr_array_add (out, e);
    }
  return out;
}

void
gt_graph_row_free (GtGraphRow *row)
{
  if (row == NULL)
    return;
  g_array_unref (row->segs);
  g_free (row);
}

static void
seg (GtGraphRow *row, int from, int to, gboolean top, int color)
{
  GtGraphSeg s = { from, to, top, color };
  g_array_append_val (row->segs, s);
}

/* A free lane (not @avoid), or a new one at the end. */
static int
free_lane (GPtrArray *lanes, int avoid)
{
  for (guint i = 0; i < lanes->len; i++)
    if (g_ptr_array_index (lanes, i) == NULL && (int) i != avoid)
      return (int) i;
  g_ptr_array_add (lanes, NULL);
  return (int) lanes->len - 1;
}

GPtrArray *
gt_graph_layout (GPtrArray *entries)
{
  GPtrArray *rows = g_ptr_array_new_with_free_func ((GDestroyNotify) gt_graph_row_free);
  g_autoptr (GPtrArray) lanes = g_ptr_array_new_with_free_func (g_free); /* the commit each lane waits for */
  g_autoptr (GArray) colors = g_array_new (FALSE, TRUE, sizeof (int));
  int next_color = 0;
  for (guint k = 0; entries != NULL && k < entries->len; k++)
    {
      GtLogEntry *e = g_ptr_array_index (entries, k);
      GtGraphRow *row = g_new0 (GtGraphRow, 1);
      row->segs = g_array_new (FALSE, FALSE, sizeof (GtGraphSeg));
      int node = -1;
      for (guint i = 0; i < lanes->len; i++)
        if (g_strcmp0 (g_ptr_array_index (lanes, i), e->hash) == 0)
          {
            node = (int) i;
            break;
          }
      gboolean expected = node >= 0;
      if (!expected)
        node = free_lane (lanes, -1);
      g_array_set_size (colors, MAX (colors->len, lanes->len));
      if (!expected)
        g_array_index (colors, int, node) = next_color++;
      /* Top half: every lane coming in; the ones waiting for this commit bend into its node. */
      int width = (int) lanes->len;
      for (guint i = 0; i < lanes->len; i++)
        {
          const char *want = g_ptr_array_index (lanes, i);
          if (want == NULL)
            continue;
          int c = g_array_index (colors, int, i);
          seg (row, (int) i, g_str_equal (want, e->hash) ? node : (int) i, TRUE, c);
          if (g_str_equal (want, e->hash) && (int) i != node)
            g_ptr_array_index (lanes, i) = (g_free (g_ptr_array_index (lanes, i)), NULL);
        }
      row->node = node;
      row->node_color = g_array_index (colors, int, node);
      int n_parents = (int) g_strv_length (e->parents);
      row->merge = n_parents > 1;
      g_free (g_ptr_array_index (lanes, node));
      g_ptr_array_index (lanes, node) = n_parents > 0 ? g_strdup (e->parents[0]) : NULL;
      /* Lanes made in this row for the other parents get no pass-through segment of their own. */
      g_autoptr (GArray) fresh = g_array_new (FALSE, TRUE, sizeof (gboolean));
      g_array_set_size (fresh, lanes->len + (guint) n_parents);
      for (int pi = 1; pi < n_parents; pi++)
        {
          int j = -1;
          for (guint i = 0; i < lanes->len; i++)
            if (g_strcmp0 (g_ptr_array_index (lanes, i), e->parents[pi]) == 0)
              {
                j = (int) i;
                break;
              }
          if (j < 0)
            {
              j = free_lane (lanes, node);
              g_free (g_ptr_array_index (lanes, j));
              g_ptr_array_index (lanes, j) = g_strdup (e->parents[pi]);
              g_array_set_size (colors, MAX (colors->len, lanes->len));
              g_array_index (colors, int, j) = next_color++;
              g_array_set_size (fresh, MAX (fresh->len, lanes->len));
              g_array_index (fresh, gboolean, j) = TRUE;
            }
          seg (row, node, j, FALSE, g_array_index (colors, int, j));
        }
      /* Bottom half: the node to its first parent, every other lane straight down. */
      for (guint i = 0; i < lanes->len; i++)
        {
          if (g_ptr_array_index (lanes, i) == NULL || g_array_index (fresh, gboolean, i))
            continue;
          seg (row, (int) i, (int) i, FALSE, g_array_index (colors, int, i));
        }
      /* Trailing free lanes are dropped (rows stay narrow). */
      while (lanes->len > 0 && g_ptr_array_index (lanes, lanes->len - 1) == NULL)
        g_ptr_array_set_size (lanes, lanes->len - 1);
      row->n_lanes = MAX (width, MAX ((int) lanes->len, node + 1));
      g_ptr_array_add (rows, row);
    }
  return rows;
}
