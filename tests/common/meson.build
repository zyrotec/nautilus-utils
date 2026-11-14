subdir('old-schema')

nu_test_env = environment()
nu_test_env.set('G_DEBUG', 'fatal-warnings,gc-friendly')
nu_test_env.set('GSETTINGS_BACKEND', 'memory')
nu_test_env.set('GSETTINGS_SCHEMA_DIR', nu_schema_dir)

test_settings = executable('test-settings', 'test-settings.c', nu_schemas_compiled,
  link_with: nu_common_lib,
  dependencies: nu_common_deps,
  include_directories: nu_common_inc,
)
test('settings', test_settings, env: nu_test_env, protocol: 'tap', args: ['--tap'],
     suite: 'common', timeout: 60)

# The module updated but the installed schema not (PLAN.md §13.9): defaults + one warning, no abort.
nu_old_env = environment()
nu_old_env.set('G_DEBUG', 'gc-friendly')
nu_old_env.set('GSETTINGS_BACKEND', 'memory')
nu_old_env.set('GSETTINGS_SCHEMA_DIR', nu_old_schema_dir)
nu_old_env.set('NU_TEST_OUTDATED_SCHEMA', '1')
test('settings-outdated-schema', test_settings, env: nu_old_env, protocol: 'tap',
     args: ['--tap', '-p', '/settings/outdated-schema'], depends: nu_old_schemas_compiled, suite: 'common')
