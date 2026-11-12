project('nautilus-utils', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>= 1.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',   # -Wall -Wextra
    'werror=true',
    'buildtype=debug',
  ],
)

cc = meson.get_compiler('c')

# Strict warnings on top of -Wall -Wextra -Werror. G_LOG_DOMAIN is set per feature (see src/*/meson.build).
add_project_arguments(
  '-Wformat=2',
  '-Wnull-dereference',
  '-Wshadow',
  '-Wmissing-prototypes',
  '-Wstrict-prototypes',
  # Fail the build if we use a GLib API newer than what we compile against.
  '-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_80',
  language: 'c',
)

if get_option('analyzer')
  if cc.get_id() != 'gcc'
    error('-Danalyzer=true needs GCC')
  endif
  add_project_arguments('-fanalyzer', language: 'c')
endif

glib_dep = dependency('glib-2.0', version: '>= 2.80')
gobject_dep = dependency('gobject-2.0', version: '>= 2.80')
gio_dep = dependency('gio-2.0', version: '>= 2.80')
gmodule_dep = dependency('gmodule-2.0', version: '>= 2.80')
# API 4 exists since Nautilus 43; only 51.1 has been tested.
nautilus_dep = dependency('libnautilus-extension-4', version: '>= 43')

extensiondir = get_option('extensiondir')
if extensiondir == ''
  # Nautilus only loads from this compiled-in path, so we don't rebase it on --prefix.
  extensiondir = nautilus_dep.get_variable(pkgconfig: 'extensiondir')
endif

ext_deps = [glib_dep, gobject_dep, gio_dep, nautilus_dep]

# Each feature adds its static libraries (linked whole into the module) and its dependencies.
feature_libs = []
feature_deps = []

subdir('data')
subdir('src/common')
subdir('src/folder-preview')
subdir('src/sidebar-drives')
subdir('src/settings')
subdir('src/folder-views')

# Built at the build root as build/libnautilus-utils.so, which is what the dev symlink points at (README.md).
nu_module = shared_module('nautilus-utils',
  'src/extension.c',
  link_whole: feature_libs,
  dependencies: ext_deps + feature_deps,
  include_directories: include_directories('src'),
  c_args: ['-DG_LOG_DOMAIN="nautilus-utils"'],
  gnu_symbol_visibility: 'hidden',
  install: true,
  install_dir: extensiondir,
)

subdir('tests/common')
subdir('tests/folder-preview')
subdir('tests/sidebar-drives')
subdir('tests/folder-views')

summary({
  'extensiondir': extensiondir,
  'analyzer (-fanalyzer)': get_option('analyzer'),
  'sanitizers': get_option('b_sanitize'),
  'debug driver': get_option('debug_driver'),
}, section: 'nautilus-utils')
