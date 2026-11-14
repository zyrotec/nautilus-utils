# Code shared by several features (PLAN.md §3): the settings service (PLAN.md §9.2.3).

nu_common_deps = [glib_dep, gobject_dep, gio_dep]
nu_common_inc = include_directories('.')

nu_common_lib = static_library('nu-common',
  'nu_settings.c',
  'nu_icon_store.c',
  c_args: ['-DG_LOG_DOMAIN="settings"'],
  dependencies: nu_common_deps,
  include_directories: nu_common_inc,
  pic: true,
  gnu_symbol_visibility: 'hidden',
)

feature_libs += [nu_common_lib]
