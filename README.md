# nautilus-utils

A set of utilities to extend the functionality of the Nautilus file manager (GNOME Files), shipped as a
single native extension module: `libnautilus-utils.so`.

Tested on Fedora 45, Nautilus 51.1, GTK 4.24.

## Features

### Folder preview

Dolphin-style folder thumbnails: each folder icon in the grid view shows its first images, videos and album
covers, and cycles through the next ones while the pointer hovers over it. Two layouts:

- **Grid**: tilted photos on the folder's front (1 → one large photo, 2 → side by side, 3 → two on top and
  one below, 4+ → quarters).
- **Stacked**: up to three rounded cards stacked inside the folder, the first picture in front. Hovering
  brings the next picture to the front (or the next three, see *Pictures per Step*). Optionally with your
  own **folder icon** SVG and a **front overlay** SVG drawn over the cards, so the pictures look like they
  sit inside the folder. Both SVGs are drawn at the size of the icon: give them the same canvas and place
  the overlay where it belongs inside its SVG. A **depth effect** (Blur, Transparency or Darken) makes the
  cards further back look further away; the front card always stays upright. The stack's side padding
  (8 px by default, at the 64 × 64 size, scaled with the zoom), its position (Top, Centre, Bottom, plus a fine-tuning slider), a bottom fade and
  rounded top/bottom corners that clip all pictures can be set too.
  Folders without previews keep the theme's folder icon.

Border (on/off and colour), drop shadow (it follows the picture's shape, so transparent logos get a soft
outline shadow, not a box), keep aspect ratio (off = pictures fill their space, cropped), corner radius,
random rotation and cycling on hover can be changed for both layouts.
Only folders with the theme's plain folder icon get previews: special folders (Pictures, Downloads, …,
`folder-*` icons) and folders with a custom icon keep their own icon.

Known limitations:

- **Nautilus updates may switch the previews off.** Nautilus has no API for custom icons or hover, so the
  feature uses Nautilus' private grid widgets (verified on Nautilus 51.1). If an update changes them, it
  logs one warning ("folder previews disabled: …") and Nautilus keeps working with plain folder icons.
- **Grid view only.** List view shows plain icons.
- **Local folders only** (no network, MTP, etc.).
- A preview is keyed by the folder's modification time. Changes *inside* a folder you are looking at are
  picked up live; editing a file in place (which doesn't change the folder's time) shows up after the
  folder itself changes or the cache is cleared.
- Custom folder SVGs need GTK 4.22 or newer (GTK's own SVG renderer; animations and external files are
  ignored). An unreadable file counts as not set.
- Audio cover art needs `gst-thumbnailers`; audio without cover art is skipped.
- With **nautilus-python** extensions installed, Nautilus can stall for up to ~0.5 s while scrolling large
  folders whether or not this extension is installed (Python's global interpreter lock).

Clear its frame cache with `rm -rf ~/.cache/nautilus-utils/folder-preview` (rebuilt on demand). Thumbnails
themselves live in the shared `~/.cache/thumbnails`.

### Sidebar drives

Turns the drive rows of the sidebar into rows with a usage bar and free space, like the Computer view:

```
  ──────────────────────────────────
  Fedora Linux 45 (Workstation Edition)
  ███░░░░░░░░░░░░░░░░░░░░
  445.7 GB free of 497.3 GB
  Personal Drive                   ⏏
  ████░░░░░░░░░░░░░░░░░░░
  803.2 GB free of 1.0 TB
  User Drive                       ⏏
  ░░░░░░░░░░░░░░░░░░░░░░░
  500.0 GB free of 500.1 GB
  ──────────────────────────────────
  mediadrive on media-server.local ⏏
  ██████░░░░░░░░░░░░░░░░░
  1.2 TB free of 3.6 TB
```

- **OS drive**: a row named after the installed OS (`PRETTY_NAME` from os-release) directly above the other
  drives; clicking it opens `/`, right-clicking it opens Files' own drive menu (Open, Open in New Tab,
  Open in New Window, Properties). Nautilus' own "Computer" row is left untouched.
- **Local drives**: Nautilus' drive rows get a bar and "X free of Y" under their name; eject, right-click
  menu and drag-and-drop work as before.
