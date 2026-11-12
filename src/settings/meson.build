# Settings feature: main-menu entry + settings dialog (PLAN.md §9). The settings service itself is shared
# code in src/common/.

adw_dep = dependency('libadwaita-1', version: '>= 1.5') # AdwDialog, adw_dialog_add_breakpoint

st_feature_lib = static_library('st-feature',
  'settings.c',
  'settings_dialog.c',
  'menu_hook.c',
  c_args: ['-DG_LOG_DOMAIN="settings"'] + (get_option('debug_driver') ? ['-DST_DEBUG_DRIVER'] : []),
  dependencies: nu_common_deps + [gtk_dep, adw_dep],
  include_directories: [include_directories('.'), nu_common_inc],
  pic: true,
  gnu_symbol_visibility: 'hidden',
)

feature_libs += [st_feature_lib]
feature_deps += [adw_dep]
