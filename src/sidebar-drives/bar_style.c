/* Usage-bar colours (PLAN.md §9.1.5). See bar_style.h. */
#include "bar_style.h"

/* Every filled block, including the warning/full states, so a chosen colour always wins (N3). */
#define FILLED_SELECTORS                                    \
  "levelbar.sd-usage-bar block.filled, "                    \
  "levelbar.sd-usage-bar.sd-warning block.filled, "         \
  "levelbar.sd-usage-bar.sd-full block.filled"

/* Returns a newly allocated string; caller frees with g_free. */
char *
sd_bar_colour_css (const NuConfig *config)
{
  switch (config->bar_colour_mode)
    {
    case NU_BAR_COLOUR_CUSTOM:
      {
        g_autofree char *colour = nu_rgb_to_string (config->bar_colour);
        return g_strdup_printf (FILLED_SELECTORS " { background-color: %s; background-image: none; }\n", colour);
      }
    case NU_BAR_COLOUR_GRADIENT:
      {
        g_autofree char *start = nu_rgb_to_string (config->gradient_start);
        g_autofree char *end = nu_rgb_to_string (config->gradient_end);
        /* The filled block is exactly as wide as the used part, so the gradient spans the filled part. */
        return g_strdup_printf (FILLED_SELECTORS " { background-color: transparent; "
                                "background-image: linear-gradient(to right, %s, %s); }\n", start, end);
      }
    case NU_BAR_COLOUR_ACCENT:
    default:
      return g_strdup ("");
    }
}
