# Up and Refresh buttons next to Files' Back and Forward buttons.

nb_feature_lib = static_library('nb-feature',
  'nav_buttons.c',
  c_args: ['-DG_LOG_DOMAIN="nav-buttons"'] + (get_option('debug_driver') ? ['-DNB_DEBUG_DRIVER'] : []),
  dependencies: ext_deps + [gtk_dep, adw_dep],
  include_directories: [include_directories('.'), nu_common_inc],
  pic: true,
  gnu_symbol_visibility: 'hidden',
)
feature_libs += [nb_feature_lib]
