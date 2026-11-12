/* Compile-time constants for the sidebar-drives feature (PLAN.md §8.3). */
#pragma once

#define SD_REFRESH_S        60     /* Periodic free-space refresh while a window is visible */
#define SD_NET_TIMEOUT_MS   3000   /* Give up on a network size query after this */
#define SD_WARN_FRACTION    0.90   /* Bar turns warning colour at this fill level */
#define SD_FULL_FRACTION    0.95   /* Bar turns error colour at this fill level */
#define SD_BAR_HEIGHT_PX    4      /* Usage bar height */
#define SD_EJECT_SLIDE_MS   200    /* Sliding eject button animation (PLAN.md §9.1.4) */

/* Label of the OS drive row when the OS has no PRETTY_NAME (§8.1.1). */
#define SD_OS_FALLBACK_NAME "Computer"
