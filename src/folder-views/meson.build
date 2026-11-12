# Folder-views feature: every folder remembers its own view type and icon size (PLAN.md §10).

fv_c_args = ['-DG_LOG_DOMAIN="folder-views"']
fv_core_deps = [glib_dep, gobject_dep, gio_dep]
fv_inc = include_directories('.')

# Nautilus-free core (Phase 1): saved-value format, location kinds, zoom ↔ icon size, LRU cache.
fv_core_lib = static_library('fv-core',
  'fv_state.c',
  c_args: fv_c_args,
  dependencies: fv_core_deps,
  include_directories: [fv_inc, nu_common_inc],
  link_with: nu_common_lib,
  pic: true,
  gnu_symbol_visibility: 'hidden',
)
# Linked into the module from Phase 2, together with the Nautilus hook.
