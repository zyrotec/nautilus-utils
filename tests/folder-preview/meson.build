test_env = environment()
test_env.set('G_DEBUG', 'fatal-warnings,gc-friendly')
test_env.set('G_TEST_SRCDIR', meson.current_source_dir())
test_env.set('G_TEST_BUILDDIR', meson.current_build_dir())
test_env.set('GIO_USE_VFS', 'local')        # no gvfs daemons in tests
test_env.set('GSETTINGS_BACKEND', 'memory')
test_env.set('FP_MODULE_PATH', nu_module.full_path())
test_env.set('LC_ALL', 'en_US.utf8')        # deterministic natural-order collation

fake_lib = static_library('fake-file-info', 'fake-file-info.c',
  dependencies: ext_deps,
)

test_info_provider = executable('test-info-provider', 'test-info-provider.c',
  link_with: [fp_ext_lib, fake_lib],
  dependencies: ext_deps,
  include_directories: conf_inc,
)
test('info-provider', test_info_provider, env: test_env, protocol: 'tap', args: ['--tap'])

test_module = executable('test-module', 'test-module.c',
  link_with: fake_lib,
  dependencies: ext_deps + [gmodule_dep],
)
test('module', test_module, env: test_env, protocol: 'tap', args: ['--tap'], depends: nu_module)


test_util_lib = static_library('test-util', 'test-util.c', dependencies: core_deps)

test_scanner = executable('test-scanner', 'test-scanner.c',
  link_with: [fp_core_lib, test_util_lib],
  dependencies: core_deps,
  include_directories: conf_inc,
)
test('scanner', test_scanner, env: test_env, protocol: 'tap', args: ['--tap'], timeout: 120)

test_latency = executable('test-latency', 'test-latency.c',
  link_with: [fp_core_lib, test_util_lib],
  dependencies: core_deps,
  include_directories: conf_inc,
)
# Timing-sensitive: run alone, and not under valgrind (suite 'latency' is excluded there).
test('latency', test_latency, env: test_env, protocol: 'tap', args: ['--tap'], timeout: 120,
     is_parallel: false, suite: 'latency')

test_thumbs = executable('test-thumbs', 'test-thumbs.c',
  link_with: [fp_core_lib, test_util_lib],
  dependencies: core_deps,
  include_directories: conf_inc,
)
# Runs real (sandboxed) thumbnailers; media + cache live under the build dir, not /tmp (PLAN.md §4.2).
test('thumbs', test_thumbs, env: test_env, protocol: 'tap', args: ['--tap'], timeout: 300,
     suite: 'thumbnailers')

test_composite = executable('test-composite', 'test-composite.c',
  link_with: [fp_core_lib, test_util_lib],
  dependencies: core_deps,
  include_directories: conf_inc,
)
test('composite', test_composite, env: test_env, protocol: 'tap', args: ['--tap'], timeout: 120)

test_cache = executable('test-cache', 'test-cache.c',
  link_with: [fp_core_lib, test_util_lib],
  dependencies: core_deps,
  include_directories: conf_inc,
)
test('cache', test_cache, env: test_env, protocol: 'tap', args: ['--tap'], timeout: 300,
     suite: 'thumbnailers')

test_folder_paintable = executable('test-folder-paintable', 'test-folder-paintable.c',
  link_with: fp_ui_lib,
  dependencies: core_deps,
  include_directories: conf_inc,
)
test('folder-paintable', test_folder_paintable, env: test_env, protocol: 'tap', args: ['--tap'])

# Phase 6 profiling tool (not a test): build/tests/bench-core PARENT_DIR [frame-width]
executable('bench-core', 'bench-core.c',
  link_with: fp_core_lib,
  dependencies: core_deps,
  include_directories: conf_inc,
)
