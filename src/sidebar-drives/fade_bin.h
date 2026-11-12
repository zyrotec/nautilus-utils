/* SdFadeBin: a one-child container that fades the right end of its child out through an alpha mask
 * (PLAN.md §9.1.4). Used under the sliding eject button: the mask is exactly as wide as the button (with
 * its margins) at every moment of the slide, so the row content fades out where the button covers it.
 * Plain GTK, no Nautilus internals. Main thread only. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define SD_TYPE_FADE_BIN (sd_fade_bin_get_type ())
G_DECLARE_FINAL_TYPE (SdFadeBin, sd_fade_bin, SD, FADE_BIN, GtkWidget)

/* A new, empty fade bin (floating ref, like every new widget). */
GtkWidget *sd_fade_bin_new (void);

/* Sets the child (the bin takes it; any previous child is unparented). NULL removes it. */
void sd_fade_bin_set_child (SdFadeBin *self,
                            GtkWidget *child);

/* The child, or NULL. Borrowed. */
GtkWidget *sd_fade_bin_get_child (SdFadeBin *self);

/* Fixed width in px of the fade at the right edge (transparent at the edge, opaque @width px to the left;
 * 0 = no mask). Ignored while following a revealer. */
void   sd_fade_bin_set_fade_width (SdFadeBin *self,
                                   double     width);
double sd_fade_bin_get_fade_width (SdFadeBin *self);

/* Makes the fade exactly as wide as @revealer (the sliding eject button incl. margins) on every frame,
 * 0 while its child is hidden. Weak: stops when the revealer is destroyed or on sd_fade_bin_follow (self,
 * NULL). */
void sd_fade_bin_follow (SdFadeBin   *self,
                         GtkRevealer *revealer);

/* The mask (exposed for tests): a horizontal gradient from @opaque (outer edge of the button) to @clear
 * (end of the row) with these stops — opaque at the button's outer edge, transparent from its middle on,
 * so the content has faded out before it reaches the icon. */
#define SD_FADE_MASK_N_STOPS 3
extern const GskColorStop sd_fade_mask_stops[SD_FADE_MASK_N_STOPS];

/* Gradient line for a bin @width px wide with a @fade px fade (clamped to @width). Returns FALSE when there
 * is nothing to mask (fade < 0.5 px or empty bin). */
gboolean sd_fade_mask_line (float             width,
                            float             fade,
                            gboolean          rtl,
                            graphene_point_t *opaque,
                            graphene_point_t *clear);

G_END_DECLS
