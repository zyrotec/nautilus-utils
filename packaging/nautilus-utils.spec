# RPM spec for Fedora. Build:
#   tools/make-tarball.sh                       # → ~/rpmbuild/SOURCES/nautilus-utils-<ver>.tar.xz
#   rpmbuild -ba packaging/nautilus-utils.spec  (or: fedpkg/mock with the SRPM)

Name:           nautilus-utils
Version:        0.1.0
Release:        1%{?dist}
Summary:        Extra features for GNOME Files (Nautilus)
License:        MIT
Source0:        %{name}-%{version}.tar.xz

BuildRequires:  meson >= 1.0
BuildRequires:  gcc
BuildRequires:  pkgconfig(libnautilus-extension-4) >= 43
BuildRequires:  pkgconfig(glib-2.0) >= 2.80
BuildRequires:  pkgconfig(gio-2.0)
BuildRequires:  pkgconfig(gmodule-2.0)
BuildRequires:  pkgconfig(gtk4) >= 4.12
BuildRequires:  pkgconfig(gnome-desktop-4)
BuildRequires:  pkgconfig(gdk-pixbuf-2.0)
BuildRequires:  pkgconfig(cairo) >= 1.14
# For %check: the natural-order test pins LC_ALL=en_US.utf8; type guessing by name needs the MIME database.
BuildRequires:  glibc-langpack-en
BuildRequires:  shared-mime-info

# The extension hooks into Nautilus' private grid widgets: it was verified against Nautilus 51.1 and
# disables itself (one warning) if a Nautilus update changes them.
Requires:       nautilus%{?_isa} >= 51
# Audio cover art comes from the system audio thumbnailer (PLAN.md D2).
Requires:       gst-thumbnailers

%description
A Nautilus extension that adds extra features to GNOME Files.

Folder preview: shows a 2×2 preview of the first images, videos and album covers of each folder on its icon in the
Nautilus grid view, Dolphin style (tilted photos, aspect ratio kept), and cycles through the next ones
while the pointer hovers over the folder. Thumbnails come from the shared freedesktop thumbnail cache;
everything heavy runs in background threads.

%prep
%autosetup

%build
# -Werror stays on in the project's own safety checks, not in distro builds (a future compiler's new
# warning must not break the package). Verified warning-free with Fedora 45's optflags incl. LTO and
# _FORTIFY_SOURCE=3 (PLAN.md §6.5).
%meson -Ddebug_driver=false -Dwerror=false
%meson_build

%install
%meson_install

%check
# 'thumbnailers' tests run the real sandboxed thumbnailers (bubblewrap), which need user namespaces that
# build roots like mock usually don't allow; 'latency' is timing-sensitive. Both run in tools/check-safety.sh.
%meson_test --no-suite thumbnailers --no-suite latency

%files
%license LICENSE
%doc README.md
%{_libdir}/nautilus/extensions-4/libnautilus-utils.so

%changelog
* Tue Nov 10 2026 Packager <packager@example.invalid> - 0.1.0-1
- First package
