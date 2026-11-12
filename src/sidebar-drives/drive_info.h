/* Drive information for the sidebar-drives feature (PLAN.md §8, Phase 1): OS name, local/network
 * classification, async size queries with a timeout, and the text/bar state shown under each drive.
 * No Nautilus or GTK types (PLAN.md §4.2), only GLib/GIO, so it is tested standalone. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum
{
  SD_DRIVE_LOCAL,
  SD_DRIVE_NETWORK,
} SdDriveKind;

typedef enum
{
  SD_FILL_UNKNOWN,   /* Size not reported: show the name only */
  SD_FILL_NORMAL,    /* Accent colour */
  SD_FILL_WARNING,   /* >= SD_WARN_FRACTION full */
  SD_FILL_FULL,      /* >= SD_FULL_FRACTION full */
} SdFillState;

/* Result of a size query. */
typedef struct
{
  gboolean known;    /* FALSE when the filesystem doesn't report size/free (some network servers) */
  guint64  size;     /* Bytes; valid when known */
  guint64  free;     /* Bytes; valid when known, never > size */
  gboolean remote;   /* filesystem::remote (NFS, CIFS, gvfs network backends, …) */
  char    *fs_type;  /* Owned; filesystem::type or NULL */
} SdUsage;

/* Frees @usage and its fields; NULL-safe. */
void sd_usage_free (SdUsage *usage);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SdUsage, sd_usage_free)

/* The OS's pretty name ("Fedora Linux 45 (Workstation Edition)"), or SD_OS_FALLBACK_NAME.
 * Caller owns the returned string (g_free). */
char *sd_os_name (void);

/* Network if the location isn't native (smb://, sftp://, …) or the filesystem reports itself as remote. */
SdDriveKind sd_classify (gboolean is_native,
                         gboolean is_remote);

/* Used fraction in [0, 1], or a negative value when unknown (size 0). */
double sd_fill_fraction (guint64 size,
                         guint64 free);

/* Bar state for a fill fraction from sd_fill_fraction(). */
SdFillState sd_fill_state (double fraction);

/* "445.7 GB free of 497.3 GB" (g_format_size, no-break space before the unit).
 * Caller owns the returned string (g_free). */
char *sd_usage_text (guint64 size,
                     guint64 free);

/* Queries size/free/type/remote of the filesystem holding @root without blocking the calling thread.
 * @timeout_ms 0 = no timeout; otherwise the callback gets G_IO_ERROR_TIMED_OUT after that long even if the
 * underlying query never returns (it is cancelled and its late result dropped). @cancellable cancels with
 * G_IO_ERROR_CANCELLED. The callback runs in the calling thread's thread-default main context. */
void sd_query_usage_async (GFile               *root,
                           guint                timeout_ms,
                           GCancellable        *cancellable,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data);

/* Caller owns the returned SdUsage (sd_usage_free); NULL with @error set on failure. */
SdUsage *sd_query_usage_finish (GFile         *root,
                                GAsyncResult  *result,
                                GError       **error);

G_END_DECLS
