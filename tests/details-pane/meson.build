dp_test_env = environment()
dp_test_env.set('G_DEBUG', 'fatal-warnings,gc-friendly')
dp_test_env.set('GIO_USE_VFS', 'local')        # no gvfs daemons in tests
dp_test_env.set('GSETTINGS_BACKEND', 'memory')
dp_test_env.set('G_TEST_SRCDIR', meson.current_source_dir())
dp_test_env.set('G_TEST_BUILDDIR', meson.current_build_dir())
dp_test_env.set('LC_ALL', 'en_US.utf8')
dp_test_env.set('TZ', 'UTC')

test_dp_format = executable('test-dp-format', 'test-dp-format.c',
  link_with: dp_core_lib,
  dependencies: dp_core_deps,
  include_directories: dp_inc,
)
test('dp-format', test_dp_format, env: dp_test_env, protocol: 'tap', args: ['--tap'], suite: 'details-pane')

test_dp_meta = executable('test-dp-meta', 'test-dp-meta.c',
  c_args: dp_c_args,
  link_with: [dp_core_lib, test_util_lib],
  dependencies: dp_core_deps,
  include_directories: [dp_inc, include_directories('../folder-preview')],
)
test('dp-meta', test_dp_meta, env: dp_test_env, protocol: 'tap', args: ['--tap'], suite: 'details-pane',
     timeout: 120)

# Not a test: the pane on its own for a look (needs a display). See demo-pane.c.
executable('demo-pane', 'demo-pane.c',
  link_with: [dp_ui_lib, dp_core_lib],
  dependencies: dp_ui_deps,
  include_directories: dp_inc,
)
