# Sidebar-drives feature: usage bars on drive rows, OS drive row, network shares (PLAN.md §8).

sd_c_args = ['-DG_LOG_DOMAIN="sidebar-drives"']
sd_core_deps = [glib_dep, gobject_dep, gio_dep]
sd_inc = include_directories('.')

# Nautilus-free core (Phase 1): OS name, classification, async size queries, usage text. Testable standalone.
sd_core_lib = static_library('sd-core',
  'drive_info.c',
  c_args: sd_c_args,
  dependencies: sd_core_deps,
  include_directories: sd_inc,
  pic: true,
  gnu_symbol_visibility: 'hidden',
)

gtk_dep = dependency('gtk4', version: '>= 4.12')

# Feature entry points + the sidebar hook (the only file using Nautilus internals, PLAN.md §8.1.4).
sd_feature_c_args = sd_c_args + (get_option('debug_driver') ? ['-DSD_DEBUG_DRIVER'] : [])
sd_feature_lib = static_library('sd-feature',
  'sidebar_drives.c',
  'sidebar_hook.c',
  c_args: sd_feature_c_args,
  dependencies: sd_core_deps + [gtk_dep],
  include_directories: [sd_inc, nu_common_inc], # nu_settings.h (PLAN.md §9)
  pic: true,
  gnu_symbol_visibility: 'hidden',
)

feature_libs += [sd_feature_lib, sd_core_lib]
feature_deps += [gtk_dep]
