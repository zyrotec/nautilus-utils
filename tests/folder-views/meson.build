fv_test_env = environment()
fv_test_env.set('G_DEBUG', 'fatal-warnings,gc-friendly')
fv_test_env.set('GIO_USE_VFS', 'local')        # no gvfs daemons in tests
fv_test_env.set('GSETTINGS_BACKEND', 'memory')

test_fv_state = executable('test-fv-state', 'test-fv-state.c',
  link_with: fv_core_lib,
  dependencies: fv_core_deps,
  include_directories: [fv_inc, nu_common_inc],
)
test('fv-state', test_fv_state, env: fv_test_env, protocol: 'tap', args: ['--tap'], suite: 'folder-views')
