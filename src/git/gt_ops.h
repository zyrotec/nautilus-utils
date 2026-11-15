/* Git: operations started from the status bar (push, pull, sync, checkout, create branch; clone in Phase 5) and
 * their progress, for the progress popover (PLAN.md §14.1 G4). One list for the whole process, newest first;
 * finished ones are kept (the last GT_OPS_KEEP). No GTK. Main thread. */
#pragma once

#include <gio/gio.h>

#include "gt_run.h"

G_BEGIN_DECLS

#define GT_OPS_KEEP 20

typedef enum
{
  GT_OP_RUNNING,
  GT_OP_DONE,
  GT_OP_FAILED,
} GtOpState;

#define GT_TYPE_OP (gt_op_get_type ())
G_DECLARE_FINAL_TYPE (GtOp, gt_op, GT, OP, GObject)

/* Properties (all notify): "title" (string), "root" (string), "state" (int GtOpState), "phase" (string, the git
 * progress phase while running), "percent" (int, -1 unknown), "message" (string: the result, or git's error),
 * "kind" (int GtErrorKind when failed). */
const char *gt_op_get_title   (GtOp *self);
const char *gt_op_get_root    (GtOp *self);
GtOpState   gt_op_get_state   (GtOp *self);
const char *gt_op_get_phase   (GtOp *self);
int         gt_op_get_percent (GtOp *self);
const char *gt_op_get_message (GtOp *self);
GtErrorKind gt_op_get_kind    (GtOp *self);

/* Called once the operation has finished (any state). */
typedef void (*GtOpDone) (GtOp *op, gpointer user_data);

/* Starts `git <args…>` in @root as an operation titled @title ("Push", "Switch to main"…); @success is the message
 * on success. Afterwards the repository's GtInfo (if any) reads again. Returns the operation (borrowed: the list
 * keeps it). */
GtOp *gt_ops_start (const char        *root,
                    const char        *title,
                    const char *const *args,
                    GtRunFlags         flags,
                    const char        *success,
                    GtOpDone           done,
                    gpointer           user_data);

/* The list (GtOp, newest first). Borrowed, process-wide. */
GListModel *gt_ops_get_model (void);

/* Operations running in @root (NULL = anywhere). */
guint gt_ops_count_running (const char *root);

/* Drops the list (feature off / shutdown). Running operations are left to finish: stopping a push or a checkout
 * halfway could leave the repository half done. */
void gt_ops_shutdown (void);

G_END_DECLS
