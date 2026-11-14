/* Details pane: reading what the pane shows (PLAN.md §13.1.2, §13.2, §13.3).
 *
 * Every *_read / count function is synchronous and does blocking I/O: worker threads (or tests) only. The
 * *_async wrappers run them in GLib's thread pool (GTask) with a GCancellable and call back on the calling
 * thread's main context. No Nautilus types (PLAN.md §4.2). */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum
{
  DP_KIND_OTHER = 0,
  DP_KIND_FOLDER,
  DP_KIND_IMAGE,
  DP_KIND_VIDEO,
  DP_KIND_AUDIO,
  DP_KIND_PDF,
  DP_KIND_DOCUMENT, /* text, office documents (no extra properties; counted as documents) */
} DpKind;

/* One "label  value" line. */
typedef struct
{
  char *label;   /* Owned. */
  char *value;   /* Owned, never NULL. */
  char *tooltip; /* Owned or NULL. */
} DpRow;

/* A titled group of rows ("Details", "Image", "Video", "Audio", "Document"). */
typedef struct
{
  char      *title; /* Owned. */
  GPtrArray *rows;  /* Owned; DpRow* (owned). */
} DpGroup;

/* New empty group. Caller owns it (dp_group_free). */
DpGroup *dp_group_new (const char *title);
void     dp_group_free (DpGroup *group);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (DpGroup, dp_group_free)

/* Adds a row, taking ownership of @value and @tooltip. A NULL or empty @value adds nothing (and frees
 * @tooltip), so optional values can be passed straight through. */
void dp_group_take (DpGroup    *group,
                    const char *label,
                    char       *value,
                    char       *tooltip);

/* The row labelled @label, or NULL. Borrowed. */
const DpRow *dp_group_find (const DpGroup *group,
                            const char    *label);

typedef struct
{
  gboolean has_mode;      /* Unix mode known (local files, most network shares) */
  guint32  mode;
  char    *owner;         /* Owned or NULL: owner's user name */
  char    *group;         /* Owned or NULL */
  gboolean has_access;    /* access::can-* known */
  gboolean can_read, can_write, can_execute;
} DpPerms;

/* General information about one file or folder (PLAN.md §13.1.2 "All"). Immutable once read, so it can be
 * shared with worker threads: atomically reference counted. */
typedef struct
{
  GFile     *file;            /* Owned. */
  char      *uri;             /* Owned. */
  char      *display_name;    /* Owned, never NULL. */
  char      *content_type;    /* Owned or NULL. */
  char      *description;     /* Owned or NULL: "JPEG image", "Folder". */
  DpKind     kind;
  gboolean   is_dir;
  gboolean   is_symlink;
  gboolean   is_native;       /* A local path (not network/FUSE-backed GIO). */
  char      *symlink_target;  /* Owned or NULL. */
  GIcon     *icon;            /* Owned or NULL: the custom icon if set, else GIO's icon. */
  gboolean   custom_icon;     /* @icon is the user's custom icon (metadata::custom-icon / -name). */
  guint64    size;            /* standard::size (files) */
  GDateTime *modified;        /* Owned or NULL. */
  GDateTime *created;         /* Owned or NULL. */
  GDateTime *accessed;        /* Owned or NULL. */
  guint64    mtime_s;         /* For thumbnail and cache keys */
  char      *location;        /* Owned or NULL: the parent folder (a path when native, else a URI). */
  DpPerms    perms;
} DpInfo;

/* Reads @file's general information. Caller owns the result (dp_info_unref). */
DpInfo *dp_info_read  (GFile         *file,
                       GCancellable  *cancellable,
                       GError       **error);
DpInfo *dp_info_ref   (DpInfo *info);
void    dp_info_unref (DpInfo *info);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (DpInfo, dp_info_unref)

/* What a content type is, for the extra readers and the multi-selection counts. */
DpKind dp_kind_for_content_type (const char *content_type,
                                 gboolean    is_dir);

/* Extra properties by kind (PLAN.md §13.1.2): image (gexiv2, else gdk-pixbuf), video / audio
 * (GstDiscoverer), PDF (poppler). Returns DpGroup* (owned by the array); empty when the kind has none, the
 * file is too large, not native (§13.3 rule 3) or unreadable. Errors: only G_IO_ERROR_CANCELLED. */
GPtrArray *dp_extra_read (DpInfo        *info,
                          GCancellable  *cancellable,
                          GError       **error);

/* Folder contents (PLAN.md §13.3 rule 4). */
typedef struct
{
  guint    items;           /* direct children, hidden ones included */
  guint    hidden;          /* direct children whose name starts with "." (or standard::is-hidden) */
  gboolean items_truncated; /* stopped counting children at the entry limit */
  guint64  total;           /* bytes of all files below, no symlinks followed, no mount points crossed */
  guint64  entries;         /* entries visited for @total */
  gboolean total_truncated; /* stopped at the entry or time limit: @total is a lower bound */
} DpFolderCount;

/* Counts @dir. @max_entries / @max_us bound the walk (0 = config.h defaults). FALSE with @error if @dir
 * can't be listed or on cancellation. */
gboolean dp_folder_count (GFile          *dir,
                          guint           max_entries,
                          gint64          max_us,
                          GCancellable   *cancellable,
                          DpFolderCount  *out,
                          GError        **error);

/* The "Details" group for @info: Type, Contents and Total size (folders: from @count, NULL = "Calculating…"),
 * Size (files), Location, Modified, Created, Accessed, Link target. Times relative to @now. */
DpGroup *dp_details_group (const DpInfo        *info,
                           const DpFolderCount *count,
                           GDateTime           *now);

/* Several selected items (PLAN.md §13.1.2 item 4). */
typedef struct
{
  guint      n_selected;   /* how many were selected */
  guint      n_read;       /* how many were read (at most DP_MULTI_MAX) */
  GPtrArray *infos;        /* Owned; DpInfo* (owned), the first ones read, in selection order */
  DpGroup   *group;        /* Owned: "Selection" rows: total size (files), kinds, location */
} DpSummary;

void dp_summary_free (DpSummary *summary);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (DpSummary, dp_summary_free)

/* Reads up to DP_MULTI_MAX of @files (GFile*, borrowed) of @n_selected selected. Unreadable files are
 * skipped. Errors: only G_IO_ERROR_CANCELLED. */
DpSummary *dp_summary_read (GPtrArray     *files,
                            guint          n_selected,
                            GCancellable  *cancellable,
                            GError       **error);

/* Async wrappers (main thread). Each takes its own refs on its arguments. */
void    dp_info_read_async  (GFile *file, GCancellable *cancellable, GAsyncReadyCallback cb, gpointer data);
DpInfo *dp_info_read_finish (GAsyncResult *result, GError **error);

void       dp_extra_read_async  (DpInfo *info, GCancellable *cancellable, GAsyncReadyCallback cb, gpointer data);
GPtrArray *dp_extra_read_finish (GAsyncResult *result, GError **error);

void     dp_folder_count_async  (GFile *dir, GCancellable *cancellable, GAsyncReadyCallback cb, gpointer data);
gboolean dp_folder_count_finish (GAsyncResult *result, DpFolderCount *out, GError **error);

/* @files: GFile*; the array is copied (refs taken). */
void       dp_summary_read_async  (GPtrArray *files, guint n_selected, GCancellable *cancellable,
                                   GAsyncReadyCallback cb, gpointer data);
DpSummary *dp_summary_read_finish (GAsyncResult *result, GError **error);

G_END_DECLS