- **Network shares** (mounted ones, e.g. SMB/SFTP, or NFS/CIFS mounted by the system) are listed below a
  separator under the local drives, with a bar when the server reports its size. Click opens the share,
  the eject button unmounts it. Their right-click menu and dropping files onto them are not available
  (Nautilus sorts shares above the local drives and its rows can't be re-sorted, so these are our own rows).
- Bars use the accent colour by default (another colour or a gradient can be chosen in the settings), the
  warning colour from 90 % full and the error colour from 95 %; light, dark and high-contrast themes are
  followed. Bars, size labels and a sliding eject button are configurable (see *Settings*). Free space is refreshed when a window gets focus and every 60 s.
  A slow or unreachable server never blocks Files: its size query gives up after 3 s and keeps the last
  values.

Known limitations:

- **Nautilus updates may switch the feature off.** It relies on Nautilus' private sidebar widgets (verified
  on Nautilus 51.1). If they change, it logs one warning ("sidebar drives disabled: …") and the sidebar
  stays as stock Nautilus.
- Only mounted network shares are listed (mount them via *Network*).

### Folder views

Every folder remembers its own **view type** (grid or list) and **icon size**, like Windows File Explorer.
Switch a folder to list view (or zoom it with Ctrl + / Ctrl − / Ctrl + scroll) and only that folder
changes; other folders, tabs and windows keep theirs. Each folder keeps one icon size per view type, so
switching it back to the grid restores its grid size.

- A folder you never changed uses the **default** view and icon size. The default is copied once from Files'
  own setting the first time the feature runs, and changing a folder never changes it.
- Views are saved in the folder's GIO metadata (where Files keeps each folder's sort order), so they survive
  restarts and follow folders you rename or move in Files. This works on local and NTFS drives, read-only
  folders, Trash and Recent.
- **Starred**, **Other Locations** and Files' global search always use the default (they can't store a
  view). A search inside a folder keeps that folder's view. The Network view is left alone.
- **Reset All Folder Views…** (in the settings) makes every folder use the default again.
- Switching off puts Files' own default back, so Files behaves as before.

Known limitations:

- **Nautilus updates may switch the feature off.** It relies on Nautilus' private tab and view widgets
  (verified on Nautilus 51.1). If they change, it logs one warning ("folder views disabled: …") and Files
  keeps its normal global view.
- The saved views of a folder's subfolders, siblings and parent are read in the background after it loads,
  so the next folder's view is known when you click. When two folders in a row have different view types,
  the old folder stays on screen as a still picture while the new one loads and appears at once when its
  files are in. The switch underneath is Files' own view rebuild (what Ctrl+1 / Ctrl+2 costs, 0.1–0.3 s),
  so such a folder appears about 0.1 s later than in stock Files; folders with the same view type open as
  fast as stock.
- Network shares (SMB, SFTP) haven't been tested yet; reading a saved view never blocks Files.

### Settings

Files' main menu (☰) gets a **Nautilus Utils Settings** entry above *Preferences*. It opens a dialog with
one tab per feature; every tab can switch its feature on or off. Changes only take effect when you press
**Apply**; closing the dialog without Apply discards them.

- **Folder Preview**: show previews on folder icons (on/off), layout (Grid or Stacked), cycle on hover,
  pictures per step and depth effect (Stacked), border, border colour, shadow, keep aspect ratio, corner
  radius and random rotation; for the Stacked layout the stack's position, vertical offset, bottom fade and
  top/bottom corner radius, and
  a custom folder icon and front overlay (SVG files; the path is stored, so keep the files where they are).
- **Sidebar Drives**: enhanced drive rows (on/off), bar colour (accent colour, a custom colour, or a
  two-colour gradient), show/hide the usage bars and the size labels, and slide the eject button in only
  on hover.
