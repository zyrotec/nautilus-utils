/* Usage-bar colours from the settings (PLAN.md §9.1.5) as a CSS rule. GLib only, unit-tested. */
#pragma once

#include "nu_settings.h"

G_BEGIN_DECLS

/* CSS that colours the filled part of every usage bar for @config's colour mode:
 *  - accent: "" (the built-in rules apply: accent colour, warning ≥ 90 %, error ≥ 95 %);
 *  - custom: the custom colour, also on warning/full bars (N3);
 *  - gradient: start → end, left to right, across the filled part (N2), also on warning/full bars.
 * Caller owns the returned string (g_free). */
char *sd_bar_colour_css (const NuConfig *config);

G_END_DECLS
