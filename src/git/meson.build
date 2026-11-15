# Git (PLAN.md §14).

gt_c_args = ['-DG_LOG_DOMAIN="git"']
gt_inc = include_directories('.')

# Nautilus- and GTK-free core: parsers, repository detection, the git runner (PLAN.md §14.4 Phase 1).
gt_core_deps = [glib_dep, gobject_dep, gio_dep]
gt_core_lib = static_library('gt-core',
  'gt_parse.c',
  'gt_repo.c',
  'gt_run.c',
  c_args: gt_c_args,
  dependencies: gt_core_deps,
  include_directories: gt_inc,
  pic: true,
  gnu_symbol_visibility: 'hidden',
)

# Feature entry point. Until Phase 2: the Phase 0 spike, inert unless NU_GT_SPIKE=1 with in-memory settings.
gt_feature_lib = static_library('gt-feature',
  'git.c',
  c_args: gt_c_args + (get_option('debug_driver') ? ['-DGT_DEBUG_DRIVER'] : []),
  dependencies: ext_deps + [gtk_dep, adw_dep],
  include_directories: [gt_inc, include_directories('..')],
  link_with: gt_core_lib,
  pic: true,
  gnu_symbol_visibility: 'hidden',
)
feature_libs += [gt_feature_lib, gt_core_lib]
