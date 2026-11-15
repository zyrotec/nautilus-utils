/* Git operations. See gt_ops.h. */
#include "gt_ops.h"

#include "gt_info.h"

#define LOCAL_TIMEOUT_MS   (2 * 60 * 1000)
#define NETWORK_TIMEOUT_MS (15 * 60 * 1000)

struct _GtOp
{
  GObject       parent_instance;
  char         *title;
  char         *root;
  char         *success;
  GtOpState     state;
  char         *phase;
  int           percent;
  char         *message;
  GtErrorKind   kind;
  GCancellable *cancel;
  GtOpDone      done;
  gpointer      done_data;
};

enum { PROP_0, PROP_TITLE, PROP_ROOT, PROP_STATE, PROP_PHASE, PROP_PERCENT, PROP_MESSAGE, PROP_KIND, N_PROPS };
static GParamSpec *props[N_PROPS];
static GListStore *ops; /* GtOp, newest first */

G_DEFINE_FINAL_TYPE (GtOp, gt_op, G_TYPE_OBJECT)

static void
gt_op_get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  GtOp *self = GT_OP (object);
  switch (id)
    {
    case PROP_TITLE: g_value_set_string (value, self->title); break;
    case PROP_ROOT: g_value_set_string (value, self->root); break;
    case PROP_STATE: g_value_set_int (value, self->state); break;
    case PROP_PHASE: g_value_set_string (value, self->phase); break;
    case PROP_PERCENT: g_value_set_int (value, self->percent); break;
    case PROP_MESSAGE: g_value_set_string (value, self->message); break;
    case PROP_KIND: g_value_set_int (value, self->kind); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
    }
}

static void
gt_op_finalize (GObject *object)
{
  GtOp *self = GT_OP (object);
  g_clear_object (&self->cancel);
  g_free (self->title);
  g_free (self->root);
  g_free (self->success);
  g_free (self->phase);
  g_free (self->message);
  G_OBJECT_CLASS (gt_op_parent_class)->finalize (object);
}

static void
gt_op_class_init (GtOpClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS (klass);
  oc->get_property = gt_op_get_property;
  oc->finalize = gt_op_finalize;
  GParamFlags f = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY;
  props[PROP_TITLE] = g_param_spec_string ("title", NULL, NULL, NULL, f);
  props[PROP_ROOT] = g_param_spec_string ("root", NULL, NULL, NULL, f);
  props[PROP_STATE] = g_param_spec_int ("state", NULL, NULL, GT_OP_RUNNING, GT_OP_FAILED, GT_OP_RUNNING, f);
  props[PROP_PHASE] = g_param_spec_string ("phase", NULL, NULL, NULL, f);
  props[PROP_PERCENT] = g_param_spec_int ("percent", NULL, NULL, -1, 100, -1, f);
  props[PROP_MESSAGE] = g_param_spec_string ("message", NULL, NULL, NULL, f);
  props[PROP_KIND] = g_param_spec_int ("kind", NULL, NULL, 0, G_MAXINT, 0, f);
  g_object_class_install_properties (oc, N_PROPS, props);
}

static void
gt_op_init (GtOp *self)
{
  self->percent = -1;
}

const char *gt_op_get_title (GtOp *self) { g_return_val_if_fail (GT_IS_OP (self), NULL); return self->title; }
const char *gt_op_get_root (GtOp *self) { g_return_val_if_fail (GT_IS_OP (self), NULL); return self->root; }
GtOpState gt_op_get_state (GtOp *self) { g_return_val_if_fail (GT_IS_OP (self), GT_OP_FAILED); return self->state; }
const char *gt_op_get_phase (GtOp *self) { g_return_val_if_fail (GT_IS_OP (self), NULL); return self->phase; }
int gt_op_get_percent (GtOp *self) { g_return_val_if_fail (GT_IS_OP (self), -1); return self->percent; }
const char *gt_op_get_message (GtOp *self) { g_return_val_if_fail (GT_IS_OP (self), NULL); return self->message; }
GtErrorKind gt_op_get_kind (GtOp *self) { g_return_val_if_fail (GT_IS_OP (self), GT_ERROR_OTHER); return self->kind; }

static void
on_progress (const char *phase, int percent, const char *line, gpointer data)
{
  (void) line;
  GtOp *self = data;
  if (g_strcmp0 (self->phase, phase) != 0)
    {
      g_free (self->phase);
      self->phase = g_strdup (phase);
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PHASE]);
    }
  if (self->percent != percent)
    {
      self->percent = percent;
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PERCENT]);
    }
}

