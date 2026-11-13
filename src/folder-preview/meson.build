# Folder-preview feature: Dolphin-style thumbnails drawn on folder icons in the grid view.

fp_c_args = ['-DG_LOG_DOMAIN="folder-preview"']

gnome_desktop_dep = dependency('gnome-desktop-4', version: '>= 43')
gdk_pixbuf_dep = dependency('gdk-pixbuf-2.0') # not pulled in by gnome-desktop-4.pc
gtk_dep = dependency('gtk4', version: '>= 4.12') # GdkTexture loaders/downloader; no display needed
cairo_dep = dependency('cairo', version: '>= 1.14') # frame drawing (rotation + good downscale filter)
m_dep = cc.find_library('m', required: true) # lround() in folder_paintable.c
core_deps = [glib_dep, gobject_dep, gio_dep, gnome_desktop_dep, gdk_pixbuf_dep, gtk_dep, cairo_dep, m_dep]

conf_inc = include_directories('.')

# Nautilus-independent core: scanner, thumbs, composite, cache. Testable standalone.
fp_core_lib = static_library('fp-core',
  'workers.c',
  'scanner.c',
  'thumbs.c',
  'composite.c',
  'cache.c',
  c_args: fp_c_args,
  dependencies: core_deps,
  include_directories: conf_inc,
  pic: true,
  gnu_symbol_visibility: 'hidden',
)

# Plain-GTK drawing code (no Nautilus internals), shared by the module and its unit test.
fp_ui_lib = static_library('fp-ui',
  'folder_paintable.c',
  'icon_art.c',
  c_args: fp_c_args,
  dependencies: core_deps,
  include_directories: conf_inc,
  pic: true,
  gnu_symbol_visibility: 'hidden',
)

# Nautilus info provider, in its own library so tests can link it directly.
fp_ext_lib = static_library('fp-ext',
  'info_provider.c',
  c_args: fp_c_args,
  dependencies: ext_deps,
  include_directories: conf_inc,
  pic: true,
  gnu_symbol_visibility: 'hidden',
)

# Feature entry points + the view hook (the only file using Nautilus internals).
fp_feature_c_args = fp_c_args + (get_option('debug_driver') ? ['-DFP_DEBUG_DRIVER'] : [])
fp_feature_lib = static_library('fp-feature',
  'folder_preview.c',
  'view_hook.c',
  c_args: fp_feature_c_args,
  dependencies: ext_deps + core_deps,
  include_directories: [conf_inc, nu_common_inc], # nu_settings.h (PLAN.md §9)
  pic: true,
  gnu_symbol_visibility: 'hidden',
)

feature_libs += [fp_feature_lib, fp_ext_lib, fp_core_lib, fp_ui_lib]
feature_deps += core_deps
