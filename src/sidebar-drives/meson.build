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

# Not linked into the module yet: the sidebar hook and the feature entry points arrive in Phase 2.
