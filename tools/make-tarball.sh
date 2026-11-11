#!/usr/bin/env bash
# Builds the source tarball for packaging/nautilus-utils.spec from the files tracked in git
# (no build dirs or logs). Usage: tools/make-tarball.sh [OUTDIR]     (default ~/rpmbuild/SOURCES)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="$(sed -n "s/^ *version: '\([^']*\)'.*/\1/p" "$ROOT/meson.build" | head -1)"
NAME="nautilus-utils-$VERSION"
OUT="${1:-$HOME/rpmbuild/SOURCES}"
mkdir -p "$OUT"
git -C "$ROOT" archive --format=tar --prefix="$NAME/" HEAD \
    meson.build meson_options.txt src tests tools packaging README.md LICENSE | xz > "$OUT/$NAME.tar.xz"
echo "$OUT/$NAME.tar.xz"
