# Git (PLAN.md §14). Phase 0: a spike, inert unless NU_GT_SPIKE=1 with in-memory settings (the smoke session).

gt_c_args = ['-DG_LOG_DOMAIN="git"'] + (get_option('debug_driver') ? ['-DGT_DEBUG_DRIVER'] : [])
gt_feature_lib = static_library('gt-feature',
  'git.c',
  c_args: gt_c_args,
  dependencies: ext_deps + [gtk_dep, adw_dep],
  include_directories: include_directories('..'),
  pic: true,
  gnu_symbol_visibility: 'hidden',
)
feature_libs += [gt_feature_lib]
