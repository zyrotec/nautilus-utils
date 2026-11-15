/* Details pane constants (PLAN.md §13.1, §13.3). */
#pragma once

#define DP_DEBOUNCE_MS          80        /* selection changes applied after this much quiet (§13.3 rule 2) */
#define DP_DISCOVER_TIMEOUT_S   3         /* GstDiscoverer limit per file (§13.3 rule 3) */
#define DP_PARSE_MAX_BYTES      (200 * G_GUINT64_CONSTANT (1024 * 1024)) /* EXIF / PDF only below this size */
#define DP_COUNT_MAX_ENTRIES    100000    /* folder total: stop after this many entries (§13.3 rule 4) */
#define DP_COUNT_MAX_US         (5 * G_USEC_PER_SEC) /* … or after this long */
#define DP_COUNT_CACHE_MAX      256       /* folder totals remembered per session (URI + mtime) */
#define DP_MULTI_MAX            500       /* multiple selection: files read for the summary */
#define DP_PREVIEW_MAX_HEIGHT   260       /* px, logical */
#define DP_WIDTH_DEFAULT        320       /* px; the setting's default */
