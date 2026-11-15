/* DpPane: the details pane widget (PLAN.md §13.1.2, §13.1.3).
 *
 * Plain GTK + libadwaita, no Nautilus types (PLAN.md §13.3 rule 5): the hook (or the demo) tells it what to
 * show with dp_pane_show(); it reads everything in workers (dp_meta, dp_thumb), cancelling the previous reads
 * whenever it is told to show something else. */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define DP_TYPE_PANE (dp_pane_get_type ())
G_DECLARE_FINAL_TYPE (DpPane, dp_pane, DP, PANE, GtkWidget)

/* A floating new pane. */
GtkWidget *dp_pane_new (void);

/* Shows @files (GFile*, borrowed; the pane takes its own refs) of @n_selected selected items (may be more
 * than files->len for huge selections). No files: shows @folder (may be NULL = nothing). */
void dp_pane_show (DpPane    *self,
                   GFile     *folder,
                   GPtrArray *files,
                   guint      n_selected);

/* Stops the video preview, if playing, and shows the thumbnail again. */
void dp_pane_stop_media (DpPane *self);

/* Look options shared by every pane (PLAN.md §13.10). */
typedef struct
{
  gboolean border;          /* line toward the folder view (all round when floating) */
  int      margin;          /* px around the pane; > 0 = a floating card */
  int      padding;         /* px inside */
  int      radius;          /* px corner radius of the pane */
  int      preview_radius;  /* px corner radius of the preview box */
  int      preview_margin;  /* px around the preview box */
  int      stack_depth;     /* FpDepth (= NuDepthEffect) */
  gboolean stack_shadow;
  gboolean stack_rotation;
  gboolean stack_keep_aspect;
  int      stack_fade;      /* % */
  gboolean stack_card;      /* translucent card behind icon items */
  gboolean stack_border;    /* media cards: border (P25: the folder previews' options, own copies) */
  GdkRGBA  stack_border_color;
  double   stack_border_width; /* px */
  int      stack_corner_radius; /* px, media cards */
  gboolean stack_media_square; /* media as squares the size of the icons (P26) */
  gboolean backdrop;        /* a picture's blurred copy fills the box behind it (P28) */
  int      icon_size;       /* px: icons in the preview box and the stack's front icon (P16) */
  gboolean media_box;       /* media flush inside a fixed box instead of a box of their shape (P20) */
  gboolean title_centered;  /* P21 */
  gboolean show_icon;       /* the small icon next to the name (P21) */
} DpPaneStyle;

/* Restyles every pane (and re-renders shown stacks if their options changed). Main thread. */
void dp_pane_set_style (const DpPaneStyle *style);

/* VERTICAL = right of the view (one column); HORIZONTAL = the Windows 7 strip at the bottom (P14). */
void dp_pane_set_layout (DpPane         *self,
                         GtkOrientation  layout);

/* Height the pane asks for at the bottom; the minimum is DP_PANE_MIN_HEIGHT. */
#define DP_PANE_MIN_HEIGHT 72
void dp_pane_set_preferred_height (DpPane *self,
                                   int     height);

/* Width the pane asks for (its natural width); the minimum is DP_PANE_MIN_WIDTH. */
#define DP_PANE_MIN_WIDTH 260
void dp_pane_set_preferred_width (DpPane *self,
                                  int     width);

/* Draws @folder's preview with @icon (the folder's themed icon) at @size × @size logical px and @scale, or
 * returns NULL to use the plain icon. Returns a new reference. Main thread. */
typedef GdkPaintable *(*DpFolderRenderer) (GFile        *folder,
                                           GdkPaintable *icon,
                                           int           size,
                                           int           scale);

/* Process-wide; NULL = folders always show their icon. */
void dp_pane_set_folder_renderer (DpFolderRenderer renderer);

/* The CSS the pane needs, added to every display once (removed by dp_pane_unload_style). */
void dp_pane_unload_style (void);

/* For the debug driver and tests ---------------------------------------------------------------- */

/* Clicks the play button. FALSE if there is none (not a video, or already playing). */
gboolean dp_pane_play (DpPane *self);

/* TRUE while the video preview is playing. */
gboolean dp_pane_is_playing (DpPane *self);

/* Reads still running (info, extra, count, thumbnail). */
guint dp_pane_get_n_pending (DpPane *self);

/* Longest main-thread step of any pane so far (showing a selection, applying a read), in µs (PLAN.md §4.4). */
gint64 dp_pane_get_max_main_us (void);

/* One line describing what is shown: title, preview kind, groups and rows. Caller frees. */
char *dp_pane_describe (DpPane *self);

G_END_DECLS
