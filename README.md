# nautilus-utils

A set of utilities to extend the functionality of the Nautilus file manager (GNOME Files), shipped as a
single native extension module: `libnautilus-utils.so`.

Tested on Fedora 45, Nautilus 51.1, GTK 4.24.

## Features

### Folder preview

Dolphin-style folder thumbnails: each folder icon in the grid view shows its first images, videos and album
covers as tilted photos (1 → one large photo, 2 → side by side, 3 → two on top and one below, 4+ →
quarters), and cycles through the next ones while the pointer hovers over it.

Known limitations:

- **Nautilus updates may switch the previews off.** Nautilus has no API for custom icons or hover, so the
  feature uses Nautilus' private grid widgets (verified on Nautilus 51.1). If an update changes them, it
  logs one warning ("folder previews disabled: …") and Nautilus keeps working with plain folder icons.
- **Grid view only.** List view shows plain icons.
- **Local folders only** (no network, MTP, etc.).
- A preview is keyed by the folder's modification time. Changes *inside* a folder you are looking at are
  picked up live; editing a file in place (which doesn't change the folder's time) shows up after the
  folder itself changes or the cache is cleared.
- Audio cover art needs `gst-thumbnailers`; audio without cover art is skipped.
- With **nautilus-python** extensions installed, Nautilus can stall for up to ~0.5 s while scrolling large
  folders whether or not this extension is installed (Python's global interpreter lock).

Clear its frame cache with `rm -rf ~/.cache/nautilus-utils/folder-preview` (rebuilt on demand). Thumbnails
themselves live in the shared `~/.cache/thumbnails`.

## Requirements

```
sudo dnf install meson ninja-build gcc nautilus-devel glib2-devel gnome-desktop4-devel gtk4-devel gst-thumbnailers
# for tools/check-safety.sh and tools/smoke-nautilus.sh:
sudo dnf install valgrind clang-analyzer libasan libubsan
```

## Build and test

```
meson setup build
ninja -C build
meson test -C build
```

## Install for development (recommended)

Nautilus only loads extensions from `/usr/lib64/nautilus/extensions-4` (no per-user folder). Link the
build output there **once**; after that every rebuild is picked up without sudo:

```
sudo ln -sf "$PWD/build/libnautilus-utils.so" /usr/lib64/nautilus/extensions-4/libnautilus-utils.so
```

Nautilus keeps running in the background, so a rebuild only takes effect after it quits:

```
nautilus -q
G_MESSAGES_DEBUG=nautilus-utils,folder-preview nautilus    # with log messages
```

## Install system-wide

```
sudo meson install -C build
```

or as an RPM (Fedora):

```
tools/make-tarball.sh                                  # → ~/rpmbuild/SOURCES/nautilus-utils-<ver>.tar.xz (from git HEAD)
rpmbuild -ba packaging/nautilus-utils.spec
sudo dnf install ~/rpmbuild/RPMS/x86_64/nautilus-utils-*.rpm
nautilus -q
```

Remove the dev symlink first if you made one.

## Uninstall

```
nautilus -q
sudo rm /usr/lib64/nautilus/extensions-4/libnautilus-utils.so     # dev symlink or meson install
# or: sudo dnf remove nautilus-utils
```

## Safety checks

```
tools/check-safety.sh                    # strict build, gcc -fanalyzer, scan-build, ASan+UBSan tests,
                                         # valgrind tests, coverage report. Non-zero exit = a finding.
tools/check-safety.sh asan               # run only some steps
tools/smoke-nautilus.sh DIR              # isolated real Nautilus with an ASan build
tools/smoke-nautilus.sh --auto 120 DIR   # unattended: scripted scroll/zoom/hover + self-checks
```

Logs land in `safety-logs/`.

## Benchmarks (folder preview)

```
tests/folder-preview/make-bench.sh         # 500 folders, ~517 000 files (hard links) in ~/.cache/folder-preview-bench
tools/bench-nautilus.sh                    # cold / warm / browse in an isolated Nautilus
BENCH_BASELINE=1 tools/bench-nautilus.sh   # the same browse with the feature's hook disabled
build/tests/folder-preview/bench-core DIR  # per-stage timings of the pipeline outside Nautilus
```

## Layout

| Path | What |
|---|---|
| `src/extension.c` | Module entry points Nautilus calls; fans out to every feature |
| `src/<feature>/` | One directory per feature, with its own `meson.build` |
| `src/folder-preview/folder_preview.c` | Feature set-up / tear-down |
| `src/folder-preview/info_provider.c` | Supported Nautilus hook (`NautilusInfoProvider`) |
| `src/folder-preview/workers.c` | Shared prioritised worker pool (max 2 threads) for all blocking work |
| `src/folder-preview/scanner.c` | Finds the first eligible images/videos/audio in a folder |
| `src/folder-preview/thumbs.c` | Finds or generates their thumbnails in the shared cache |
| `src/folder-preview/composite.c` | Builds the 2×2 frames |
| `src/folder-preview/cache.c` | Memory + disk cache of frames; the request pipeline |
| `src/folder-preview/folder_paintable.c` | Draws the theme folder icon with a frame on its front face |
| `src/folder-preview/view_hook.c` | **Unsupported**: hooks into Nautilus' private grid widgets |
| `src/folder-preview/config.h` | Tunable constants |
| `tests/<feature>/` | GLib unit tests (`meson test`) |
| `tools/` | Safety, smoke-test, benchmark and packaging scripts |
| `packaging/` | Fedora RPM spec |
| `PLAN.md` | Design, rules, decisions and measured results for every feature |

## Adding a feature

1. Plan it in `PLAN.md` (§6 is the template), then create `src/<feature>/` with a `meson.build` that appends its static libraries to `feature_libs` and
   its dependencies to `feature_deps`, and sets its own `G_LOG_DOMAIN`.
2. Expose `initialize (GTypeModule *)`, `shutdown (void)` and `list_types (int *)` and add them to the
   `features[]` table in `src/extension.c`.
3. Add `subdir()` calls in the top-level `meson.build` for the sources and the tests.
