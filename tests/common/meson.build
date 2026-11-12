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
