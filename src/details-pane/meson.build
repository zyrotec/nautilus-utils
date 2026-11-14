# Details pane: the selected item's preview and properties right of the folder view (PLAN.md §13).

dp_c_args = ['-DG_LOG_DOMAIN="details-pane"']

gst_pbutils_dep = dependency('gstreamer-pbutils-1.0', version: '>= 1.20')
gexiv2_dep = dependency('gexiv2-0.16', required: false)  # EXIF + image size (PLAN.md §13.2, P9)
poppler_dep = dependency('poppler-glib', required: false) # PDF properties
if gexiv2_dep.found()
  dp_c_args += ['-DDP_HAVE_GEXIV2']
endif
if poppler_dep.found()
  dp_c_args += ['-DDP_HAVE_POPPLER']
endif
dp_core_deps = [glib_dep, gobject_dep, gio_dep, gdk_pixbuf_dep, gst_pbutils_dep, gexiv2_dep, poppler_dep]
dp_inc = include_directories('.')

# Nautilus-free core: formatting and readers (PLAN.md §13.3 rule 5).
dp_core_lib = static_library('dp-core',
  'dp_format.c',
  'dp_meta.c',
  c_args: dp_c_args,
  dependencies: dp_core_deps + [m_dep],
  include_directories: dp_inc,
  pic: true,
  gnu_symbol_visibility: 'hidden',
)

feature_libs += [dp_core_lib]
feature_deps += dp_core_deps

summary({
  'gexiv2 (EXIF)': gexiv2_dep.found(),
  'poppler-glib (PDF)': poppler_dep.found(),
}, section: 'details pane')
