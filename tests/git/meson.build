gt_test_env = environment()
gt_test_env.set('G_DEBUG', 'fatal-warnings,gc-friendly')
gt_test_env.set('GIO_USE_VFS', 'local')
gt_test_env.set('G_TEST_SRCDIR', meson.current_source_dir())
gt_test_env.set('G_TEST_BUILDDIR', meson.current_build_dir())
# git without the user's configuration (no credential helper, no hooks, no signing), a fixed identity, "main".
gt_test_env.set('GIT_CONFIG_GLOBAL', '/dev/null')
gt_test_env.set('GIT_CONFIG_NOSYSTEM', '1')
gt_test_env.set('GIT_CONFIG_COUNT', '1')
gt_test_env.set('GIT_CONFIG_KEY_0', 'init.defaultBranch')
gt_test_env.set('GIT_CONFIG_VALUE_0', 'main')
gt_test_env.set('GIT_AUTHOR_NAME', 'Test Author')
gt_test_env.set('GIT_AUTHOR_EMAIL', 'author@example.invalid')
gt_test_env.set('GIT_COMMITTER_NAME', 'Test Author')
gt_test_env.set('GIT_COMMITTER_EMAIL', 'author@example.invalid')

test_gt_parse = executable('test-gt-parse', 'test-gt-parse.c',
  link_with: gt_core_lib,
  dependencies: gt_core_deps,
  include_directories: gt_inc,
)
test('gt-parse', test_gt_parse, env: gt_test_env, protocol: 'tap', args: ['--tap'], suite: 'git')

test_gt_git = executable('test-gt-git', 'test-gt-git.c',
  link_with: gt_core_lib,
  dependencies: gt_core_deps,
  include_directories: gt_inc,
)
test('gt-git', test_gt_git, env: gt_test_env, protocol: 'tap', args: ['--tap'], suite: 'git', timeout: 180)
