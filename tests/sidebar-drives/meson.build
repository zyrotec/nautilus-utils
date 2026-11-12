sd_test_env = environment()
sd_test_env.set('G_DEBUG', 'fatal-warnings,gc-friendly')
sd_test_env.set('GIO_USE_VFS', 'local')        # no gvfs daemons in tests
sd_test_env.set('GSETTINGS_BACKEND', 'memory')

test_drive_info = executable('test-drive-info', 'test-drive-info.c',
  link_with: sd_core_lib,
  dependencies: sd_core_deps,
  include_directories: sd_inc,
)
test('drive-info', test_drive_info, env: sd_test_env, protocol: 'tap', args: ['--tap'],
     suite: 'sidebar-drives', timeout: 120)

test_bar_style = executable('test-bar-style', 'test-bar-style.c',
  link_with: sd_core_lib,
  dependencies: sd_core_deps + [gtk_dep],
  include_directories: [sd_inc, nu_common_inc],
)
test('bar-style', test_bar_style, env: sd_test_env, protocol: 'tap', args: ['--tap'], suite: 'sidebar-drives')

test_fade_bin = executable('test-fade-bin', 'test-fade-bin.c',
  link_with: sd_ui_lib,
  dependencies: [gtk_dep],
  include_directories: [sd_inc],
)
test('fade-bin', test_fade_bin, env: sd_test_env, protocol: 'tap', args: ['--tap'], suite: 'sidebar-drives')
