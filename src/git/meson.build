# Git (PLAN.md §14).

gt_c_args = ['-DG_LOG_DOMAIN="git"']
gt_inc = include_directories('.')

# Nautilus- and GTK-free core: parsers, repository detection, the git runner, repository info, operations, IDEs.
gt_core_deps = [glib_dep, gobject_dep, gio_dep, dependency('gio-unix-2.0')]
gt_core_lib = static_library('gt-core',
  'gt_parse.c',
  'gt_repo.c',
  'gt_run.c',
  'gt_info.c',
  'gt_ops.c',
  'gt_ide.c',
  c_args: gt_c_args,
  dependencies: gt_core_deps,
  include_directories: gt_inc,
  pic: true,
  gnu_symbol_visibility: 'hidden',
)

gnome = import('gnome')
gt_resources = gnome.compile_resources('gt_resources', 'gt.gresource.xml', c_name: 'gt')

# Plain GTK widgets: the status bar (no Nautilus types).
gt_ui_deps = gt_core_deps + [gtk_dep, adw_dep]
gt_ui_lib = static_library('gt-ui',
  'gt_bar.c',
  gt_resources,
  c_args: gt_c_args,
  dependencies: gt_ui_deps,
  include_directories: gt_inc,
  link_with: gt_core_lib,
  pic: true,
  gnu_symbol_visibility: 'hidden',
)

# Feature entry point + the hook (the only Git file using Nautilus' widgets).
gt_feature_lib = static_library('gt-feature',
  'git.c',
  'gt_columns.c',
  'gt_hook.c',
  gt_resources[1], # the header only (the code is in gt-ui)
  c_args: gt_c_args + (get_option('debug_driver') ? ['-DGT_DEBUG_DRIVER'] : []),
  dependencies: ext_deps + gt_ui_deps,
  include_directories: [gt_inc, nu_common_inc, include_directories('..')],
  pic: true,
  gnu_symbol_visibility: 'hidden',
)
feature_libs += [gt_feature_lib, gt_ui_lib, gt_core_lib]