/* A failure message: git's own text, with what to do for the cases we recognise. */
static char *
failure_message (const GtRunResult *r, const GError *error)
{
  const char *text = r != NULL && r->error_text[0] != '\0' ? r->error_text
                     : error != NULL ? error->message : "git failed without a message";
  const char *hint = NULL;
  switch (r != NULL ? r->kind : GT_ERROR_OTHER)
    {
    case GT_ERROR_NEEDS_CREDENTIALS:
      hint = "The remote needs a username and a token (password).";
      break;
    case GT_ERROR_AUTH_FAILED:
      hint = "The remote refused the username or token.";
      break;
    case GT_ERROR_SSH:
      hint = "SSH could not sign in: add your key to the SSH agent (ssh-add) and to your account on the server, "
             "or check the host name.";
      break;
    case GT_ERROR_OTHER:
    default:
      break;
    }
  return hint != NULL ? g_strdup_printf ("%s\n\n%s", hint, text) : g_strdup (text);
}

static void
on_finished (GObject *src, GAsyncResult *res, gpointer data)
{
  (void) src;
  GtOp *self = data; /* our reference */
  g_autoptr (GError) error = NULL;
  g_autoptr (GtRunResult) r = gt_run_finish (res, &error);
  g_free (self->message);
  if (r != NULL && r->exit_status == 0)
    {
      self->state = GT_OP_DONE;
      self->message = g_strdup (self->success);
    }
  else
    {
      self->state = GT_OP_FAILED;
      self->kind = r != NULL ? r->kind : GT_ERROR_OTHER;
      self->message = g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ? g_strdup ("Stopped")
                                                                                : failure_message (r, error);
    }
  g_clear_pointer (&self->phase, g_free);
  self->percent = -1;
  g_object_freeze_notify (G_OBJECT (self));
  for (int p = PROP_STATE; p <= PROP_KIND; p++)
    g_object_notify_by_pspec (G_OBJECT (self), props[p]);
  g_object_thaw_notify (G_OBJECT (self));
  g_autoptr (GtInfo) info = gt_info_lookup (self->root);
  if (info != NULL)
    gt_info_refresh (info); /* the git folder monitor would too; this is immediate */
  if (self->done != NULL)
    self->done (self, self->done_data);
  g_object_unref (self);
}

/* Drops the oldest finished operations beyond GT_OPS_KEEP. */
static void
trim (void)
{
  guint n = g_list_model_get_n_items (G_LIST_MODEL (ops));
  for (guint i = n; i-- > GT_OPS_KEEP;)
    {
      g_autoptr (GtOp) op = g_list_model_get_item (G_LIST_MODEL (ops), i);
      if (op->state != GT_OP_RUNNING)
        g_list_store_remove (ops, i);
    }
}

GtOp *
gt_ops_start (const char *root, const char *title, const char *const *args, GtRunFlags flags, const char *success,
              GtOpDone done, gpointer user_data)
{
  g_return_val_if_fail (root != NULL && title != NULL && args != NULL, NULL);
  GListModel *model = gt_ops_get_model ();
  (void) model;
  GtOp *self = g_object_new (GT_TYPE_OP, NULL);
  self->title = g_strdup (title);
  self->root = g_strdup (root);
  self->success = g_strdup (success != NULL ? success : "Done");
  self->cancel = g_cancellable_new ();
  self->done = done;
  self->done_data = user_data;
  g_list_store_insert (ops, 0, self);
  trim ();
  gt_run_async (root, args, flags, NULL, (flags & GT_RUN_NETWORK) ? NETWORK_TIMEOUT_MS : LOCAL_TIMEOUT_MS,
                self->cancel, on_progress, self, on_finished, g_object_ref (self));
  g_object_unref (self); /* the list keeps it */
  return self;
}

GListModel *
gt_ops_get_model (void)
{
  if (ops == NULL)
    ops = g_list_store_new (GT_TYPE_OP);
  return G_LIST_MODEL (ops);
}

guint
gt_ops_count_running (const char *root)
{
  guint n = 0;
  for (guint i = 0; ops != NULL && i < g_list_model_get_n_items (G_LIST_MODEL (ops)); i++)
    {
      g_autoptr (GtOp) op = g_list_model_get_item (G_LIST_MODEL (ops), i);
      n += op->state == GT_OP_RUNNING && (root == NULL || g_strcmp0 (op->root, root) == 0);
    }
  return n;
}

void
gt_ops_shutdown (void)
{
  for (guint i = 0; ops != NULL && i < g_list_model_get_n_items (G_LIST_MODEL (ops)); i++)
    {
      g_autoptr (GtOp) op = g_list_model_get_item (G_LIST_MODEL (ops), i);
      op->done = NULL; /* nobody to tell any more; a running one finishes (never kill a push halfway) */
    }
  g_clear_object (&ops);
}
