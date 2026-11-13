/* Compile-time constants (PLAN.md §6.3). */
#pragma once

#define PREVIEW_SLOTS     4    /* Thumbnails shown at once (2×2) */
#define MAX_PREVIEW_FILES 16   /* Max files collected per folder → up to 4 hover frames */
#define HOVER_FRAME_MS    700  /* Time each frame is shown while hovering */
#define WORKER_THREADS    2    /* Max concurrent scan/thumbnail jobs */
#define ENUM_BATCH        200  /* Entries enumerated between cancellation checks (the scan runs in a
                                * worker with sync next_file(); see PLAN.md D7) */
#define LRU_FOLDERS       256  /* Folders kept in memory */

/* Candidates a scan returns: more than MAX_PREVIEW_FILES so that files whose thumbnail turns out to be
 * impossible (e.g. audio without cover art) can be skipped without rescanning (PLAN.md D8). */
#define SCAN_WINDOW       (2 * MAX_PREVIEW_FILES)

/* In-memory frame cache budget, on top of LRU_FOLDERS: at high zoom on HiDPI a single folder's frames can
 * take ~2 MB, so a folder count alone could reach hundreds of MB (PLAN.md §6.5 Phase 6 target: < 100 MB). */
#define CACHE_MAX_BYTES   (48u * 1024u * 1024u)

/* Where the preview frame sits on the folder icon (fractions of the icon size). Measured on the Adwaita and
 * Hatter-Orange folder icons: a 0.76 × 0.52 rectangle centred at (50 %, 61.5 %) stays inside the front face
 * of both (PLAN.md §6.1.2). */
#define FRAME_FACE_W        0.76
#define FRAME_FACE_H        0.52
#define FRAME_FACE_CENTER_X 0.50
#define FRAME_FACE_CENTER_Y 0.615

/* Dolphin-style photos: maximum tilt either way, in degrees (owner request, Phase 5 follow-up). */
#define FRAME_MAX_TILT_DEG  8.0

/* Transparent margin around the face area inside the frame texture, as a fraction of the face size on each
 * side, so large tilted photos can poke slightly past the face (like Dolphin) instead of being clipped. */
#define FRAME_PAD           0.10

/* Stacked layout (PLAN.md §11.1), measured on the owner's mock-up (Asset-2, 434 × 294 units). All values
 * are fractions of the stack width. Card i (0 = front) is inset by i·STACK_STEP on both sides and raised by
 * i·STACK_STEP; every card is STACK_CARD_H tall, so the front card ends at STACK_H. */
#define STACK_CARDS         3
#define STACK_STEP          0.0405
#define STACK_CARD_H        0.5975
#define STACK_H             (STACK_CARD_H + (STACK_CARDS - 1) * STACK_STEP)
#define STACK_PAD           0.06   /* transparent margin around the stack in the frame texture */
#define STACK_MAX_TILT_DEG  4.0
#define STACK_DIM           0.10   /* each card further back is darkened by this much more */
#define STACK_DEPTH_DIM     0.16   /* depth "blur": darkening per card further back */
#define STACK_DEPTH_BLUR    0.006  /* depth "blur": blur radius per card further back (fraction of card width) */
#define STACK_DEPTH_ALPHA   0.30   /* depth "transparency": opacity lost per card further back */
#define STACK_DEPTH_DARKEN  0.22   /* depth "darken": darkening per card further back */

/* Sizes the owner gives "at the 64 × 64 icon size" scale with the icon (PLAN.md §11.5b). */
#define REF_ICON_PX         64.0
#define STACK_SIDE          (8.0 / REF_ICON_PX) /* stack 8 px from the icon's left and right edges */
#define STACK_ICON_W        (1.0 - 2 * STACK_SIDE) /* stack width as a fraction of the icon */
#define STACK_EDGE          (8.0 / REF_ICON_PX) /* top / bottom position: 8 px from the icon's edge */
/* Centre position: the stack's top is this far below the top of the folder's visible body (its opaque
 * bounding box), as a fraction of the body height (the owner's mock-up). */
#define STACK_BODY_TOP      0.204

/* Disk cache layout version: bump when the look of frames changes, so old frames are never shown.
 * v4: the style (layout, border, shadow, rotation) is part of each folder's cache directory name.
 * v5: drop shadows following the picture's shape, keep-aspect and depth options.
 * v6: stack 8 px from the sides, depth presets, corner radius, fade and view clipping. */
#define CACHE_DISK_VERSION  "v6"