- **Folder Views**: remember the view per folder (on/off), and **Reset All Folder Views…**, which asks for
  confirmation and acts right away (it isn't a setting waiting for Apply).

The settings are stored with GSettings (schema `io.github.zyrotec.NautilusUtils`, inspect with
`gsettings list-recursively io.github.zyrotec.NautilusUtils`). If the schema isn't installed, the features
run with their defaults, the dialog says so, and Files logs one warning.

Custom colours and the gradient replace the warning/error colours of nearly full drives; with the accent
colour a drive turns the warning colour from 90 % and the error colour from 95 % full.

With **Slide In on Hover**, the eject buttons stay hidden and the bars use the full row width (like the OS
drive); hovering a drive (or giving it keyboard focus) slides its eject button in, and the row's text and bar
fade out underneath it.

Right-clicking the OS drive (or the Menu key) opens Files' own drive menu for it: Open, Open in New Tab,
Open in New Window and Properties.

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

Install the settings schema into your home folder (repeat when `data/*.gschema.xml` changes):

```
mkdir -p ~/.local/share/glib-2.0/schemas
cp data/io.github.zyrotec.NautilusUtils.gschema.xml ~/.local/share/glib-2.0/schemas/
glib-compile-schemas ~/.local/share/glib-2.0/schemas
```

Nautilus keeps running in the background, so a rebuild only takes effect after it quits:

```
nautilus -q
G_MESSAGES_DEBUG="nautilus-utils folder-preview sidebar-drives settings" nautilus    # with log messages
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
# dev schema, if installed:
rm ~/.local/share/glib-2.0/schemas/io.github.zyrotec.NautilusUtils.gschema.xml
glib-compile-schemas ~/.local/share/glib-2.0/schemas
```

## Safety checks

```
tools/check-safety.sh                    # strict build, gcc -fanalyzer, scan-build, ASan+UBSan tests,
                                         # valgrind tests, coverage report. Non-zero exit = a finding.
tools/check-safety.sh asan               # run only some steps
tools/smoke-nautilus.sh DIR              # isolated real Nautilus with an ASan build
tools/smoke-nautilus.sh --auto 120 DIR   # unattended: scripted scroll/zoom/hover + sidebar steps + self-checks
SMOKE_FAKE_NETWORK="User Drive" tools/smoke-nautilus.sh --auto 120 DIR   # also exercise the network list
SMOKE_TOGGLES=5 tools/smoke-nautilus.sh --auto 60 DIR   # fewer settings on/off rounds (default 25)
```

Logs land in `safety-logs/`. The smoke test also drives the settings (colours, show/hide, sliding eject,
feature on/off) through an in-memory settings store, and runs with its own data directory (so the folder views
it saves stay in its own gvfs metadata), so it never changes your own settings or folder views.

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
| `src/folder-preview/composite.c` | Builds the frames (grid or stacked cards, style options) |
| `src/folder-preview/cache.c` | Memory + disk cache of frames; the request pipeline |
| `src/folder-preview/folder_paintable.c` | Draws the folder icon (or custom SVG), the frame and the overlay |
| `src/folder-preview/icon_art.c` | Custom folder SVGs → textures at exact sizes; the folder's opaque body |
| `src/folder-preview/view_hook.c` | **Unsupported**: hooks into Nautilus' private grid widgets |
| `src/folder-preview/config.h` | Tunable constants |
| `src/sidebar-drives/sidebar_drives.c` | Feature set-up / tear-down |
| `src/sidebar-drives/drive_info.c` | OS name, local/network classification, async size queries with timeout, usage text |
| `src/sidebar-drives/sidebar_hook.c` | **Unsupported**: hooks into Nautilus' private sidebar widgets (drive rows, OS row, network list) |
| `src/sidebar-drives/bar_style.c` | Bar colours from the settings, as CSS |
| `src/sidebar-drives/fade_bin.c` | Container that fades its content out under the sliding eject button |
| `src/sidebar-drives/config.h` | Tunable constants |
| `src/folder-views/folder_views.c` | Feature set-up / tear-down, follows the settings |
| `src/folder-views/fv_state.c` | Saved-value format, location kinds, zoom levels, in-memory cache |
| `src/folder-views/view_mode_hook.c` | **Unsupported**: hooks into Nautilus' private tabs and views (switch view, zoom, detect the user's changes) |
| `src/folder-views/config.h` | Tunable constants |
| `src/common/nu_settings.c` | Shared settings service (GSettings snapshot + change notifications) |
| `src/settings/settings_dialog.c` | The "Nautilus Utils Settings" dialog |
| `src/settings/menu_hook.c` | **Unsupported**: adds the entry to Nautilus' private main menu |
| `data/*.gschema.xml` | GSettings schema |
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
