/* Compile-time constants for the folder-views feature (PLAN.md §10). */
#pragma once

/* GIO metadata attribute holding a folder's saved view (PLAN.md §10.1.2). */
#define FV_METADATA_ATTR "metadata::nautilus-utils-folder-view"

/* Format version written as the first field of the attribute value. */
#define FV_FORMAT_VERSION 1

/* In-memory cache of folders read or saved this session (PLAN.md §10.7, design consequence 1). Least
 * recently used entries are dropped beyond this. ~100 bytes per entry. */
#define FV_CACHE_MAX 8192

/* Files writes its global view/zoom keys on every switch; we put them back to our default this long after
 * the last change, so a burst of navigation costs one write (PLAN.md §10.1.1, D1). */
#define FV_RESTORE_KEYS_DELAY_MS 2000

/* Upper bound on zoom steps per apply (the grid view has 5 levels). */
#define FV_MAX_ZOOM_STEPS 6

/* Nautilus' view types must be registered this long after a tab first reports a location (D3), otherwise
 * the feature assumes Nautilus changed and switches itself off. */
#define FV_TYPES_TIMEOUT_MS 5000

/* Folder changes (PLAN.md §10.9, §10.10): when a folder needs another view type or icon size, a frozen picture
 * of the old view covers the switch, which runs right after the click has been drawn, and is taken away the
 * moment the new folder's files are in. */
#define FV_FADE_MS      0    /* fade-out of the cover; 0 = instant cut, like stock Files (owner: "feel instant") */
#define FV_COVER_MAX_MS 1000 /* reveal anyway this long after the click if the folder is still loading */
#define FV_SWITCH_FALLBACK_MS 100 /* switch even if no frame was drawn by then (e.g. window not shown) */

/* Read-ahead (§10.10): after a local folder has loaded, the saved views of its subfolders and siblings (up to this many
 * entries enumerated per folder) and of its parent are read in the background, so the next folder's view is known at
 * the click. */
#define FV_PREFETCH_MAX   2000
#define FV_PREFETCH_BATCH 64
