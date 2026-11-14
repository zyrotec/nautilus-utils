fi_test_env = environment()
fi_test_env.set('G_DEBUG', 'fatal-warnings,gc-friendly')
fi_test_env.set('GIO_USE_VFS', 'local')        # no gvfs daemons in tests
fi_test_env.set('GSETTINGS_BACKEND', 'memory')
fi_test_env.set('G_TEST_SRCDIR', meson.current_source_dir())
fi_test_env.set('G_TEST_BUILDDIR', meson.current_build_dir())

test_fi_rules = executable('test-fi-rules', 'test-fi-rules.c',
  link_with: fi_core_lib,
  dependencies: fi_core_deps,
  include_directories: [fi_inc, nu_common_inc],
)
test('fi-rules', test_fi_rules, env: fi_test_env, protocol: 'tap', args: ['--tap'], suite: 'folder-icons')

test_icon_store = executable('test-icon-store', 'test-icon-store.c',
  link_with: nu_common_lib,
  dependencies: fi_core_deps,
  include_directories: nu_common_inc,
)
test('icon-store', test_icon_store, env: fi_test_env, protocol: 'tap', args: ['--tap'], suite: 'folder-icons')

test_fi_apply = executable('test-fi-apply', 'test-fi-apply.c',
  link_with: fi_core_lib,
  dependencies: fi_core_deps,
  include_directories: [fi_inc, nu_common_inc],
)
test('fi-apply', test_fi_apply, env: fi_test_env, protocol: 'tap', args: ['--tap'], suite: 'folder-icons',
     timeout: 60)
