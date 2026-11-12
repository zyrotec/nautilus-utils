/* Compile-time constants for the folder-views feature (PLAN.md §10). */
#pragma once

/* GIO metadata attribute holding a folder's saved view (PLAN.md §10.1.2). */
#define FV_METADATA_ATTR "metadata::nautilus-utils-folder-view"

/* Format version written as the first field of the attribute value. */
#define FV_FORMAT_VERSION 1

/* In-memory cache of folders read or saved this session (PLAN.md §10.7, design consequence 1). Least
 * recently used entries are dropped beyond this. ~100 bytes per entry. */
#define FV_CACHE_MAX 4096
