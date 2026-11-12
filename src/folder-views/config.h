/* Compile-time constants for the folder-views feature (PLAN.md §10). */
#pragma once

/* GIO metadata attribute holding a folder's saved view (PLAN.md §10.1.2). */
#define FV_METADATA_ATTR "metadata::nautilus-utils-folder-view"

/* Format version written as the first field of the attribute value. */
#define FV_FORMAT_VERSION 1

/* In-memory cache of folders read or saved this session (PLAN.md §10.7, design consequence 1). Least
 * recently used entries are dropped beyond this. ~100 bytes per entry. */
#define FV_CACHE_MAX 4096

/* Files writes its global view/zoom keys on every switch; we put them back to our default this long after
 * the last change, so a burst of navigation costs one write (PLAN.md §10.1.1, D1). */
#define FV_RESTORE_KEYS_DELAY_MS 2000

/* Upper bound on zoom steps per apply (the grid view has 5 levels). */
#define FV_MAX_ZOOM_STEPS 6

/* Nautilus' view types must be registered this long after a tab first reports a location (D3), otherwise
 * the feature assumes Nautilus changed and switches itself off. */
#define FV_TYPES_TIMEOUT_MS 5000

/* Smooth folder changes (PLAN.md §10.9): when a folder needs another view type or icon size, a frozen picture
 * of the old view covers the switch and fades out once the new folder's files are in. */
#define FV_FADE_MS      180  /* fade-out of the cover (instant when GTK animations are off) */
#define FV_COVER_MAX_MS 1000 /* fade anyway this long after the switch if the folder is still loading */
