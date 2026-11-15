/* Details-pane feature: set-up, tear-down and settings (PLAN.md §13). Called from src/extension.c.
 * Everything the user sees is in dp_pane.c; everything touching Nautilus' widgets in dp_hook.c. */
#include "details_pane.h"

#include "dp_hook.h"
#include "dp_pane.h"
#include "dp_thumb.h"
#include "folder-preview/view_hook.h"
#include "nu_settings.h"

#ifdef DP_DEBUG_DRIVER
#include <glib-unix.h>
#include <signal.h>
#endif

static guint settings_listener; /* nu_settings listener id, 0 when not registered */
#ifdef DP_DEBUG_DRIVER
static guint debug_signal_id;
#endif

/* The look options (PLAN.md §13.10) to the pane and the hook. */
static void
apply_look (void)
{
  const NuConfig *c = nu_settings_get ();
  DpPaneStyle st = {
    .border = c->details_pane_border,
    .margin = c->details_pane_margin,
    .padding = c->details_pane_padding,
    .radius = c->details_pane_radius,
    .preview_radius = c->details_pane_preview_radius,
    .preview_margin = c->details_pane_preview_margin,
    .stack_depth = (int) c->details_pane_stack_depth, /* same values as FpDepth */
    .stack_shadow = c->details_pane_stack_shadow,
    .stack_rotation = c->details_pane_stack_rotation,
    .stack_keep_aspect = c->details_pane_stack_keep_aspect,
    .stack_fade = c->details_pane_stack_fade,
    .stack_card = c->details_pane_stack_card,
    .icon_size = c->details_pane_icon_size,
    .media_box = c->details_pane_media_fit == NU_MEDIA_FIT_BOX,
    .title_centered = c->details_pane_title_centered,
    .show_icon = c->details_pane_show_icon,
    .stack_fp_look = c->details_pane_stack_folder_look,
  };
  if (!fp_view_hook_get_style (&st.fp_style))
    fp_style_init_default (&st.fp_style);
  dp_pane_set_style (&st);
  dp_hook_set_look (c->details_pane_position == NU_PANE_BOTTOM, c->details_pane_header_border);
}

static void
on_settings_changed (NuChanged changed, gpointer data)
{
  (void) data;
  const NuConfig *c = nu_settings_get ();
  /* Folder preview look changes reach the pane's stack too (P22); folder-preview's own listener ran first. */
  if (changed & (NU_CHANGED_DP_LOOK | NU_CHANGED_PREVIEW_STYLE))
    apply_look ();
  if (changed & NU_CHANGED_DP_STATE)
    dp_hook_set_state (c->details_pane_visible, c->details_pane_width, c->details_pane_height);
  if (changed & NU_CHANGED_DETAILS_PANE)
    dp_hook_set_enabled (c->details_pane_enabled);
}

#ifdef DP_DEBUG_DRIVER
/* Debug driver (only in -Ddebug_driver=true builds, tools/smoke-nautilus.sh): "dp-*" commands. */
static gboolean
on_debug_signal (gpointer data)
{
  (void) data;
  const char *dir = g_getenv ("FP_DEBUG_DIR");
  g_autofree char *path = dir != NULL ? g_build_filename (dir, "cmd", NULL) : NULL;
  g_autofree char *cmd = NULL;
  if (path != NULL && g_file_get_contents (path, &cmd, NULL, NULL))
    dp_hook_debug_command (g_strstrip (cmd));
  return G_SOURCE_CONTINUE;
}
#endif

void
dp_feature_initialize (GTypeModule *module)
{
  (void) module;
  /* Folders in the pane get the folder-preview look (P4) when that feature draws them. */
  dp_pane_set_folder_renderer (fp_view_hook_render_folder);
  dp_hook_init (nu_settings_get ()->details_pane_enabled);
  apply_look ();
  settings_listener = nu_settings_add_listener (on_settings_changed, NULL);
#ifdef DP_DEBUG_DRIVER
  debug_signal_id = g_unix_signal_add (SIGUSR2, on_debug_signal, NULL);
#endif
  g_debug ("details-pane loaded");
}

void
dp_feature_shutdown (void)
{
#ifdef DP_DEBUG_DRIVER
  g_clear_handle_id (&debug_signal_id, g_source_remove);
#endif
  nu_settings_remove_listener (settings_listener);
  settings_listener = 0;
  dp_hook_shutdown ();
  dp_pane_set_folder_renderer (NULL);
  dp_pane_unload_style ();
  dp_thumb_shutdown ();
  g_debug ("details-pane shut down");
}

const GType *
dp_feature_list_types (int *num_types)
{
  *num_types = 0;
  return NULL;
}
