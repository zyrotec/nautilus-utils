# Folder icons by name: every folder with a rule's name gets the rule's icon as Files' own custom icon
# (PLAN.md §12).

fi_c_args = ['-DG_LOG_DOMAIN="folder-icons"']
fi_core_deps = [glib_dep, gobject_dep, gio_dep]
fi_inc = include_directories('.')

# Nautilus-free core: rules, markers, decisions; applying them through GIO metadata.
fi_core_lib = static_library('fi-core',
  'fi_rules.c',
  c_args: fi_c_args,
  dependencies: fi_core_deps,
  include_directories: [fi_inc, nu_common_inc],
  link_with: nu_common_lib,
  pic: true,
  gnu_symbol_visibility: 'hidden',
)

feature_libs += [fi_core_lib]
