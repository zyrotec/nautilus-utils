# GSettings schema (PLAN.md §9.3). Installed system-wide by `meson install` / the RPM; compiled into the
# build dir for the tests (GSETTINGS_SCHEMA_DIR = this dir).
gnome = import('gnome')

nu_schema_file = files('io.github.zyrotec.NautilusUtils.gschema.xml')
nu_schemas_compiled = gnome.compile_schemas(depend_files: nu_schema_file)
nu_schema_dir = meson.current_build_dir()

install_data(nu_schema_file, install_dir: get_option('datadir') / 'glib-2.0' / 'schemas')
gnome.post_install(glib_compile_schemas: true)
