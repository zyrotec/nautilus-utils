# PLAN — nautilus-utils

> Planning doc for Claude Code. Read this file in full before writing any code.
> Code comments cite it as `PLAN.md §x.y`. Keep section numbers stable; append rather than renumber.

---

## 1. Goal

One native Nautilus (GNOME Files) extension, `libnautilus-utils.so`, that adds features GNOME Files lacks.
Every feature must be **fast** (Nautilus never stutters, even in folders with 10k+ entries), **memory-safe**
(proven by tooling, §4.3), and must **degrade gracefully**: if Nautilus changes underneath us, the feature
switches itself off with one warning and Nautilus keeps working.

Target: Fedora (current release), GNOME / Nautilus as shipped, GTK4, `libnautilus-extension-4`.
Tested: Fedora 45, Nautilus 51.1, GTK 4.24.

---

## 2. Hard constraints & ground truth

- **Not a GNOME Shell extension.** Shell extensions run in `gnome-shell` and cannot reach widgets inside the
  Nautilus window. Nautilus loads only its own extensions, in **C** (`libnautilus-extension-4`) or Python
  (`nautilus-python`). We use **C** for performance; no Python (it also causes main-loop stalls through the
  GIL, measured in §6.5).
- **The public extension API is small:** `NautilusInfoProvider`, `NautilusMenuProvider`,
  `NautilusColumnProvider`, `NautilusPropertiesModelProvider`. None of them draw icons or receive pointer
  events. Prefer these whenever a feature can be built on them.
- Extensions are loaded **in-process**, so we *can* reach Nautilus' GTK4 widget tree
  (`gtk_window_get_toplevels()`). That is **unsupported and private**: class names and structure change
  between releases. Each feature keeps all such code in **one file** (e.g. `folder-preview/view_hook.c`),
  looks types up by name (`g_type_from_name`), reads properties with `g_object_get` (no private headers),
  and disables itself with one warning if the expected widgets aren't there.
- Nautilus only loads extensions from the compiled-in `extensiondir`
  (`/usr/lib64/nautilus/extensions-4`); there is no per-user folder.
- **Never invent an API.** Before calling any `nautilus_*`, `gnome_desktop_*`, `g_*`, `gdk_*` or `gtk_*`
  function, confirm it exists in the installed headers (`/usr/include/nautilus/`,
  `/usr/include/gnome-desktop-4.0/`, `/usr/include/glib-2.0/`, `/usr/include/gtk-4.0/`). Example: GIO has
  no synchronous `g_file_enumerator_next_files()`.

---

## 3. Architecture

```
libnautilus-utils.so
├─ src/extension.c          nautilus_module_initialize / list_types / shutdown
│                           → calls every feature in features[] (init in order, shutdown in reverse)
│                           → concatenates their provider GTypes for Nautilus
├─ src/folder-preview/      feature 1 (§6)
└─ src/<next-feature>/      …
```

Per-feature conventions:

| What | Convention |
|---|---|
| Sources | `src/<feature>/`, own `meson.build` appending to `feature_libs` / `feature_deps` |
| Entry points | `<prefix>_feature_initialize (GTypeModule *)`, `_shutdown (void)`, `_list_types (int *)`, registered in `features[]` in `src/extension.c` |
| Symbols | Static libraries with `gnu_symbol_visibility: 'hidden'`; the module exports only the 3 Nautilus entry points |
| Log domain | `G_LOG_DOMAIN="<feature>"` (module glue uses `nautilus-utils`); debug with `G_MESSAGES_DEBUG="nautilus-utils <feature>"` (space-separated) |
| Cache | `$XDG_CACHE_HOME/nautilus-utils/<feature>/` |
| Tests | `tests/<feature>/`, own `meson.build`, GLib `g_test` (TAP) |
| Prefix | Short, unique C prefix per feature (`fp_` = folder preview) |

Shared infrastructure (e.g. a worker pool) may move to `src/common/` once a second feature needs it — not
before.

---

## 4. Engineering rules (every feature)

### 4.1 Code rules

- C11, GLib/GObject style. `g_autoptr`, `g_autofree`, `g_auto` for every local allocation/reference; manual
  `g_free`/`g_object_unref` only where ownership is transferred, with a comment.
- `g_steal_pointer()` to transfer ownership. Never return a borrowed pointer without saying so.
- `g_clear_object()`, `g_clear_pointer()`, `g_clear_handle_id()`, `g_clear_signal_handler()` when releasing
  struct fields, so pointers are never left dangling.
- No fixed-size buffers, no `strcpy`/`sprintf`/`strcat`; use `GString`, `g_strdup_printf`, `g_build_filename`.
- Every async call carries a `GCancellable`; callbacks check cancellation before touching anything that may
  have been freed, and hold a ref on every object they use.
- Code touching Nautilus' widget tree (highest crash risk, since cells are recycled while scrolling): never
  store raw widget pointers (use `GWeakRef` / `g_signal_connect_object()`); remove timers and handlers when a
  cell is unbound, recycled or destroyed; disconnect everything and free all state on shutdown.
- **Ownership comments:** every function that allocates, returns, stores or frees memory gets a one-line
  plain-English comment; every struct field holding a pointer says who frees it and when.

### 4.2 Testability

- Keep Nautilus types out of the core logic of a feature so it compiles into standalone test binaries
  (where sanitizers work cleanly). Only the entry/hook files may include `nautilus-extension.h`.
- Tests use `g_test`, registered in Meson. Include stress tests (repeat → cancel → redo cycles, caches filled
  past their limits, thousands of create/destroy cycles): leaks show up under repetition.
- Tests keep temporary files under the build dir, not `/tmp` (gnome-desktop's thumbnailer sandbox misbehaves
  there, §6.4), and pin `LC_ALL=en_US.utf8` for deterministic collation.

### 4.3 Safety tooling

`tools/check-safety.sh` runs everything and exits non-zero on **any** finding:

1. **Strict compile:** `-Wall -Wextra -Werror -Wformat=2 -Wnull-dereference -Wshadow -Wmissing-prototypes
   -Wstrict-prototypes`.
2. **GCC `-fanalyzer`** build.
3. **Clang static analyzer** (`scan-build --status-bugs`).
4. **ASan + LSan + UBSan** over `meson test`.
5. **Valgrind memcheck** over `meson test` (GLib's `/usr/share/glib-2.0/valgrind/glib.supp`), definite leaks
   fail.
6. **Coverage** (informational): lines never run by tests. Both static analyzers are weak on GObject code
   (`-fanalyzer` loses track after GObject type checks; Clang gives up around `g_autofree`), so steps 4–5
   are the real safety net and only cover lines the tests execute — keep coverage near 100 %.

Plus `tools/smoke-nautilus.sh`: a real, isolated Nautilus (own D-Bus session) with an ASan build of the
module. It fails on any sanitizer report that mentions our code or `libnautilus-utils.so` and ignores the
~5 MB of third-party exit-time leaks. Suppression files may only contain **third-party** entries, each with a
comment; never suppress a finding in our code.

The project owner doesn't read C: a feature is **not done** until `tools/check-safety.sh` passes with zero
findings and the smoke test is clean.

### 4.4 Performance rules

1. **The main thread only does** cheap lookups, queueing and widget/paintable updates. All I/O, decoding,
   scaling and drawing run in worker threads.
2. Nautilus calls the providers on the main thread: they must never block.
3. Measure before optimising, with the main-loop latency probe (a 16 ms timer that logs when it fires late)
   and per-stage benchmarks. Record numbers in this file (§6.5 is the model).
4. Writes to caches: temp file + rename, **no fsync** (fsync on the development drive — NTFS via `ntfs3` —
   blocked for > 300 s; a damaged cache file is detected, deleted and rebuilt instead).

### 4.5 Workflow

- Each feature is planned as a section in this file (behaviour spec, rules, constants, phases with
  acceptance criteria) **before** code is written, then built phase by phase.
- After each phase: run the tests and `tools/check-safety.sh`, record what was verified/changed and the safety
  summary in the feature's section, then stop for review.
- Design deviations get a numbered entry (`D1`, `D2`, …) in the feature's decision list.

---

## 5. Features & roadmap

| # | Feature | Dir | Status |
|---|---|---|---|
| 1 | Folder preview — Dolphin-style thumbnails on folder icons, cycling on hover; v2: stacked layout, custom folder SVGs, thumbnail style (§11) | `src/folder-preview/` | **v2 done** (2026-11-13), pending the owner's final check |
| 2 | Sidebar drives — usage bars on drive rows, OS drive named after the OS, network shares below a separator | `src/sidebar-drives/` | **v1 done** (2026-11-12), pending the owner's final check |
| 3 | Settings — "Nautilus Utils Settings" window from the main menu (per-feature on/off, bar colours, hide bars/labels, sliding eject), OS drive context menu | `src/settings/`, `src/common/` | **v1 done** (2026-11-12), pending the owner's final check |
| 4 | Folder views — every folder remembers its own view type and icon size, like Windows File Explorer | `src/folder-views/` | **v1 done** (2026-11-12), pending the owner's final check |
| 5 | Folder icons by name — every folder with a given name (e.g. `node_modules`) gets a chosen icon, as Files' own custom icon (§12) | `src/folder-icons/` | in progress (Phase 0 done 2026-11-14) |

Open items:

- Folder preview: list view support; RPM changelog author is a placeholder; `rpmbuild` itself not yet run
  (`rpm-build` not installed — emulated, §6.5 Phase 7).

---

## 6. Feature 1 — Folder preview

Draws the first images / videos / album covers of each folder on its icon in the grid view and cycles
through the next ones while the pointer hovers, like Dolphin. Prefix `fp_`, log domain `folder-preview`.

### 6.1 Behaviour

#### 6.1.1 Which files count, and which are "first"

- Direct children only (no recursion). Skip hidden files (dot files + the folder's `.hidden` list), backup
  files (trailing `~`) and subdirectories.
- Classify by `g_content_type_guess (name)` (no content sniffing, no `stat`):
  `image/*` and `video/*` → eligible; `audio/*` → eligible **only if** a thumbnail can be produced (embedded
  cover art); everything else → excluded.
- Order: natural filename order (`g_utf8_collate_key_for_filename()`, matches Nautilus' "by name").
- "First" = the first `MAX_PREVIEW_FILES` eligible files in that order. No eligible files → plain icon.
- Only native `file://` locations (no network/MTP).

#### 6.1.2 Static display

- Frame 0 (first 4 files) is drawn on the front face of the theme folder icon: a 0.76 × 0.52 rectangle centred
  at (0.50, 0.615) of the icon, at the current zoom size.
- Dolphin-style photos: each thumbnail keeps its aspect ratio, has a white border and soft shadow, and is
  tilted up to ±`FRAME_MAX_TILT_DEG`. The tilt is a hash of the file URI, so it is identical across redraws,
  zoom levels and restarts.
- Layout by count (per frame): 1 → whole face; 2 → left/right halves; 3 → two on top + one centred below;
  4 → quarters. Photos fill up to 96 % of their section, centred; a transparent `FRAME_PAD` margin lets
  tilted corners poke slightly past the face.

#### 6.1.3 Hover

- Pointer enters the icon → every `HOVER_FRAME_MS` show the next frame (files 5–8, 9–12, …), wrapping to
  frame 0. Hover frames load lazily on first hover.
- ≤ 4 eligible files → nothing cycles. Pointer leaves → frame 0 immediately.
- Exactly one hover timer may be active in the whole process.

#### 6.1.4 Invalidation

- Cache key = folder URI + `time::modified` + `time::modified-usec`.
- Folders currently **on screen** are watched with a `GFileMonitor` (created on map, removed on
  unmap/rebind/destroy, events debounced 600 ms) — Nautilus does not re-call the info provider when a
  child is added (D15). Off-screen folders are never monitored.

### 6.2 Performance rules

1. Nothing on the main thread except hash lookups, queueing and setting the icon texture (§4.4).
2. The info provider returns `NAUTILUS_OPERATION_COMPLETE` immediately; it only does bookkeeping (D3).
3. Reuse Nautilus' freedesktop thumbnails first (`thumbnail::path`, `is-valid`, `thumbnailing-failed`),
   queried **only for the ≤ `SCAN_WINDOW` kept files** (D6).
4. Missing thumbnails are generated with `GnomeDesktopThumbnailFactory` into the shared
   `~/.cache/thumbnails/`, so Nautilus benefits too. Failed files get a failure record and are skipped.
5. Enumerate only `standard::name,display-name,type` (no `stat` per entry), synchronously inside a worker,
   checking cancellation every `ENUM_BATCH` entries (D7); keep only the best `SCAN_WINDOW` candidates.
6. Priority: folders whose icon is **mapped** are processed first; requests for icons scrolled out of view
   are cancelled.
7. Decode thumbnails with `gdk_texture_new_from_filename` (thread-safe, ~5× faster than gdk-pixbuf here, D9)
   and draw frames with cairo at device size.
8. Composite once per (folder, frame, size); keep `GdkTexture`s in a memory LRU bounded by `LRU_FOLDERS`
   **and** `CACHE_MAX_BYTES` (D13). Off-screen icons don't pin their textures.
9. Disk cache: `~/.cache/nautilus-utils/folder-preview/<CACHE_DISK_VERSION>/<sha1(uri)>/<sec>.<usec>-<px>-<frames>.png`,
   plus empty `<sec>.<usec>-none` markers for folders with nothing to preview (D11, D12). Frame 0 appears
   instantly on the next launch.
10. Workers: a dedicated pool of `WORKER_THREADS` at nice 19; when the queue empties, `malloc_trim(0)` at most
    every 2 s returns decoder memory.

### 6.3 Constants (`src/folder-preview/config.h`)

| Name | Value | Meaning |
|---|---|---|
| `PREVIEW_SLOTS` | 4 | Thumbnails shown at once (2×2) |
| `MAX_PREVIEW_FILES` | 16 | Max files per folder → up to 4 hover frames |
| `SCAN_WINDOW` | 32 | Candidates a scan returns (room to skip audio without art) |
| `HOVER_FRAME_MS` | 700 | Time each frame is shown while hovering |
| `WORKER_THREADS` | 2 | Max concurrent scan/thumbnail jobs |
| `ENUM_BATCH` | 200 | Entries enumerated between cancellation checks |
| `LRU_FOLDERS` | 256 | Folders kept in memory |
| `CACHE_MAX_BYTES` | 48 MB | Memory budget for frame textures |
| `FRAME_FACE_*` | 0.76 × 0.52 @ (0.50, 0.615) | Front-face rectangle on the folder icon |
| `FRAME_MAX_TILT_DEG` | 8 | Max photo tilt either way |
| `FRAME_PAD` | 0.10 | Transparent margin around the face in the frame texture |
| `CACHE_DISK_VERSION` | `v3` | Bump when the look of frames changes |

### 6.4 Decisions & findings

**Nautilus internals (verified on 51.1 / GTK 4.24.1)** — the grid is
`NautilusWindow > … > NautilusFilesView > … > NautilusGridView > … > GtkGridView`. One cell:
`GtkListItemWidget > NautilusGridCell (NautilusViewCell; props view, item, icon-size, position)` containing
`NautilusImage` (props `source: GFile`, `size`, `fallback: GdkPaintable`) — a custom widget, not a
`GtkPicture`. Cell → file: `NautilusGridCell.item` → `NautilusViewItem.file` (implements
`NautilusFileInfo`) → `nautilus_file_info_get_uri()`. GtkGridView keeps ~640 cells alive in a 5000-item
folder while ~30 are mapped. Zoom sizes 64 / 96 / 168 / 256 px. List view uses `GtkColumnView` (not
targeted).

| # | Decision |
|---|---|
| D1 | The 4.x info-provider method is `update_file_info` (not `update_file_info_full`). |
| D2 | No custom audio thumbnailer: Fedora's `gst-audio-thumbnailer` (`gst-thumbnailers`) extracts embedded cover art and fails without it. |
| D3 | `update_file_info` runs for **every** folder up front, so it is not a visibility signal. Scans are requested by `view_hook.c` for **mapped** cells; `info_provider.c` does bookkeeping only. |
| D4 | Icon override = `NautilusImage` `source = NULL` + `fallback = <frame texture>`, re-applied after `notify::source/fallback/size` (Nautilus resets them on bind, rebind and every zoom). |
| D5 | `NautilusFileInfo` has no mtime getter; the scanner reads the folder mtime itself. |
| D6 | No `thumbnail::*` attributes in the enumeration (10k files: 48–57 ms without vs 156–231 ms with); queried only for kept files. |
| D7 | Sync `g_file_enumerator_next_file()` inside a worker (no sync `next_files` exists; async would run filtering on the main thread). |
| D8 | Scan window (32) > 16, so audio without art can be skipped without rescanning; re-enumerate with a 4× bound if still short. |
| D9 | Decode with GTK texture loaders, not gdk-pixbuf (0.8–0.9 ms vs 3.2–4.6 ms per 256 px thumbnail). |
| D10 | The frame contains only the photos; `folder_paintable.c` draws it over the theme folder icon. |
| D11 | Disk cache: one directory per folder (cheap stale-file cleanup), frame count in the file name. |
| D12 | "Nothing to preview" markers, so folders without media aren't re-enumerated on every launch. |
| D13 | Memory LRU has a byte budget as well as a folder count (a 256 px HiDPI frame is ~1 MB). |
| D14 | No fsync on cache writes (§4.4 rule 4). |
| D15 | `GFileMonitor` on on-screen folders only (Nautilus didn't re-call the provider when an image was added). |
| D16 | After a zoom, the old size is shown scaled until the new frame is built (otherwise ~1–2 s of plain icons). |
| D17 | Dolphin look (owner request): aspect ratio kept, tilted photos, layout by count (§6.1.2). |
| D18 | Scanner computes hidden/backup/content type from the name itself: requesting them made GLib `stat()` every entry (2 000 entries: 2.6 → 9.4 ms). An empty `x.png` becomes a candidate whose thumbnail fails — same end result. |

**Other findings**

- `g_utf8_collate_key_for_filename` follows `LC_COLLATE` only after `setlocale (LC_ALL, "")` (Nautilus/GTK
  call it; tests must too).
- With the thumbnail cache under `/tmp`, gnome-desktop 51.0's bubblewrap sandbox fails to clean up its work
  dir and prints garbage in its warning (its bug, not ours). Real users' cache is `~/.cache`.
- A thumbnailer failure record is honoured by gnome-desktop for both us and Nautilus, so a file is excluded
  until it changes (`is-valid` goes FALSE and it comes back).
- Thread priority: Linux `setpriority (PRIO_PROCESS, 0, …)` affects only the calling thread, so the pool is
  `exclusive` (GLib's shared threads, which Nautilus also uses, are never re-niced).

### 6.5 Phases & results (v1, all done)

| Phase | Acceptance | Result |
|---|---|---|
| 0 Spike | Widget-hook approach proven feasible | ✅ |
| 1 Skeleton | Module loads/unloads cleanly, logs folder URIs, safety script works | ✅ |
| 2 Scanner | Natural order, filtering, top-N; a 10k-file scan never blocks the main loop (16 ms probe) | ✅ |
| 3 Thumbnails | Mixed folder → only images, videos and mp3 with art, in natural order | ✅ |
| 4 Composite & cache | Second launch loads frame 0 from disk without decoding thumbnails | ✅ |
| 5 View hook | Previews, hover cycling, zoom redraw, no mix-ups while scrolling fast; graceful failure | ✅ (`verify`: 0 wrong) |
| 6 Performance | See below | ✅ |
| 7 Packaging | RPM spec, tarball, README | ✅ (build emulated with Fedora optflags + `-Werror`: 0 warnings) |

Phase 6 targets (benchmark tree: 500 folders, ~517 000 files; optimised build; 8 CPUs; thumbnails cached):

| Target | Result |
|---|---|
| No main-loop stall > 16 ms attributable to us | Max callback 0.6 ms; stall rate = Nautilus alone |
| Cold open: frame 0 for visible folders ≤ 1 s | 30 visible: p50 235 ms, max 679 ms |
| Warm reopen instant (disk cache) | p50 27 ms, max 44 ms |
| RSS growth after 1 000 folder previews < 100 MB | 51 MB (Nautilus + Python alone: ~52 MB) |
| No wrong previews | 0 in every run |

nautilus-python extensions, when installed, cause stalls up to ~0.5 s while scrolling large folders with or
without this extension (GIL).

---

## 7. Instructions for Claude Code

- Language: **C11**, GLib/GObject style. Build: Meson + Ninja, `-Werror` in dev builds.
- Follow §2–§4 for every feature. Keep unsupported Nautilus-internals code in one file per feature.
- Never block the main thread. If a function might do I/O, it runs in a worker.
- Verify every API against installed headers; record anything uncertain here instead of guessing.
- Never mark a phase done while `tools/check-safety.sh` reports anything, and never "fix" a finding by
  suppressing it in our own code.
- New feature: add its section to this file (§6 is the template) and its row to §5, get it approved, then
  build it phase by phase (§4.5).

---

## 8. Feature 2 — Sidebar drives

Turns the drive rows of the Nautilus sidebar into Dolphin/GNOME-Disks-style rows with a usage bar and free
space, names the OS drive after the installed OS, and lists connected network shares below a separator.
Prefix `sd_`, log domain `sidebar-drives`, branch `dev-side-bar-drives`.

### 8.1 Behaviour

#### 8.1.1 Layout

```
  …bookmarks (Documents, Music, …, Development)
  ───────────────────────────────────────────────  existing separator (unchanged)
  [icon] Fedora Linux 45 (Workstation Edition)        OS drive (no eject)
         ███░░░░░░░░░░░░░░░░░░
         445.7 GB free of 497.3 GB
  [icon] Personal Drive                          ⏏
         █████████████░░░░░░░
         803.2 GB free of 1.0 TB
  [icon] User Drive                              ⏏
         ░░░░░░░░░░░░░░░░░░░░
         500.0 GB free of 500.1 GB
  ───────────────────────────────────────────────  new separator, same look as the existing one
  [icon] share on nas.local                      ⏏    network shares (only if any are mounted)
         ██████░░░░░░░░░░░░░░
         1.2 TB free of 3.6 TB
```

- **Local drive rows** (every mount the sidebar already lists in its devices section): name on the first
  line, a thin usage bar below it, and `"<free> free of <size>"` as a caption below the bar. Eject button,
  icon, click, right-click menu and drag-and-drop behave exactly as before.
- **OS drive**: the mount holding `/`. Its label is the OS's pretty name
  (`g_get_os_info (G_OS_INFO_KEY_PRETTY_NAME)`, e.g. "Fedora Linux 45 (Workstation Edition)"), falling back
  to "Computer" if unavailable; ellipsised at the end if too long (tooltip shows the full name). It is the
  **first** drive row; clicking opens `/` (S5). No eject button. Nautilus has no row for `/`, so it is our
  own row (§8.6). Nautilus' own "Computer" row (Computer view) stays where it is, untouched.
- Order: OS drive, then the other local drives in Nautilus' order (Personal Drive, User Drive).
- **Network shares**: mounted network locations only (SMB, SFTP, NFS, WebDAV, …; i.e. mounts that are not
  native/local). They appear under a separator styled like the existing one; separator and section are
  hidden when nothing is mounted. Same row layout; usage bar and caption only when the server reports a
  size, otherwise just the name. Unmount button as Nautilus provides it.
- Sizes: `g_format_size()` (one decimal, SI units: "445.7 GB free of 497.3 GB").
- Bar colour: accent colour normally; warning colour at ≥ 90 % full, error colour at ≥ 95 %
  (`GtkLevelBar` offsets, theme colours only).

#### 8.1.2 Updating

- Rows update when Nautilus rebuilds the sidebar (mount/unmount, bookmarks changed) — the hook re-applies
  after every rebuild.
- Free space refreshes: when a window gains focus, after a file operation finishes in Nautilus (if a
  signal is available, Phase 0), and every `SD_REFRESH_S` while a window is visible. Never per keystroke or
  per file.
- Local queries: `g_file_query_filesystem_info_async` (`filesystem::size,free`); never on the main thread.
- Network queries: same call with a `GCancellable` and an `SD_NET_TIMEOUT_MS` timeout; a slow or dead
  server keeps its last known values (or name only) and is retried at the next refresh, never more than one
  query in flight per mount.

#### 8.1.3 Look & accessibility (GNOME HIG, libadwaita)

- Bar: `GtkLevelBar` in continuous mode, 4 px high, rounded, inset under the label (aligned with the text,
  not the icon), as in images 1–3. Caption: `.caption` + `.dim-label`. All colours from libadwaita CSS
  variables (`var(--accent-bg-color)`, `var(--warning-bg-color)`, `var(--error-bg-color)`) so light/dark
  and high-contrast themes work; GTK4 CSS syntax only.
- Rows get taller (two extra lines); the row height must stay consistent between drive rows.
- Accessibility: the row's accessible description is the caption text; the level bar has an accessible
  label ("Disk usage") and value. Keyboard navigation unchanged.

#### 8.1.4 Graceful failure

- All Nautilus-internals code lives in `src/sidebar-drives/sidebar_hook.c`. If `NautilusSidebar` /
  `NautilusSidebarRow` or the properties we rely on aren't found, log one warning and leave the sidebar
  untouched. Folder preview keeps working independently.

### 8.2 Rules

1. Main thread only: widget changes and cheap lookups (§4.4). Filesystem queries are async GIO calls.
2. Never keep raw pointers to sidebar rows (Nautilus destroys and recreates them on every rebuild): use
   `g_signal_connect_object` / `GWeakRef` and per-row data via `g_object_set_data_full`.
3. Never change Nautilus' own row actions; only add children/CSS and reorder/relabel as verified in Phase 0.
4. Size math and text formatting live in a Nautilus-free core (`drive_info.c`) with unit tests.

### 8.3 Constants (`src/sidebar-drives/config.h`)

| Name | Value | Meaning |
|---|---|---|
| `SD_REFRESH_S` | 60 | Periodic free-space refresh while a window is visible |
| `SD_NET_TIMEOUT_MS` | 3000 | Give up on a network size query after this |
| `SD_WARN_FRACTION` | 0.90 | Bar turns warning colour at this fill level |
| `SD_FULL_FRACTION` | 0.95 | Bar turns error colour at this fill level |
| `SD_BAR_HEIGHT_PX` | 4 | Usage bar height |

### 8.4 Decisions (owner, 2026-11-11)

| # | Decision |
|---|---|
| S1 | Network section lists **mounted shares only** (no saved-but-disconnected servers). |
| S2 | Network rows show usage when the server reports it, else name only. |
| S3 | Size text uses `g_format_size` (one decimal), as in image 3. |
| S4 | The OS drive is named after the OS (`PRETTY_NAME`), not "Computer". |
| S6 | Network shares = our own rows in our own list below a separator under the local drives; Nautilus' rows for them hidden. Click opens, eject goes through Nautilus' hidden row. No right-click menu / file drop on these rows (owner accepted, 2026-11-12). |
| S5 | OS drive = our own row directly above Personal Drive; clicking opens `/`. Nautilus' "Computer" row is **never altered** (owner correction 2026-11-11: "Computer" in the request only referred to the OS drive's name in the screenshot). |

### 8.5 Phases

| Phase | Work | Acceptance |
|---|---|---|
| 0 Spike | Throwaway probe (not built by Meson): dump the `NautilusSidebar` widget tree and `NautilusSidebarRow` properties (label, mount/volume/drive, uri, section type, eject button); find how rows map to mounts; whether `/` is listed; how the existing separator is drawn; what triggers rebuilds; whether rows can get extra children and be reordered without breaking activation/DnD/context menus. Record findings + screenshots here. | Every question answered; if the sidebar can't be modified safely, stop and report. |
| 1 Core | `drive_info.c`: OS name, mount classification (OS / local / network), async size query with timeout + cancellation, caption text, fill level → state. Unit tests incl. fake slow mounts. | Tests + `check-safety.sh` pass. |
| 2 Local rows | `sidebar_hook.c`: usage bar + caption on local drive rows, OS row first and renamed, re-apply on rebuild, CSS. | Matches images 1–3 in light and dark; activation, eject, DnD, context menu unchanged; graceful failure. |
| 3 Network | Separator + network section, timeout handling, unmount. | A mounted SMB/SFTP share appears below the separator; a dead server never stalls Nautilus (16 ms probe). |
| 4 Refresh & hardening | Focus/timer refresh, smoke-test coverage (extend `tools/smoke-nautilus.sh`), safety run. | `check-safety.sh` + smoke test PASS; no main-loop stall > 16 ms attributable to us. |
| 5 Docs | README feature section, screenshots. | — |

### 8.6 Phase 0 findings (2026-11-11, Nautilus 51.1 / GTK 4.24.1)

Method: throwaway probe module loaded into an isolated Nautilus (own D-Bus session, own `XDG_CONFIG_HOME`
with a copy of the bookmarks, so rebuilds can be triggered without touching the real ones). Not in the repo.

**Structure.** `NautilusWindow > … > AdwToolbarView > NautilusSidebar (css placessidebar) > GtkScrolledWindow
> GtkViewport > GtkBox.navigation-sidebar` containing:
1. `GtkListBox` #1 — only the **Computer** row (`uri computer:///`, opens Nautilus 51's Computer view, which
   already shows "Fedora Linux 45 (Workstation…)" with a usage bar = the owner's images 2–3).
2. a hidden `GtkSeparator`.
3. `GtkListBox` #2 — Home, Recent, Starred, Network, Trash, (hidden "New bookmark"), bookmarks, mounts.
   It has a **sort function** (by row type/section) and a **header function**: section separators are
   `GtkSeparator` row headers (on the first bookmark and the first mount row).

**Rows** (`NautilusSidebarRow < GtkListBoxRow`): properties `label` (writable, updates the UI),
`tooltip`, `start-icon`, `end-icon`, `ejectable`, `eject-tooltip`, `uri`, `file` (**read-only**),
`drive`/`volume`/`mount` (Personal/User Drive: `GProxyDrive`/`GProxyVolume`/`GProxyMount`),
`section-type` (`NautilusSidebarSectionType`: INVALID, DEFAULT_LOCATIONS, BOOKMARKS, CLOUD, MOUNTS),
`place-type` (`NautilusSidebarRowType`: INVALID, BUILT_IN, EXTERNAL_MOUNT, INTERNAL_MOUNT, NEW_BOOKMARK,
BOOKMARK, BOOKMARK_PLACEHOLDER), `order-index`, `sidebar`. Content: `GtkRevealer > GtkBox.horizontal >
[GtkImage start, GtkLabel, GtkImage end (hidden), GtkButton.sidebar-button eject, AdwSpinner]`.
`/` has **no** row (only the Computer view row). No section exists for network mounts yet (unverified until
a network share is mounted).

**Signals.** `NautilusSidebar`: `mount`, `unmount`, `drag-action-requested`, `drag-action-ask`,
`drag-perform-drop`. Rows: none of their own.

**Modifying rows.**
- Wrapping the row's `GtkLabel` in a vertical box with a `GtkLevelBar` + caption works: the row grows, icon
  and eject button stay aligned. ✅
- **Every rebuild** (e.g. a bookmark change) recreates **all** rows of list #2, so decorations must be
  re-applied after each rebuild (detect: rows without our marker). ✅ (expected, §8.1.2)
- Activation via `row-activated` on a real row works (moved Computer row opened the Computer view). ✅

**Event handling** (`gtk_widget_observe_controllers`): `NautilusSidebar` — `GtkGestureLongPress`;
list #2 — `GtkDropTarget` (formats `NautilusSidebarRow`, `GdkFileList`), `GtkEventControllerKey`,
`GtkGestureClick` (button 1), shortcuts; each Nautilus-created row — `GtkGestureDrag` (button 1, bookmark
reordering) and `GtkGestureClick` (all buttons: activation, middle-click, right-click menu); eject button —
its own click gesture. Controllers are added by Nautilus when it creates a row, so a row created by us has
**none**.

**Sorting & headers.** List #2's sort function orders mount rows by **label** (`order-index` has no effect
on them; values < 0 are rejected). The header function puts the section separator on the first row of the
mounts section, recomputed by `gtk_list_box_invalidate_headers`.

**OS drive row — chosen design (owner, S5; the first version hid the Computer row and opened the Computer
view — that was a misreading of the request and was reverted, see §8.7):**
- Nautilus' Computer row is left alone.
- Insert our own `NautilusSidebarRow` into list #2: `uri file:///`, `section-type MOUNTS`,
  `place-type INTERNAL_MOUNT`, `ejectable FALSE`, **`label ""`** so the sort puts it first among the
  mounts; Nautilus' `GtkLabel` hidden and our own label shows the OS name (tooltip + accessible label too);
  `activatable FALSE` so the list never hands it to Nautilus' activation code.
- Clicks/Enter on our row (our own gesture + key controller, claiming the event) open `/` via Nautilus'
  `slot.open-location` action (string URI; resolved by the window for its active slot). ✅ (Probe note:
  emitting `row-activated` on a row that isn't selected makes Nautilus log
  `g_object_get: assertion 'G_IS_OBJECT (object)' failed`.)
- Result: separator above our row, then Personal Drive, User Drive; survives rebuilds by re-applying. ✅
- Selection highlight: see Phase 2 results (§8.7) — driven by the window's real location.
- Rejected: moving Nautilus' Computer row (Nautilus re-adds it on rebuild); our row with `file` (read-only);
  forwarding clicks to the Computer row (opens the Computer view, not `/`).

**Still to verify** (need real input or a real mount — Phase 2/3 acceptance, manual by the owner):
right-click and drag-and-drop onto our row (our row's gesture claims clicks; a refusing `GtkDropTarget` on
it), and how mounted **network shares** appear (section/place type, separator) — requires a mounted share;
**deferred to Phase 3** by the owner (tested with a share the owner mounts).

**Phase 0 status: done** (2026-11-11), network-share item deferred to Phase 3.

### 8.7 Phase results

**Phase 1 — Core (done 2026-11-11).** `src/sidebar-drives/drive_info.c` (Nautilus- and GTK-free):
`sd_os_name` (`g_get_os_info` PRETTY_NAME, fallback "Computer"), `sd_classify` (network = not native **or**
`filesystem::remote`, which also covers kernel NFS/CIFS mounts), `sd_fill_fraction`/`sd_fill_state`
(thresholds §8.3), `sd_usage_text` (`g_format_size`: no-break space before "GB"/"TB", plain space before
"bytes"), and `sd_query_usage_async` (`filesystem::size,free,type,remote`). The timeout returns
`G_IO_ERROR_TIMED_OUT` on time **even if the backend ignores cancellation** (a dead server); the late result is
dropped; the caller's cancellable gives `G_IO_ERROR_CANCELLED`.
Tests: `tests/sidebar-drives/test-drive-info.c`, 12 tests, incl. a fake slow `GFile` (honouring or ignoring
cancellation) and a 300-round stress test. Safety: strict, `-fanalyzer`, scan-build, ASan+UBSan, valgrind
all PASS; coverage 95.9 % (never run: the "Computer" fallback, and a defensive timer cleanup).
Lesson: tests that run under valgrind must not depend on tight wall-clock limits (first run timed out a real
query under valgrind load; the real-query test now has no timeout and timing checks have wide margins).

**Phase 2 — Local rows (done 2026-11-11, pending the owner's manual checks).** `sidebar_hook.c` +
`sidebar_drives.c`, registered in `src/extension.c`. Discovery: emission hook on `GtkWidget::map` (sees the
sidebar and every row Nautilus recreates), `apply()` coalesced in a high-priority idle and idempotent.
Drive rows: Nautilus' label moved into a vertical box with a `GtkLevelBar` (GTK's low/high/full offsets
removed; our `.sd-warning`/`.sd-full` classes) and a `.caption.dim-label`; size queried with
`sd_query_usage_async`. OS row as designed in §8.6 (own click/key controllers opening `/` via `slot.open-location`,
refusing drop target). CSS via one `GtkCssProvider` per display, libadwaita variables only.

Findings while building it (Nautilus 51.1):
- Nautilus keeps **one selected row across all its lists**: selecting our row makes it unselect the Computer
  row, so the highlight can't be mirrored from that row's selection.
- `NautilusSidebar:location` is only set by Nautilus when it activates a row and becomes NULL after a
  rebuild; while it is NULL Nautilus **unselects any row we select**. The window's real location is
  `NautilusWindow:active-slot` → `NautilusWindowSlot:location` (notifies on change).
- Design: our row is highlighted exactly while the active slot's location is `/`; after a rebuild we
  first give the sidebar its location back, then select; plus one re-check 150 ms after a rebuild
  (Nautilus clears the selection in several steps).
- The debug screenshot only works while a folder grid is shown (Nautilus 51's Home is not a grid).

Verified in an isolated Nautilus with the debug driver (`sd-dump`, `sd-click-os`, `sd-open-home`):
layout and separator; Computer row visible and untouched; click → `/` + highlight; Home → highlight cleared; highlight kept across
rebuilds; no wrong navigation; light and dark theme with accent colour; `SD_DEBUG_BREAK=types|props` → one
warning, stock sidebar, folder preview unaffected. Safety: all `check-safety.sh` steps PASS; unit-test
coverage of `sidebar_hook.c` is low (5.8 %) as it only runs inside Nautilus — covered by the smoke test.
Still manual (owner): right-click and drag-and-drop on the OS row, clicking/ejecting the drives.

Owner review (2026-11-11): layout, bars and behaviour confirmed ("looks amazing"), but the Computer row must
never be altered and the OS row must open `/` — the first version hid Computer and opened the Computer view
(misread request). Fixed: Computer row untouched, OS row opens `/` through `slot.open-location`, highlight
follows the window location `/`. Re-verified with the debug driver; tests and `check-safety.sh` re-run.

### 8.8 Phase 3 — Network shares

Findings (Nautilus 51.1, owner's `smb://media-server.local/mediadrive/` mounted):
- Mounted shares **are** in Nautilus' sidebar, as mount rows (`section MOUNTS`, `place EXTERNAL_MOUNT`).
  Shares that aren't mounted are not listed (S1 keeps it that way).
- Measured sort order within the mounts section: **place type first** (EXTERNAL 2 before INTERNAL 3), then
  `g_utf8_collate` on the label. So shares (and USB sticks) always sort above internal drives.
- `section-type` is construct-only (setting it later logs a GLib warning); the header (separator) function
  only separates sections. Nautilus' own rows can therefore not be re-sorted or separated → S6.

Design (S6): Nautilus' row of every network share is hidden (`sd-net-hidden`, restored on detach); our own
`GtkListBox` (CSS classes copied from Nautilus' list) and a `GtkSeparator` (margins/classes copied from
Nautilus' row-header separators) are inserted into the sidebar's box right after Nautilus' list. Our rows
mimic Nautilus' (`row > revealer.sidebar-revealer > box`, spacing and per-widget margins copied) with name,
usage bar, caption and an eject button. Network = not native, or `filesystem::remote` (kernel NFS/CIFS:
detected when its size query returns, then moved). Reconciled after every rebuild (mount/unmount). Click →
`slot.open-location`; eject → `clicked` on the hidden Nautilus row's eject button (Nautilus' own unmount
path). Highlight follows the window location like the OS row. The list is hidden when no share is mounted.

Spacing (owner review): our separator sits between two lists, so Nautilus' list bottom padding, the box
spacing and our list's top padding added up (12 px above and below, vs 8 px / 6 px for Nautilus' own
separators, measured). Fixed with CSS on our widgets only: `list.sd-net-list { padding-top: 0 }` and
`separator.sd-net-separator { margin-top: 2px }` (a CSS margin replaces the theme's, it doesn't add) →
8 px / 6 px, identical to the native separators.

Oddity: a themed `GIcon` with drive names (e.g. `drive-harddisk-symbolic`, from Nautilus' row or from
`g_mount_get_symbolic_icon`) rendered **blank** in our row, while `folder-remote-symbolic` renders; network
rows therefore always use `folder-remote-symbolic`. Root cause not found (only reproducible with a faked
local disk; real shares use that icon anyway).

Test aid (debug builds only): `SD_DEBUG_FAKE_NETWORK=<label>` treats that local drive as a share;
debug commands `sd-click-net`, `sd-dump` (network section). Never test eject with a faked share (it would
unmount the real disk). Verified: separator + list below the local drives, bar/caption, click opens it and
highlights, highlight moves to the OS row on `/`, kept across rebuilds, no warnings; light/dark.
Still manual (owner, real share): appearance, click, eject/unmount, unmount from elsewhere removes the row,
a slow/unreachable server never freezes Files.

### 8.9 Phase 4 — Refresh & hardening, Phase 5 — Docs (2026-11-12)

- Free-space refresh: on `notify::is-active` of the window (focus) and every `SD_REFRESH_S` (60 s) while the
  sidebar is mapped; refreshes closer than 2 s are skipped; a row never has more than one query in flight.
  Verified: a 2 GB file on `/` → OS row 445.7 → 443.5 GB free after a refresh (btrfs releases deleted space
  with a delay; the value always matches `statvfs`).
- Slow/dead servers: every query is async with a 3 s timeout that returns even if the backend ignores
  cancellation (Phase 1 tests); a failed refresh keeps the last known values instead of hiding the bar.
  Clicking a share hands navigation to Nautilus (its behaviour for unreachable servers is its own).
- Smoke test: `tools/smoke-nautilus.sh` now also drives the sidebar under ASan (`sidebar_round`: OS row,
  Home, network row, refresh, back to the test folder, plus random rounds), fails on `sidebar-drives`
  warnings and if the OS row never appears; `SMOKE_FAKE_NETWORK=<label>` exercises the network list.
  Debug commands added: `sd-refresh`, `sd-open <uri>`.
  Result (120 s, `SMOKE_FAKE_NETWORK="User Drive"`): PASS — 45 sidebar steps, 15 sidebar self-dumps,
  0 wrong previews, no sanitizer report mentioning our code.
- Final `check-safety.sh`: strict, `-fanalyzer`, scan-build, ASan+UBSan, valgrind all PASS; 9/9 test suites.
  Unit-test coverage: `drive_info.c` 95.9 %, `sidebar_hook.c` 3.8 % (runs only inside Nautilus → smoke test).
- Docs: README "Sidebar drives" section (behaviour, limitations, debug/smoke usage, layout table) and the
  RPM description.

---

## 9. Feature 3 — Settings (and sidebar-drives v2)

A "Nautilus Utils Settings" window, opened from Nautilus' main menu, with one tab per feature. Every tab
can switch its feature on or off; the Sidebar drives tab also controls the bar colour, what the drive rows
show and a sliding eject button. Also adds a right-click menu to the OS drive row. Prefix `nu_` (shared
settings core) / `st_` (window + menu hook), log domain `settings`, branch `dev-settings`.

### 9.1 Behaviour

#### 9.1.1 Menu entry

Nautilus' main (hamburger) menu gets one entry, **"Nautilus Utils Settings"**, directly above
"Preferences", in its own section so a separator sits below it (and the existing separator above it):

```
  Undo Trash / Redo
  ─────────────────────────
  Nautilus Utils Settings      ← new
  ─────────────────────────    ← new
  Preferences
  Keyboard Shortcuts / Help / About Files
```

No other menu item is changed, moved or hidden. If the menu can't be found, log one warning and skip the
entry (the features keep working with their stored settings).

#### 9.1.2 Settings window (GNOME HIG, libadwaita)

An `AdwDialog` presented over the Nautilus window it was opened from: `AdwToolbarView` with an
`AdwHeaderBar` holding an `AdwViewSwitcher` (one tab per feature, each an `AdwPreferencesPage`) and an
**Apply** button (`.suggested-action`) at the header bar's end. Controls only change a pending copy of the
settings (GSettings in delay-apply mode, `g_settings_delay`); **Apply** writes them
(`g_settings_apply`) and every open Nautilus window then updates live (no restart). Apply is insensitive
until something has changed (`has-unapplied`). Closing the dialog with unapplied changes discards them
(`g_settings_revert`). One dialog at a time per window (N5).

**Tab "Folder Preview"**
- Group "Folder Preview": switch row **"Show previews on folder icons"** (feature on/off).

**Tab "Sidebar Drives"**
- Group "Sidebar Drives": switch row **"Enhanced drive rows"** (feature on/off). Off = the stock Nautilus
  sidebar (no OS row, no bars, network shares back in Nautilus' own list). All other rows on this tab are
  insensitive while it is off.
- Group "Usage Bar":
  - Combo row **"Bar Colour"**: *Accent Colour* (follows the GNOME accent colour, live) / *Custom Colour* /
    *Gradient*.
  - **"Colour"** (`GtkColorDialogButton`), visible only for *Custom Colour*.
  - **"Start Colour"** and **"End Colour"**, visible only for *Gradient*: a two-point linear gradient,
    left → right, stretched across the **filled part** of the bar (N2).
  - Switch **"Show Usage Bars"**, switch **"Show Size Labels"** (the "x free of y" caption). Both off →
    drive rows go back to one line (name only), still in our order with the OS row.
- Group "Eject Button": switch **"Slide In on Hover"** (§9.1.4).

Every row has an accessible label; the dialog is fully keyboard-usable; light/dark/high-contrast follow the
system.

#### 9.1.3 OS drive context menu

Right-click (and long-press / Menu key) on the OS drive row opens a popover menu styled like Nautilus'
drive menu, without Unmount/Format and their separator:

```
  Open
  Open in New Tab
  Open in New Window
  ─────────────────
  Properties
```

All act on `/`. Network share rows keep no menu (S6 unchanged). Implementation: Nautilus' own menu (N6, §9.6).

#### 9.1.4 Sliding eject button (when "Slide In on Hover" is on)

- The eject button of every ejectable drive row (local and network) is hidden; the row's text, bar and
  caption take the **full width of the row**, exactly like the OS row.
- When the pointer is over the row **or the row has keyboard focus**, the button slides in from the right
  edge, over the content (no reflow). Under it, the row content fades out through a **gradient mask** whose
  width equals the button plus its padding (as planned), widened by the owner by 32 px
  (`SD_EJECT_FADE_EXTRA_PX`): full opacity at the mask's left edge, transparent from its middle to the
  right edge (§9.7 Phase 6). Pointer leaves / focus lost → slides out, mask goes away. Animation ~200 ms, ease-out; disabled when
  GTK's "enable-animations" is off (instant show/hide).
- The mask fades the content itself (not a painted colour), so it works on every row background (normal,
  hover, selected, light, dark).
- Off → today's layout (button always visible, bars end before it).

#### 9.1.5 Bar colours

- *Accent Colour*: `var(--accent-bg-color)`, follows accent changes live.
- *Custom Colour* / *Gradient*: the chosen colours; stored as `#rrggbb`.
- Warning (≥ 90 %) / error (≥ 95 %) colours apply **only with Accent Colour**; a custom colour or gradient
  is always shown as chosen (N3).

### 9.2 Rules

1. All §4 rules. The settings window and menu entry only exist on the main thread; no I/O in them except
   GSettings (dconf, async by design).
2. Nautilus-internals code stays in one file per feature: menu injection in `src/settings/menu_hook.c`,
   everything sidebar-related in `src/sidebar-drives/sidebar_hook.c`, folder preview in `view_hook.c`.
3. Settings live in **GSettings**, schema `io.github.zyrotec.NautilusUtils` (installed by Meson/RPM).
   If the schema isn't installed, log one warning and run with the built-in defaults (= today's behaviour).
   A shared, Nautilus-free wrapper `src/common/nu_settings.c` (first shared code, §3) gives typed getters +
   change notifications and is unit-tested.
4. Turning a feature off must fully undo its changes on open windows (folder icons back to stock, sidebar
   rows restored, our rows/CSS removed) and stop its timers/monitors; turning it on re-applies. Repeated
   toggling must not leak (stress test + smoke test).
5. Never alter Nautilus' existing UI beyond what is listed here (menu: one added section).

### 9.3 Settings keys (defaults = current behaviour)

| Key | Type | Default |
|---|---|---|
| `folder-preview-enabled` | b | true |
| `sidebar-drives-enabled` | b | true |
| `bar-colour-mode` | enum accent / custom / gradient | accent |
| `bar-colour` | s `#rrggbb` | `#3584e4` |
| `bar-gradient-start` / `bar-gradient-end` | s `#rrggbb` | `#3584e4` / `#9141ac` |
| `show-usage-bars` | b | true |
| `show-size-labels` | b | true |
| `eject-slide` | b | false |

Constants: `SD_EJECT_SLIDE_MS` 200, `SD_EJECT_FADE_EXTRA_PX` 32 (fade = button + padding + 32 px, owner
2026-11-12).

### 9.4 Decisions (owner, 2026-11-12)

| # | Decision |
|---|---|
| N1 | Eject button slides in on **hover or keyboard focus**. |
| N2 | Gradient spans the **filled part** of the bar (every bar shows the whole gradient). |
| N3 | Warning/error colours are **dropped** for Custom/Gradient, **kept** for Accent Colour (owner confirmed). |
| N5 | The dialog has an **Apply** button; controls change nothing until it is pressed; closing without Apply discards. |
| N6 | OS drive menu = **Nautilus' own row menu**, opened for our row via the sidebar's long-press gesture (§9.6); no menu code of our own. |
| N4 | OS drive menu: Open / Open in New Tab / Open in New Window / — / Properties. Network rows: no menu. |

### 9.5 Phases

| Phase | Work | Acceptance |
|---|---|---|
| 0 Spike | Probe (not built): how Nautilus builds its main menu (`GtkMenuButton` → `GMenuModel`, mutable?), insert a section above Preferences + our own action group; how Nautilus opens a location in a new tab / window and shows Properties (window/slot actions, else the public `org.freedesktop.FileManager1.ShowItemProperties` D-Bus call); right-click popover on our OS row; eject button moved into a `GtkOverlay` over the row content while keeping Nautilus' click handling; content mask via `gtk_snapshot_push_mask` in a small container widget; live undo of folder preview and sidebar drives. | Every question answered and recorded here; anything infeasible reported before building. |
| 1 Settings core | Schema + Meson install, `nu_settings.c` (typed getters, colour parsing/validation, change signals), unit tests. | Tests + `check-safety.sh` pass. |
| 2 Window + menu | Dialog with both tabs bound to a delayed GSettings, Apply button; menu entry + separator. | Entry above Preferences with separator below; nothing changes before Apply; Apply persists across restarts; close discards; light/dark. |
| 3 Feature toggles | Live on/off for folder preview and sidebar drives, full undo. | Toggle 50× in the smoke test without leaks/warnings; stock UI restored when off. |
| 4 OS row menu | Popover menu, actions on `/`. | All four items work; Computer row and other rows unchanged. |
| 5 Bar options | Colour modes (accent/custom/gradient), hide bars/labels. | Matches spec in light/dark; changes live. |
| 6 Eject slide | Hidden eject button, slide + gradient mask, full-width bars, focus support, reduced motion. | Works on local and network rows; eject still unmounts; no reflow; 16 ms probe clean. |
| 7 Hardening & docs | Smoke-test coverage for settings changes, `check-safety.sh`, README section + RPM (schema install). | All PASS. |

### 9.6 Phase 0 findings (2026-11-12, Nautilus 51.1 / GTK 4.24.1 / libadwaita 1.10)

Method: Nautilus 51.1 source read for reference (not vendored); throwaway probe `LD_PRELOAD`ed into an
isolated Nautilus (own D-Bus session) alongside the real module, rendering widgets to PNG. No eject was
ever pressed. Not in the repo.

**Main menu.** Defined in `nautilus-window.blp` as `menu app_menu` (template child, so **one `GMenu` per
window**), set on the header bar's `GtkMenuButton` (`icon-name open-menu-symbolic`). Three sections:
New Window/Tab/Show All Tabs · Undo/Redo · Preferences/Keyboard Shortcuts/Help/About. Nautilus only
rewrites the *contents* of the Undo section (`nautilus_gmenu_set_from_model`), never the top-level list.
- `g_menu_insert_section (menu, <index of the section whose first item is app.preferences>, NULL, ours)`
  gives exactly the requested layout (separator above and below our entry). ✅ (rendered)
- Our action: `gtk_widget_insert_action_group (window, "nautilus-utils", group)`; the menu's popover
  resolves `nautilus-utils.settings` through the window. ✅
- Inject per window when it maps; find the section by action name (not index) and mark the window so it
  happens once.

**Settings dialog.** `AdwDialog` + `AdwToolbarView` + `AdwHeaderBar` (title widget `AdwViewSwitcher`,
policy wide) + `AdwViewStack` of `AdwPreferencesPage`s + Apply button (`.suggested-action`, pack end)
presents over the Nautilus window with the usual dimmed backdrop; `GtkColorDialogButton` works. ✅
libadwaita 1.10 is already in Nautilus' process; we link `libadwaita-1` (same library).

**GSettings.** The default schema source finds a compiled schema in `$XDG_DATA_HOME/glib-2.0/schemas`
(✅ with a test schema) and not otherwise, so: RPM/`meson install` → `/usr/share/glib-2.0/schemas`;
development → install the schema into `~/.local/share/glib-2.0/schemas` (README). Missing schema →
defaults + one warning (§9.2.3), never a crash (`g_settings_new` aborts on an unknown schema, so we look
it up with `g_settings_schema_source_lookup` first).

**OS drive menu — use Nautilus' own menu (better than building our own).** Nautilus' row menu
(`create_row_popover`) builds its items from the row's properties; for our OS row (`uri file:///`, no
mount/volume/drive) that is exactly **Open / Open in New Tab / Open in New Window / — / Properties**
(no Unmount/Format; the bookmark section is hidden-when-disabled). It is reached from the row's own click
gesture (which our row lacks), the Menu key / Shift+F10 on the selected row (already works for our row),
and the sidebar's touch long-press gesture, whose handler takes `gtk_list_box_get_row_at_y (list, y)`.
- Right-click on our row → emit `pressed (x, y)` on the sidebar's `GtkGestureLongPress` with our row's
  centre in list coordinates → Nautilus shows its native popover for our row. ✅ (rendered: identical to
  the owner's screenshot minus Unmount/Format)
- Actions go through Nautilus' `row.*` group with our row as context row: `open_row()` and
  `properties_cb()` only read the row's `uri`. Verified: *Open in New Tab* added a background tab
  (1 → 2 tabs), *Properties* opened Nautilus' properties dialog for `/`; no warnings. ✅
- Difference to native rows: the popover points at the row, not the click position (long-press path).
- Fallback: no long-press gesture found → no menu, one debug message (row still works).

**Sliding eject.** Row box children: `[GtkImage, our .sd-drive-info box, GtkImage (end, hidden),
GtkButton.sidebar-button (eject, natural width 30 px, margin-start 4 px), AdwSpinner]`.
- Moving Nautilus' eject button into a `GtkOverlay` (child: a small container that holds our info box,
  overlay: `GtkRevealer` slide-left, `halign END`) keeps its 3 controllers; Nautilus keeps its pointer to
  the button. ✅
- Hidden → the bar spans the full row width (like the OS row) ✅. Revealed → the button sits over the
  right end and the content under it fades out through `gtk_snapshot_push_mask` (alpha mask, linear
  gradient over the last *button width + margins* = 34 px) ✅ (rendered).
- The fade width follows the revealer's animated width each frame (tick callback while animating).
- `on_net_eject` looks Nautilus' eject button up inside its (hidden) row, so the lookup must search
  descendants, not only direct children of the row box (Phase 6).

**Live on/off.** Both hooks already have a full "disable" path (graceful failure): folder preview gives
every image back to Nautilus (`detach_all`), sidebar drives removes our rows/network list and unhides
Nautilus' rows. Missing for a clean live toggle: sidebar drives must also **undecorate** Nautilus' drive
rows (move the label back, remove the info box, restore the eject button), and both hooks need a "re-enable"
that re-scans already-mapped widgets (the map emission hook only sees widgets mapped later). Phase 3.

**Phase 0 status: done** (2026-11-12). Design change → N6.

### 9.7 Phase results

**Phase 1 — Settings core (done 2026-11-12).** `data/io.github.zyrotec.NautilusUtils.gschema.xml` (keys
§9.3, enum `BarColourMode`), compiled into the build dir for tests and installed to
`$datadir/glib-2.0/schemas` with `glib-compile-schemas` as post-install. `src/common/nu_settings.c` (first
shared code, §3; GLib/GIO only): typed `NuConfig` snapshot, `#rrggbb` parse/format (an invalid stored colour
falls back to the default), schema looked up before `g_settings_new_full` (missing schema → defaults + one
warning), `nu_settings_new_delayed()` for the dialog (delay-apply), and listeners notified **once per batch**
from an idle with the changed groups (`NuChanged`), safe against listeners removing listeners or shutting
the service down mid-dispatch. Not wired into the module yet (Phase 2).
Tests: `tests/common/test-settings.c`, 12 tests (C defaults = schema defaults, stored values, invalid
colours, delayed apply → one notification, revert, every key group, missing schema, use without init,
re-entrancy, 300-cycle init/apply/shutdown stress). Safety: strict, `-fanalyzer`, scan-build, ASan+UBSan,
valgrind all PASS (10/10 suites under ASan); coverage of `nu_settings.c` 99.2 % (never run: the guard for a
schema key missing from the key→group table).

**Phase 2 — Window + menu (done 2026-11-12).** `src/settings/`: `settings.c` (feature entry, initialized
**first** in `features[]` so the other features can read settings; starts `nu_settings`),
`menu_hook.c` (the only file using Nautilus internals: finds new `NautilusWindow`s through GTK's toplevel
list → idle scan, inserts our section before the `app.preferences` section + action group
`nautilus-utils`; removed again on shutdown; one warning if the menu isn't found), `settings_dialog.c`
(`AdwDialog`, tabs via `AdwViewSwitcher`, `AdwViewSwitcherBar` + breakpoint below 500 sp, Apply button
bound to `has-unapplied`, controls bound to the delayed GSettings with mappings for the enum and
`#rrggbb` ↔ `GdkRGBA`, colour rows only visible in their mode, feature options insensitive while the
feature switch is off — following the pending value; without schema: an "Settings Unavailable" status
page and no Apply). Links `libadwaita-1` (≥ 1.5).
Verified in an isolated Nautilus (memory GSettings backend, so the owner's settings are never touched):
menu sections exactly as §9.1.1; edits leave the stored values unchanged and enable Apply; Apply stores
them and disables Apply; a pending edit is discarded on close; reopening shows the applied values;
narrow layout (bottom sheet, tabs at the bottom); no-schema path (one warning, entry still shown, status
page); no warnings. Packaging: schema in `%files`, `libadwaita-1` BuildRequires; README: Settings section,
dev schema install into `~/.local/share/glib-2.0/schemas`.
Finding: the module-load test failed under ASan on a clean machine (fatal "schema not installed" warning) —
the folder-preview tests now get `GSETTINGS_SCHEMA_DIR` = the build's compiled schema; all suites verified
with the home schema hidden (`XDG_DATA_HOME=/nonexistent`).
Safety: strict, `-fanalyzer`, scan-build, ASan+UBSan, valgrind all PASS (10/10 suites). Unit coverage of
the dialog/menu is low (dialog 0 %, menu 13 %: they need a Nautilus window) → smoke test (Phase 7).

**Phase 3 — Feature toggles (done 2026-11-12).** Both features follow their on/off switch live (one
notification per Apply):
- Folder preview: `fp_view_hook_set_enabled`. Off → every icon given back to Nautilus (`detach_all`),
  map events and folder-change notifications ignored (the map emission hook stays, so switching back on
  needs no restart). On → the mapped widget trees of all toplevels are walked and visible icons hooked
  again (they are not mapped a second time).
- Sidebar drives: `sd_sidebar_hook_set_enabled`. Off → `detach_all`, which now also **undecorates**
  Nautilus' drive rows (label moved back to its place, our bar/caption box removed, size query cancelled,
  accessible description reset) on top of removing our OS row/network list and unhiding hidden rows — the
  graceful-failure path benefits too. On → types resolved/validated if needed, mapped sidebars re-hooked.
- Initial state comes from the settings in each feature's initialize (settings feature runs first).
Debug driver (debug builds only): `st-set <key> <GVariant>` (as Apply would), `st-open`, `st-close`,
`st-dump`; `fp-state` (hooks + icons still showing our paintable); `sd-dump` while off reports our
leftovers anywhere in the windows. The smoke test now runs Nautilus with an **in-memory GSettings backend**
and the build's schema (never touches the owner's settings) and toggles both features `SMOKE_TOGGLES`
(25) times, failing on any leftover while off or preview shown while off.
Smoke (`smoke-20261112-184138`, ASan, ~/Pictures, 60 s + 25 toggle rounds): PASS — 25/25 sidebar off-dumps with
0 leftovers and OS row + bars back after every switch-on; 25/25 preview off-states with 0 icons showing
previews and icons re-hooked after every switch-on; 38 verifies, 0 wrong previews; no warnings; no sanitizer
report mentioning our code. Safety: strict, `-fanalyzer`, scan-build, ASan+UBSan, valgrind all PASS
(home schema hidden).

**Phase 4 — OS row menu (done 2026-11-12).** Right-click (secondary button) and Menu / Shift+F10 on our OS
row open **Nautilus' own drive menu** for it (N6): `show_os_menu` emits `pressed` on the sidebar's
`GtkGestureLongPress` at the row's centre in places-list coordinates, after checking that
`gtk_list_box_get_row_at_y` returns our row there. Items (from Nautilus, for a row without mount):
Open / Open in New Tab / Open in New Window / — / Properties; the hidden bookmark/unmount sections don't
show. No gesture found → no menu, one debug message, the row keeps working. Primary click still opens `/`.
Safety: Nautilus keeps a raw pointer to the menu's row (`context_row`), so before our OS row is removed
(switch-off, graceful failure, shutdown) an open row menu is closed first.
Verified in an isolated Nautilus (debug commands `sd-os-menu`, `sd-os-menu-dump`,
`sd-os-menu-activate <action> [int]`, `sd-os-menu-close`): items as above, *Open in New Tab* and
*Properties* run, switching the feature off with the menu open leaves 0 leftovers and no crash, menu works
again after switching back on. Smoke (`smoke-20261112-185718`, ASan, 5 toggle rounds + 60 s): PASS, menu checked for
Properties and for the absence of Unmount/Format/Eject. Safety: strict, `-fanalyzer`, scan-build,
ASan+UBSan, valgrind all PASS. Still manual (owner): a real right-click, Menu key, and the popover's position.

**Phase 5 — Bar options (done 2026-11-12).** `src/sidebar-drives/bar_style.c` (GLib only, in the core
library): `sd_bar_colour_css` turns the colour settings into one CSS rule for every filled block incl. the
warning/full states — accent: empty (built-in accent/warning/error rules apply), custom: the colour,
gradient: `linear-gradient(to right, start, end)` on the filled block, which is exactly as wide as the used
part (N2). Loaded into a second `GtkCssProvider` at APPLICATION + 1 and reloaded on every colour change.
Rows remember whether a size is known; bar and caption visibility = known ∧ show-usage-bars /
show-size-labels, re-synced on every row (Nautilus drive rows, OS row, network rows) when those settings
change; both off → one-line rows. Tests: `tests/sidebar-drives/test-bar-style.c` (4 tests, incl. GTK parses
every generated rule without errors); `bar_style.c` 100 % covered.
Verified in an isolated Nautilus with rendered PNGs (debug commands `sd-png <path>`, `sd-resize W H`):
accent, custom red, green→red gradient across the filled part, labels hidden, both hidden (one line), bars
hidden, back to accent — all live, no warnings. Smoke (`smoke-20261112-191813`, ASan): PASS with a style round (colour
modes × all four show/hide combinations) and a check that visibility always matches the settings
(19 style dumps). Safety: strict, `-fanalyzer`, scan-build, ASan+UBSan, valgrind all PASS.
Still manual (owner): light theme and high-contrast look of custom/gradient bars.

**Phase 6 — Sliding eject (done 2026-11-12).** `src/sidebar-drives/fade_bin.c` (plain GTK, own library
`sd-ui`): `SdFadeBin`, a one-child container whose snapshot pushes an alpha mask (`gtk_snapshot_push_mask`)
— a linear gradient over the button's width, opaque at the button's outer edge and **transparent from its
middle on** (owner-visible refinement: with a plain linear ramp the text showed half-transparent right under
the icon); mirrored for RTL. The mask width is read **at draw time** from the followed revealer's allocated
width (button + margins), so it matches the slide on every frame; a tick callback only redraws while the
revealer animates; 0 when Nautilus hides the button. Weak in both directions.
Hook (`eject_slide_attach/detach`, `sync_eject_mode`): with eject-slide on, every decorated row with an
eject button (Nautilus' drive rows, our network rows; not the OS row) gets `[overlay(fade bin(info),
revealer(eject)), …]` instead of `[info, …, eject]`: info takes the full width like the OS row, the revealer
(`halign END`, slide-left/right, `SD_EJECT_SLIDE_MS` 200 ms, GTK's enable-animations respected) shows the
button while the row `contains-pointer` (motion controller) or `contains-focus` (focus controller, N1).
Off → button and info go back to their exact places (previous sibling remembered); undecorate detaches first.
Nautilus' eject keeps working: its handler is on the button's own `clicked` (verified in the source and in
every smoke dump: `clicked-handler=1`, button inside our revealer).
Tests: `tests/sidebar-drives/test-fade-bin.c` (mask geometry incl. RTL/clamping and the stops, child
handling, 300-cycle follow lifecycle with either side destroyed first). They deliberately don't map a
window: mapped rendering loads fontconfig/pango/GPU caches that LSan reports and that hang under valgrind
(first attempt) — on-screen behaviour is covered by renders and the smoke test. Coverage `fade_bin.c`
84.7 % (rest = the mapped animation path).
Verified in an isolated Nautilus with renders (`sd-eject-reveal 0|1` simulates hover): hidden → full-width
bars on local and network rows; mid-slide; shown with content faded out before the icon; slid out; labels
hidden; option off → identical to before. Smoke (`smoke-20261112-200421`, ASan, fake network share, eject round incl.
feature off/on while sliding): PASS. Safety: strict, `-fanalyzer`, scan-build, ASan+UBSan, valgrind all PASS.
Still manual (owner): real hover/focus, real eject from a slid-in button, the animation's feel, light theme.
Owner change (2026-11-12): fade **16 px wider** than the button (`SD_EJECT_FADE_EXTRA_PX`). The extra grows
in with the slide (`sd_fade_follow_width`: revealer width + 16 × slide progress, progress = revealer width /
the button's natural width incl. margins), so a hidden button still means no fade and nothing jumps on the
first frame; the stops are unchanged (transparent from the middle of the now 50 px mask). Test added; render
checked; strict, `-fanalyzer`, scan-build, ASan+UBSan, valgrind PASS.
Owner change (2026-11-12, second): another 16 px → `SD_EJECT_FADE_EXTRA_PX` 32 (mask 66 px with a 34 px
button, transparent from 33 px in).

**Phase 7 — Hardening & docs (done 2026-11-12).** Final smoke (`smoke-20261112-202452`, ASan, fake network share, 150 s
random browsing + every settings round: 25 on/off toggle rounds, colour modes × show/hide, sliding eject
incl. feature off/on while sliding, OS drive menu): PASS — 26 sidebar off-dumps with 0 leftovers, 25 preview
off-states with 0 previews shown and 25 re-hooks, 64 style dumps with visibility always matching the
settings, 38 verifies with 0 wrong previews, no warnings, no sanitizer report mentioning our code. Final
`check-safety.sh` (home schema hidden): strict, `-fanalyzer`, scan-build, ASan+UBSan (12/12 suites),
valgrind all PASS. Coverage: `nu_settings.c` 99.2 %, `bar_style.c` 100 %, `fade_bin.c` 84.4 %; the
Nautilus-internals files (`menu_hook.c`, `sidebar_hook.c`, `settings_dialog.c`) run only inside Nautilus →
smoke test + isolated-Nautilus checks.
Packaging (emulated, `rpm-build` not installed): release build with Fedora's `%{optflags}` and
`-Dwerror=false` warning-free; staged `DESTDIR` install contains exactly the two `%files` entries (module +
schema; `glib-compile-schemas` post-install correctly skipped under `DESTDIR`, Fedora's file trigger compiles
it); schema valid (`glib-compile-schemas --strict`); `%check` set (`--no-suite thumbnailers --no-suite
latency`) 9/9 with no home schema; module still exports only the 3 Nautilus entry points.
Docs: README (Settings section, dev schema install/uninstall, sidebar section, smoke options, layout table),
RPM description.

Still manual (owner): real right-click / Menu key on the OS row and the menu's position; hover/focus feel of
the sliding eject and a real eject from a slid-in button (on a removable drive or share); custom/gradient bars
in the light and high-contrast themes; the settings dialog with a real mouse and keyboard.

---

## 10. Feature 4 — Folder views (per-folder view type and icon size)

Every folder remembers its **own view type** (grid / list) and **icon size**, like Windows File Explorer.
Files itself keeps one global view type and one global icon size per view type, so changing them in one
folder changes them everywhere. With this feature, a change only applies to the folder it was made in.
Prefix `fv_`, log domain `folder-views`, dir `src/folder-views/`, branch `dev-path-view-size`.

### 10.1 Behaviour

- **Opening a folder** (any window, tab, back/forward, typed path, sidebar, bookmark): if the folder has a
  saved view, show it with that view type and that view type's icon size; otherwise use **our default**
  (§10.1.1). This happens before the folder's files are shown: they are never drawn in the wrong view
  (verified in Phase 0, §10.7).
- **Changing the view in a folder** (the grid/list toggle, Ctrl+1 / Ctrl+2, zoom in/out/reset in the view
  menu, Ctrl +/−/0, Ctrl+scroll): saved for **that folder only**. A folder stores its view type plus one icon
  size per view type (switching a folder from grid to list and back keeps its grid size).
- **Other open windows and tabs** are not changed by a change in another folder; each tab follows the
  folder it shows.
- **Not saved, always our default:** locations that can't store metadata (§10.1.2), and Files' global search.
  The Network view keeps Files' own Network view.
- Sort order is not part of this feature: Files already saves it per folder.

#### 10.1.1 Our default (owner: "Files default", F1)

Files writes its global keys (`org.gnome.nautilus.preferences default-folder-viewer`,
`org.gnome.nautilus.icon-view default-zoom-level`, `org.gnome.nautilus.list-view default-zoom-level`) every
time the view or size is changed anywhere, and its Preferences window doesn't show them. So the default
would drift with every per-folder change. Instead the feature keeps **its own copy of the default**,
captured once from Files' keys the first time the feature runs, and unchanged by per-folder changes.
Turning the feature off writes that default back into Files' keys, so Files behaves exactly as before the
feature was used.

#### 10.1.2 Storage (owner: GIO metadata, F2)

- One GIO metadata attribute per folder, `metadata::nautilus-utils-folder-view` (Files stores its per-folder
  sort order the same way), value `1;<generation>;<view>;<grid-zoom>;<list-zoom>` (format version 1).
  It follows folders renamed or moved inside Files (gvfs metadata).
- Reads and writes are asynchronous (`g_file_query_info_async`, `g_file_set_attributes_async`) with a
  `GCancellable`; nothing blocks the main thread.
- Locations whose filesystem can't store metadata (checked in Phase 0: Trash, Recent, Starred, search,
  `other-locations`, some network shares) use our default and save nothing.
- Malformed, unknown-version or out-of-range values are ignored (= our default).

#### 10.1.3 Reset all (owner, F3)

"Reset All Folder Views" on the settings tab forgets every saved folder view: it increments the
`folder-views-generation` key, and saved values with an older generation are ignored (no need to find every
folder that was ever saved). Open windows switch to our default immediately. Asks for confirmation first
(`AdwAlertDialog`, destructive style), because it can't be undone.

### 10.2 Settings tab "Folder Views" (in the existing Nautilus Utils Settings dialog, §9.1.2)

- Group "Folder Views": switch row **"Remember View per Folder"** (feature on/off), subtitle "Each folder
  keeps its own view type and icon size".
- Group "Saved Views": button row **"Reset All Folder Views…"** (`AdwButtonRow`, libadwaita ≥ 1.6,
  `.destructive-action`), insensitive while the feature is off. Unlike the other controls it acts immediately after confirmation (it is not a pending
  setting, so the Apply button doesn't apply to it).
- On/off still waits for **Apply** like every other setting (N5).

### 10.3 Rules

1. All §4 and §9.2 rules. Nautilus-internals code (slots, view switching, zoom actions) only in
   `src/folder-views/view_mode_hook.c`; the encoding/decoding, generation and default logic in a
   Nautilus-free, unit-tested `fv_state.c`.
2. Only Nautilus' **own actions** change the view: `slot.files-view-mode` (u: grid/list) and
   `view.zoom-in` / `view.zoom-out` / `view.zoom-standard`. No private structs, no private headers.
3. Our own view/zoom changes must never be mistaken for user changes (no save loops), and a save never
   writes the same value twice.
4. Turning the feature off stops applying and saving, restores Files' global keys to our default and leaves
   saved metadata in place (turning it back on brings the folders' views back). Repeated toggling must not
   leak (smoke test).
5. If an expected action or widget is missing: one warning, the feature switches itself off, Files keeps
   working with its normal global behaviour.

### 10.4 Settings keys (added to `io.github.zyrotec.NautilusUtils`)

| Key | Type | Default |
|---|---|---|
| `folder-views-enabled` | b | true |
| `folder-views-generation` | u | 0 |
| `folder-views-default-view` | enum grid / list | grid |
| `folder-views-default-grid-zoom` | enum small / small-plus / medium / large / extra-large | medium |
| `folder-views-default-list-zoom` | enum small / medium / large | medium |
| `folder-views-defaults-captured` | b | false (set true after the one-time capture, §10.1.1) |

### 10.5 Decisions (owner, 2026-11-12)

| # | Decision |
|---|---|
| F1 | A folder with no saved view uses **Files' default**; changing one folder never changes that default (§10.1.1). |
| F2 | Saved in **GIO metadata**, like Files' per-folder sort order (§10.1.2). |
| F3 | Settings tab: **on/off** and a **Reset All** button (§10.2). |
| D1 | Files' global keys are put back to our default **2 s after the last change** (`FV_RESTORE_KEYS_DELAY_MS`), not after every switch: a burst of navigation costs one dconf write. Nothing in Files reads them in between except a new tab/window, which then starts from the last folder's view and is switched like any other navigation. Also restored on shutdown. |
| D2 | A **search inside a folder** keeps that folder's view and saves nothing; only Files' **global** search (slot `search-global`) uses our default. |
| D3 | Tabs are found with an emission hook on `AdwTabView::page-attached` (public libadwaita signal; Nautilus adds the page before the tab loads, §10.7) plus a sweep of existing windows. Nautilus' view types are registered only when the first view is built; a tab can report its location before that (seen in Phase 3, intermittently), and Nautilus doesn't notify the same location again. So a miss is not an error: the tab is caught up at its first `notify::loading` (the view exists then); only if the types are still missing 5 s later (`FV_TYPES_TIMEOUT_MS`) does the feature switch itself off. |

### 10.6 Phases

| Phase | Work | Acceptance |
|---|---|---|
| 0 Spike | Probe (not built): the earliest point a slot's new location is known (slot `notify::location`, `notify::loading`, view `notify::...`) and whether switching `slot.files-view-mode` there shows no flash of the old view; how to read a view's current zoom level and step it with `view.zoom-*` (also for background tabs, whose `view` action group isn't on the window); detecting user changes (action state changes, view `icon-size` notify) vs ours; metadata read latency on the owner's drives (ext4/btrfs, NTFS `ntfs3`, SMB/SFTP) and which locations can store metadata. | Every question answered and recorded here; anything infeasible reported before building. |
| 1 Core | Schema keys, `fv_state.c` (encode/decode, validation, generation, defaults capture), unit tests. | Tests + `check-safety.sh` pass. |
| 2 Apply & save | `view_mode_hook.c`: apply on navigation in every slot, save on user change, async metadata, global keys kept at our default. | Folders keep their own view type and size across navigation, tabs, windows and restarts; no flash; no save loops; 16 ms probe clean. |
| 3 Settings tab | "Folder Views" tab, live on/off with full undo, Reset All with confirmation. | Off = stock Files behaviour with our default restored; Reset All resets every folder; toggling 50× in the smoke test without leaks/warnings. |
| 4 Hardening & docs | Smoke-test rounds (navigate, change views, reopen, reset, toggle), `check-safety.sh`, README + RPM. | All PASS. |

### 10.7 Phase 0 findings (2026-11-12, Nautilus 51.1 / GTK 4.24.1)

Method: Nautilus 51.1 source read for reference (not vendored); throwaway probe `LD_PRELOAD`ed into an
isolated Nautilus (own D-Bus session, in-memory GSettings, own `XDG_DATA_HOME` so the owner's real gvfs
metadata was never touched), logging every painted frame (view type, icon size, which folder's files are
drawn). Test folders on tmpfs, btrfs (`/home`) and NTFS (`ntfs3`, Personal Drive), 150 files each plus one
with 10 000 files; all removed afterwards. Not in the repo.

**How Files picks the view.** Each slot (tab) reads `default-folder-viewer` once when it is created and keeps
its own copy; on every navigation it switches to that copy (`nautilus_window_slot_get_view_id_for_location`).
`slot.files-view-mode` (u: 1 = list, 2 = grid) switches the view and updates the slot's copy **and** the
global key. Zoom lives in the inner view (`NautilusGridView` / `NautilusListView`, both `NautilusListBase`),
read from the global key only when an inner view is created; `view.zoom-in/out/standard` change it and write
the global key. **Nothing in Files listens to those global keys**, so writing or restoring them never
changes an open tab (§10.1.1 is safe).

**Hook point: slot `notify::location`.** Order inside Files' navigation callback: view switch to the slot's
default → view gets the new location (starts loading) → slot `notify::location`, all synchronously. At that
moment the **old folder's files are still on screen**; the new folder's files arrive 200 ms – 1 s later (also
for Back to a folder visited before). Measured in every case (tmpfs, btrfs, NTFS, 10 k files, Back):

- Switch synchronously at `notify::location` (value already in memory): no frame in between.
- Switch after an async metadata read: read took 0.3–18 ms; at most one frame painted before the switch,
  and that frame still shows the **old** folder.
- So the new folder's files are **never** drawn in the wrong view. What is visible: after the click, the
  old folder's files are re-laid out in the new folder's view type until the new files arrive (a list view
  usually shows empty instead). Files already keeps the old files on screen while loading; we only change
  their layout.

**Cost.** Switching view type is Files' own switch, the same as pressing Ctrl+1/Ctrl+2: 100–190 ms on the
main thread with 150 files, **295 ms when leaving a 10 000-file folder** (the old files are re-laid out
once). Only paid when two folders in a row differ in view type; same type = no switch. A zoom step costs
~10 ms.

**Detecting user changes.** View type: slot `notify::icon-name` fires on every view switch (Files' own,
ours, the user's). A switch followed by `notify::location` in the same main-loop iteration is navigation;
one that isn't (checked in an idle) is the user → save. Ours are marked by a guard flag around our action
call. Icon size: `NautilusListBase` `notify::icon-size` outside our guard → save (reconnected when the inner
view is replaced).

**Background tabs.** `slot.files-view-mode` is on the slot widget and `view.zoom-*` on the
`NautilusFilesView` widget itself (the window only gets a copy for the active tab), so
`gtk_widget_activate_action` on those widgets works for tabs that aren't selected (verified: background tab
zoomed and switched, the selected tab unchanged).

**Metadata support** (`metadata::nautilus-utils-folder-view`, write + read back):

| Location | Result |
|---|---|
| tmpfs, btrfs, NTFS `ntfs3`, read-only folders (`/`, `/usr`) | stored (write 1–7 ms, read 0.2–4 ms) |
| `trash:///`, `recent:///`, `network:///` | stored |
| `starred:///`, `other-locations:///` | write "succeeds" but nothing is stored → our default |
| SMB / SFTP shares | **not tested** (no real share here); async, so a slow share can't block |

**Design consequences (for Phase 2).**
1. Apply at slot `notify::location`; keep an in-memory cache (URI → value) of folders read or saved this
   session so revisits apply synchronously; first visits read async and apply on arrival.
2. Never switch for `network:///` (Files' Network view) or global search.
3. The switch cost above is Files' own and only happens when the view type changes between folders; no
   way around it through public behaviour. Recorded as expected cost, measured again in Phase 2 with the
   16 ms latency probe.

### 10.8 Phase results

**Phase 1 — Core (done 2026-11-12).** Schema: three enums (`FolderView` list=1/grid=2, `GridZoom` 0–4,
`ListZoom` 0–2, same values as Nautilus' own) and the six keys of §10.4. `nu_settings` (shared): `NuViewType`,
`NuFolderView`, the new keys in `NuConfig`, change groups `NU_CHANGED_FOLDER_VIEWS` / `_FV_GENERATION` /
`_FV_DEFAULT`, `nu_folder_view_valid`, `nu_settings_capture_folder_view_default` (one batch through a
delayed copy, only once, invalid input → built-in default) and `nu_settings_reset_folder_views` (generation
+ 1 right away, wraps at 2^32). `src/folder-views/fv_state.c` (GLib only): `fv_encode` / `fv_decode`
(`1;<generation>;<view>;<grid-zoom>;<list-zoom>`, digits only, every field range-checked, other generation
or format version rejected), `fv_location_kind` (save / default only: starred, other-locations, search /
ignore: `network:///` root, no scheme), zoom level ↔ Nautilus 51.1 icon size, and `FvCache`, an LRU of
URI → saved view or "has none" (`FV_CACHE_MAX` 4096). `fv-core` is not linked into the module until Phase 2.
Tests: `test-fv-state` 10 (round trip of every view × generation, 29 rejected values, generation reset,
location kinds, zoom tables, LRU order, 5 × 12 288-entry fill with clears + 1000 create/free cycles);
`test-settings` 15 (+3: validity, capture, reset; defaults-match-schema and each-group cover the new keys).
Safety: strict, `-fanalyzer`, scan-build, ASan+UBSan, valgrind all PASS. Coverage: `fv_state.c` 100 %,
`nu_settings.c` 99.3 % (the one line is from Feature 3).
The dev schema in `~/.local/share/glib-2.0/schemas` was updated (the module in `build/` reads the new keys;
GSettings aborts on unknown keys).

**Phase 2 — Apply & save (done 2026-11-12).** `src/folder-views/view_mode_hook.c` (all Nautilus internals of
the feature) and `folder_views.c` (entry, registered after the other features in `src/extension.c`; reads the
on/off switch once at start until Phase 3). Per tab (`NautilusWindowSlot`, qdata-owned `SlotHook`, weak refs
only): at `notify::location` the folder's view comes from the cache (synchronous) or an async metadata read;
the view is switched with `slot.files-view-mode` and zoomed step by step with `view.zoom-in/out` on the tab's
own `NautilusFilesView` (works for background tabs). User changes: `notify::icon-name` not followed by a
location change (idle) and `NautilusListBase` `notify::icon-size` outside our guard → saved with
`g_file_set_attributes_async`, skipped when unchanged. Files' global keys restored per D1. Debug driver
(`-Ddebug_driver=true`): `fv-open`, `fv-tab-open`, `fv-new-tab`, `fv-view`, `fv-zoom`, `fv-dump`, `fv-keys`.

Checked in an isolated Nautilus (own D-Bus session, own `XDG_DATA_HOME` so the gvfs metadata was the test's
own, in-memory GSettings), ASan + UBSan build, two sessions on the same metadata: 33 tab dumps, **0
mismatches** between what a tab shows and what its folder should show. Covered: user list/grid switch and
zoom saved per folder; a never-visited folder gets our default; revisit from the cache; new tab; a
**background tab** navigated and switched while another tab is selected; NTFS (`ntfs3`) and tmpfs folders;
a 10 000-file folder; Trash (saved); Starred (default only, the user's change is not saved); Files' global
keys back at our default 2 s later; **session 2 (fresh process): every folder came back with its saved
view** from metadata, a never-visited folder with the default. No warnings from our log domains.
Our apply time (normal build, `G_MESSAGES_DEBUG`): 33–89 ms when the view type changes (Files' own switch,
§10.7), 1–9 ms for a zoom-only change, 0 ms when the folder already matches. The 16 ms probe's larger
stalls in the same runs (up to 323 ms) are Files loading the folders (the 10 000-file folder) and appear
with or without a switch.
ASan: no report in our code. One leak report has our `restore_global_keys` → `g_settings_set_enum` in its
stack: a **third-party** leak in `pygi` (`pygi_method_cache_new`), from a Python Nautilus extension on this
system reacting to the settings change. Phase 4 must teach the smoke test's matcher to skip leaks
allocated in third-party code even when our frames are further down the stack. The smoke test must also
isolate `XDG_DATA_HOME` (exported before `dbus-run-session`, so `gvfsd-metadata` gets it too); otherwise
it would write test metadata into the owner's real gvfs metadata.
Safety: strict, `-fanalyzer`, scan-build, ASan+UBSan, valgrind all PASS; the module still exports only the 3
Nautilus entry points. Coverage: `fv_state.c` 100 %, `folder_views.c` 100 %; `view_mode_hook.c` 18 % (runs
only inside Nautilus → the isolated-Nautilus checks above, and the smoke test from Phase 4).

**Phase 3 — Settings tab (done 2026-11-12).** Settings dialog: third tab **Folder Views** (icon
`view-grid-symbolic`): switch row "Remember View per Folder" (pending until Apply, like every setting) and
group "Saved Views" with the button row "Reset All Folder Views…" (destructive style, insensitive while the
pending switch is off). It opens an `AdwAlertDialog` ("Reset All Folder Views?", Cancel default, Reset
destructive); only Reset calls `nu_settings_reset_folder_views()`, right away. libadwaita requirement raised
to 1.6 (`AdwButtonRow`) in Meson and the RPM. Feature: settings listener in `folder_views.c`. On → the hook
starts and attaches to every open tab, applying each folder's saved view. Off → it detaches everything and
puts Files' keys back to our default (tabs keep what they show; Files' stock behaviour from then on).
Reset All / our default changing → `fv_view_mode_hook_refresh` re-applies every open tab (Reset also drops
the in-memory cache so folders are read again and older-generation values are ignored). The debug driver
now lives as long as the module, with `fv-reset` (same call as the button) and an off-dump (tabs still
hooked, Files' keys at our default).
Found and fixed: a tab could report its first location before Nautilus' view types existed, which switched
the feature off at start (D3). It now catches up at `notify::loading`, with a 5 s watchdog.
Checked in the isolated ASan Nautilus (two runs): the first folder of the first window applied; saved views
in two tabs; **off** → 0 tabs still hooked, Files' keys at our default; a change made while off is not
saved; **on** → both tabs back to their folders' saved views; **Reset All** → both tabs at the default at
once, the old value ignored on revisit, saving works again with the new generation; **25 off/on rounds**:
26 off-dumps with 0 leftovers, 70 tab checks with 0 mismatches, no warnings. Dialog (probe on the real
dialog, rendered): tab layout; Reset insensitive while the switch is pending off (stored value unchanged
until Apply); confirmation shown; Cancel → generation unchanged; Reset → generation + 1; narrow layout
moves the three tabs to the bottom bar.
Safety: strict, `-fanalyzer`, scan-build, ASan+UBSan, valgrind all PASS (coverage of the Nautilus-side
files is from the isolated-Nautilus checks above, not the unit tests).

**Phase 4 — Hardening & docs (done 2026-11-12).** Smoke test (`tools/smoke-nautilus.sh`): the test Nautilus
gets its own `XDG_DATA_HOME` (exported before `dbus-run-session`, so `gvfsd-metadata` uses it too) and the
`folder-views` log domain; a **folder-views round** on a test tree under the log dir (NTFS, path with a space):
user list + zoom, default for an unchanged folder, zoom saved, revisit, new tab + background tab, off (change
while off not saved) / on, Reset All, old value ignored; folder views on/off added to every toggle round;
random browsing also switches view types. New checks: every `fv dump: tab=` line `ok=1`, every
`fv expect: ok=1` (new debug command: what the selected tab must show), no `FAILED`, off-dumps with 0
tabs still hooked and Files' keys at our default, at least one checked tab. The sanitizer matcher now skips a
**leak allocated by another extension's Python runtime** (first frame below the allocator and
GLib/GObject/GIO in pygi/libpython) even when our frames are further down (the Phase 2 pygi case). Verified
on sample reports: that block skipped, our own leaks (also one allocated inside a GTK call made by us) still
reported. Docs: README (Folder views section, Settings tab, smoke isolation, layout table), RPM description
(and libadwaita ≥ 1.6 from Phase 3).
Final smoke (`smoke-20261112-231322`, ASan, fake network share, 150 s random browsing + every settings
round): **PASS**. Folder views: 57 tab checks and 9 named checks, 0 wrong, 26 off-dumps without leftovers;
the other features unchanged (38 verifies, 0 wrong previews; 26 sidebar off-dumps; 25 preview off-states
and re-hooks; 64 style dumps; sliding eject; OS menu). No warnings, no sanitizer report mentioning our code.
The owner's real gvfs metadata was not touched (unchanged since before the tests).
Final `check-safety.sh`: strict, `-fanalyzer`, scan-build, ASan+UBSan, valgrind all PASS; coverage `fv_state.c` 100 %,
`nu_settings.c` 99.3 %.

Still manual (owner): real mouse and keyboard (Ctrl+1/2, Ctrl +/−, Ctrl + scroll, the view menu) in a few
of your own folders; restart Files and check they come back; a real SMB/SFTP share (untested, §10.7);
the Folder Views tab and the Reset confirmation with a real mouse.

### 10.9 Smooth transitions (owner, 2026-11-12: "The transition between folders needs to be more smooth")

What was rough (§10.7): when a folder needs another view type or icon size than the one before it, Files
re-lays out the **old** folder's files in the new layout (they jump), then the new files pop in; the switch
also blocks the main loop for 30–300 ms.

Now (`view_mode_hook.c`, "Smooth transitions"), only when something actually changes:
1. **Cover:** before switching, a static picture of the current view (`gtk_widget_paintable_new` →
   `gdk_paintable_get_current_image`, a render-node copy, no pixel readback) goes on top of it. It sits in a
   box with the `view` style, so nothing shows through, as an extra child of the files view's `GtkOverlay`,
   just above the view and below Files' floating "Loading…" bar. It is not a target for clicks or focus and
   is hidden from accessibility.
2. **Switch:** view type and zoom change underneath. The old files' jump and the main-loop block happen
   behind an unchanged picture.
3. **Fade:** when the tab stops loading (`notify::loading` false, i.e. the new files are in), or after
   `FV_COVER_MAX_MS` (1 s) for slow folders, the cover fades out over `FV_FADE_MS` (180 ms, ease-out cubic,
   `AdwTimedAnimation`, so it is instant with GTK animations off) and is removed. Another folder change during
   the cover keeps the same picture and restarts the wait.
4. Background tabs (not mapped) and folders that already match get no cover. Feature off or tab closed →
   the cover is removed with the hook.

Measured (isolated Nautilus, normal debug build, 180-file folders and a 10 000-file folder): the cover is up
from the click until 210–950 ms (= when Files has loaded the folder, the same moment stock Files would show
it), then fades in 180 ms. Whole-window renders during list → grid and grid → list: the old view stays
still, then crossfades into the new layout, with no jump or flash and the same background. The switch
underneath still costs 105–325 ms of main loop (Files' own view rebuild), but the screen doesn't change
during it.
Smoke (ASan, fake network share, 150 s + every round): `smoke-20261112-234613` **PASS**. Folder views: 57 tab
checks, 9 named checks, 0 wrong; 26 off-dumps without leftovers; 6 transitions completed (cover up →
fade → removed). The run before it (`smoke-20261112-234043`) failed only the sidebar's OS-drive-menu check
("row not in the places list" right after the network-row click, `sidebar_hook.c`, Feature 3). That code is
not touched by this change, and it passed in the re-run and in the Phase 4 run: an intermittent timing issue
in that check, recorded here for Feature 3's follow-up.
Safety: strict, `-fanalyzer`, scan-build, ASan+UBSan, valgrind all PASS.

### 10.10 Instant feel: read-ahead, switch after the click's frame, no fade (owner, 2026-11-13: "I want the transition to feel instant, can't we read the next dir pref and load it beforehand?")

Measured before this change (§10.9 build): the click itself wasn't drawn for 100–300 ms (the switch ran
inside Nautilus' location notification, before the next frame), then the cover waited for loading and
faded for 180 ms. Reading a folder's view takes 0.3–18 ms, so it was never the main delay, but it is
removed too.

1. **Read-ahead** (the owner's idea): when a local folder has finished loading, the saved views of its
   **subfolders**, its **siblings** (the parent's subfolders) and its **parent** are read in the background
   (`g_file_enumerate_children_async` + `next_files_async`, `G_PRIORITY_LOW`, GIO worker threads, up to
   `FV_PREFETCH_MAX` 2000 entries per folder, batches of 64) into the cache (`FV_CACHE_MAX` raised to 8192).
   Only folders the cache doesn't know are stored, so a value the user just saved is never replaced by an
   older one. It is cancelled when the tab moves on, and skipped on network shares (no second enumeration
   over the network).
2. **Switch after the click's frame:** at the location change only the still picture goes up (cheap); the
   switch runs in an idle right after the next frame has been drawn (`after-paint` on the slot's frame
   clock, with a `FV_SWITCH_FALLBACK_MS` 100 ms fallback). The path bar and loading state show at once, and
   Files' 0.1–0.3 s view rebuild happens behind an unchanged picture. Background tabs switch right away.
3. **No fade:** `FV_FADE_MS` 0. The picture is removed the moment the new folder's files are in, a cut
   like stock Files (the fade code stays for a non-zero value).

Measured (isolated Nautilus, normal build, 180-file folders A/B/C and a 10 000-file folder, same
navigation, frame logger probe), from the location change to the first drawn frame / to the first frame
showing the new folder's files uncovered:

| | first frame | new folder shown |
|---|---|---|
| stock Files (feature off) | 10–16 ms | 335–458 ms (10 000 files: 840 ms) |
| folder views, §10.9 build | 100–300 ms | ~390–980 ms (load + switch + 180 ms fade) |
| folder views, this build | 13–31 ms | 279–697 ms (10 000 files: 887 ms) |

Every folder's view was known at the click (read-ahead/cache) except the very first one. The one first-frame
outlier (417 ms, first visit to B) matches stock's first-visit stall (335 ms on its first folder).
**Remaining gap:** on list ↔ grid changes the new folder appears on average ~0.13 s later than stock. That
is Files' own view rebuild (the same as Ctrl+1/Ctrl+2), which the main thread has to run, and it can't be
skipped through Files' public behaviour. Folder changes with the same view type, or that only zoom, are as
fast as stock (zoom ~10 ms).
Smoke (ASan, 150 s + every round): `smoke-20261113-001117` **PASS** (57 tab checks, 9 named checks, 0 wrong; 26 off-dumps clean; 6 transitions). Safety: strict, `-fanalyzer`, scan-build, ASan+UBSan, valgrind all PASS.

---

## 11. Feature 1 v2 — Folder preview: stacked layout, custom folder icons, thumbnail style

Owner request (2026-11-13): a second **Stacked** layout beside the existing **Grid** (2×2) layout, the
option to replace the folder icon with your own SVGs in that layout, style options for the thumbnails, and
an on/off switch for hover cycling. Branch `folder-thumbnail-preview-extended`. Mock-ups: the owner's
`Asset-1` (back of the folder), `Asset-2` (the card stack), `g1` (front overlay), `Asset-3` (result).

### 11.1 Behaviour

- **Which folders get previews (both layouts):** only folders whose icon is the theme's plain folder icon
  (icon file `folder.*` or `inode-directory.*`, V6). Every `folder-*` icon (Pictures, Documents, Downloads, Music,
  Videos, Templates, Public, remote, …) and any custom icon keeps its normal icon, with no preview.
- **Layout** (setting): **Grid** = the v1 look (§6.1.2), **Stacked** = up to 3 rounded "cards" stacked
  like the mock: the front card full width, each card behind it inset on both sides and raised by
  4 % of the stack width and dimmed a little. The front card shows the first photo, the cards behind show
  the next photos (only their top edges are visible). Each photo fills its card (cropped, not letterboxed).
  A folder with 1 or 2 photos shows 1 or 2 cards.
- **Thumbnail style (both layouts):** border on/off, border colour, shadow on/off, random rotation on/off.
  Defaults = the v1 look (white border, shadow, rotation on). Stacked cards tilt at most ±4°.
- **Cycle on hover** (both layouts, on/off). Stacked layout: **photos per step** = *One* (each step brings
  the next photo to the front: 1-2-3, 2-3-4, …, every photo once, up to 16) or *Three* (fresh set of 3 per
  step like Grid). The stack wraps around at the end so it never shrinks.
- **Custom folder icon (Stacked only):** an SVG that replaces the folder icon (the back of the folder),
  drawn centred in the icon square, aspect ratio kept. **Front overlay (Stacked only):** an SVG drawn on
  top of the cards at the **same size and place** as the folder icon; the user positions the overlay inside
  its SVG (same canvas as the folder SVG). Either one may be set alone.
- Folders with nothing to preview (or not loaded yet) keep the **theme icon** (owner); the custom SVGs are
  drawn only together with the cards.
- Card stack placement (Stacked): measured from the folder's visible body (its opaque bounding box): stack
  width 0.969 × body width, centred, top at 0.204 × body height below the body top (= the mock).

### 11.2 Rules

1. All §4 and §6.2 rules. Frames are still composited in workers; the frame cache (memory and disk) is
   keyed by the style, so changing a style setting never shows a frame of the old style.
2. Custom SVGs are rendered with GTK's own SVG renderer (`GtkSvg`, GTK ≥ 4.22; external resources and
   animations off) into textures at the exact device size, once per zoom level (decision V3).
3. A missing or invalid SVG = not set (logged at debug level); Files keeps working.

### 11.3 Settings keys (added to `io.github.zyrotec.NautilusUtils`)

| Key | Type | Default |
|---|---|---|
| `folder-preview-layout` | enum grid / stacked | grid |
| `folder-preview-cycle` | b | true |
| `folder-preview-stack-step` | enum one / three | one |
| `folder-preview-border` | b | true |
| `folder-preview-border-colour` | s `#rrggbb` | `#ffffff` |
| `folder-preview-border-width` | d 0.5–6 (px at 64 × 64) | 1.0 |
| `folder-preview-shadow` | b | true |
| `folder-preview-rotation` | b | true |
| `folder-preview-keep-aspect` | b | true |
| `folder-preview-depth-effect` | enum none / blur / transparency / darken | blur (stacked only) |
| `folder-preview-corner-radius` | i 0–16 (px at 64 × 64) | 2 |
| `folder-preview-fade` | i 0–100 (% of the stack height) | 0 (stacked only) |
| `folder-preview-view-top-radius` | i 0–16 (px at 64 × 64) | 0 (stacked only) |
| `folder-preview-view-bottom-radius` | i 0–16 (px at 64 × 64) | 0 (stacked only) |
| `folder-preview-stack-padding` | i 0–20 (px at 64 × 64) | 8 (stacked only) |
| `folder-preview-stack-position` | enum top / centre / bottom | centre (stacked only) |
| `folder-preview-stack-offset` | i −16–16 (px at 64 × 64) | 0 (stacked only) |
| `folder-preview-folder-icon` | s (path, empty = theme) | '' |
| `folder-preview-overlay-icon` | s (path, empty = none) | '' |

Settings tab "Folder Preview": group *Layout* (layout, cycle on hover, photos per step — Stacked only),
group *Thumbnails* (border, border colour, shadow, random rotation), group *Folder Icon* (Stacked only:
folder icon and front overlay, each with Choose…/Clear). Everything waits for **Apply** (N5).

### 11.4 Decisions (owner, 2026-11-13)

| # | Decision |
|---|---|
| V1 | Style options apply to **both** layouts. |
| V2 | Stacked cycling step is a **user option** (one / three photos per step). |
| V3 | Folders without previews keep the **theme icon**, even with custom SVGs set. |
| V4 | ~~Same scale, anchored at the body's bottom centre~~ → (owner, 2026-11-13 follow-up) the overlay SVG is drawn at **the same size and place as the folder SVG** (the whole icon square); the SVG itself positions the overlay. |
| V5 | SVG rasterising runs on the main thread (GtkSvg is not thread-safe), once per (zoom, scale) per settings change; textures are cached and shared by all icons. |
| V6 | Nautilus 51.1 looks a plain folder's icon up as `inode-directory` before `folder` (verified: Adwaita and the owner's Hatter-Orange both resolve to `mimetypes/inode-directory.svg`), so both icon file names count as "the plain folder icon". |
| V7 | The disk cache keeps one directory per folder **per look** (style key in the hashed name, `CACHE_DISK_VERSION` v4), so switching back to a look is a disk hit. Directories of older versions (v3) are deleted by a low-priority worker job at start-up. |

### 11.5 Phases

| Phase | Work | Acceptance |
|---|---|---|
| 1 Core | Schema keys + `nu_settings`; `FpStyle` in `composite.c` (stacked cards, border/shadow/rotation); cache keyed by style (memory generation + disk path); stacked frame stepping; tests. | Tests + `check-safety.sh` pass. |
| 2 Icons & hook | `icon_art.c` (GtkSvg → textures, opaque bounds); paintable decor (back, frame, overlay); view hook: plain-`folder` filter, decor, live restyle, cycle off; tests. | Isolated Nautilus shows both layouts and the custom SVGs; `folder-*` folders untouched. |
| 3 Settings tab | Groups and rows of §11.3. | Every option applies live on Apply. |
| 4 Hardening & docs | Smoke rounds switching layouts/styles, `check-safety.sh`, README. | All PASS. |

### 11.5a Follow-up (owner, 2026-11-13)

1. Folder SVG and overlay SVG are drawn at the same size (V4 updated).
2. **Depth effect** (stacked, `folder-preview-depth`, default on): card *n* behind the front is darkened by
   `n·STACK_DEPTH_DIM` (0.16) and blurred with radius `n·STACK_DEPTH_BLUR` (0.6 % of the card width).
3. **Drop shadow** instead of a box shadow (both layouts): each photo/card is drawn into its own surface; its
   shadow is that surface's alpha, blurred (3 × box blur ≈ Gaussian) and offset down, so a transparent
   picture (an SVG logo) casts the shadow of its shape. Dimming uses `ATOP`, so transparent parts stay
   transparent.
4. **Keep Aspect Ratio** (`folder-preview-keep-aspect`, default on, both layouts): grid on = v1 fit, off =
   fill the section (cropped); stacked on = the card takes the picture's shape (fitted in the card rectangle,
   centred, top-aligned so the stack's top edges stay in order), off = the picture fills the card (cropped).

`CACHE_DISK_VERSION` v5 (v4 is purged at start-up). Tests: keep-aspect (grid fill, stacked card shape),
depth (darker, soft edge, sharp front), drop-shadow shape (no shadow under a transparent half).

### 11.5b Second follow-up (owner, 2026-11-13)

Sizes the owner gives "at 64 × 64" are fractions of the icon (`REF_ICON_PX` 64), so they scale with every
zoom level; the compositor converts them with `fp_composite_icon_px` (each layout's frame covers a fixed
part of the icon).

1. **Depth presets** (replace the depth switch): *None* (STACK_DIM per card), *Blur* (the previous effect),
   *Transparency* (opacity −30 % per card further back, at least 10 %), *Darken* (−22 % brightness per
   card, no blur). The front card is always fully visible.
2. **Bottom fade** (0–100 % of the stack height): the stack's cards are drawn into a group and painted
   through a linear alpha mask (opaque above the fade, transparent at the stack's bottom edge), like the
   sliding eject button's fade.
3. **Stack 8 px from the sides** at 64 × 64 (`STACK_SIDE`): the stack width is now 0.75 of the icon, no
   longer measured from the folder body.
4. **Position** *Top* / *Bottom* (8 px from the icon's top / bottom edge) / *Centre* (the previous
   placement inside the folder body), plus a **vertical offset** slider (±16 px). Placement only moves the
   frame (its size is the same everywhere), so it never rebuilds pictures (the "icons" settings group).
5. **Corner radius** of each picture (both layouts, default 2 px ≈ the previous stacked rounding; the grid's
   photos were square before).
6. **View top / bottom radius:** the stack area (the cards' untilted bounding box) clips all pictures, with
   separate top and bottom corner radii; both 0 = no clipping (tilted corners and shadows may then poke out).

`CACHE_DISK_VERSION` v6. Tests: depth presets, fade profile, clipping (nothing outside the area, rounded vs
square bottom corners), corner radius, 8 px sides, placement (top / bottom / offset, frame size unchanged).

### 11.5c Third follow-up (owner, 2026-11-14)

1. The 8 px side padding is a setting (**Side Padding** slider, 0–20 px at 64 × 64, default 8): stack width =
   1 − 2 × padding / 64 of the icon (`fp_style_stack_width`). It changes the frame's shape relative to the
   icon, so it is part of the style (cache key). Top / bottom positions stay 8 px from the icon's edge.
2. The front card is never tilted; with Random Rotation the cards behind it still are.

`CACHE_DISK_VERSION` v7. Tests: upright front card (identical with rotation on/off), the card behind tilts,
padding widths and clamping, placement with padding 0 / 16.

### 11.5d Border width (owner, 2026-11-14)

**Border Width** slider (both layouts, active while Border is on): 0.5–6 px at 64 × 64 in 0.5 steps (a whole
pixel at 64 × 64 is 4 px at the largest zoom, too coarse), default 1. Drawn in whole device pixels, at least
1; it replaces the previous widths (4.5 % of a grid section, 2.2 % of a card), which were about 1 px at
64 × 64. Stored in `FpStyle.border_width` as tenths (integer key/equality). `CACHE_DISK_VERSION` v8.

### 11.6 Phase results (2026-11-13)

- **Phase 1:** schema keys + `nu_settings` (two new change groups: style, icons/cycle); `FpStyle` in
  `composite.c` (stacked cards measured from the mock, border/colour/shadow/rotation for both layouts);
  cache keyed by style (memory generation drops results of older looks; disk directory per look, v4);
  stacked stepping with wrap-around. Tests: style helpers, grid options, stacked cards (order, insets,
  dimming, blank cards, determinism), cache styles and few-photo stacks.
- **Phase 2:** `icon_art.c` (GtkSvg, parse errors rejected — GtkSvg reports "not XML" as
  `GTK_SVG_ERROR_INVALID_SYNTAX`; Illustrator's `data-name` attributes are harmless notices); paintable
  decor (back → frame → overlay, custom SVGs only with a frame); view hook: plain-folder filter (V6), decor,
  live restyle, cycle off. Verified in an isolated Nautilus with the owner's SVGs: result matches
  `Asset-3`; Pictures (`folder-pictures`) and folders without media keep their icons.
- **Phase 3:** settings tab groups Layout / Thumbnails / Folder Icon (stacked only), Choose…/Clear rows with
  a GtkFileDialog (SVG filter); everything pending until Apply.
- **Phase 4:** smoke `look_round` (every option while browsing, an invalid SVG, off/on, reset) plus random
  layout switches. Smoke harness fixes: the isolated `XDG_DATA_HOME` hid the user's icon themes (GTK then
  used its built-in `folder-symbolic`, which gets no previews), so `~/.local/share/icons` is linked in; the
  smoke now fails if no self-check ever sees a drawn preview; `SMOKE_XDG_CACHE` is quoted (paths with
  spaces). New thumbnails can't be generated under the ASan preload (thumbnailer children inherit it), so
  the smoke tree's thumbnails are made first by a normal build and passed with `SMOKE_XDG_CACHE`.
  Old v3 disk cache directories are removed by a start-up worker job (V7).
- Smoke (ASan, 150 s + every round, 46 photo folders): `smoke-20261113-211922` **PASS** — previews drawn in 53/54
  self-checks, 0 wrong, 27 off/on toggles, 8 hovers.
Safety: strict, `-fanalyzer`, scan-build, ASan+UBSan, valgrind all PASS.

Follow-up (§11.5a) results: smoke `smoke-20261113-223428` **PASS** — previews drawn in 58/58 self-checks, 0 wrong (look round
now also switches keep-aspect and depth). scan-build flagged reading the blur line buffer as possibly
uninitialised (a false positive: every read index is below the filled length); fixed by zero-initialising
the buffer, not suppressed. Safety: strict, `-fanalyzer`, scan-build, ASan+UBSan, valgrind all PASS.

Second follow-up (§11.5b) results: smoke `smoke-20261113-234528` **PASS** — previews drawn in 66/66 self-checks, 0 wrong
(look round now also cycles the depth presets, fade, view radii, corner radius, positions and offsets).
Safety: strict, `-fanalyzer`, scan-build, ASan+UBSan, valgrind all PASS.

Third follow-up (§11.5c) results: smoke `smoke-20261114-001626` **PASS** — previews drawn in 68/68 self-checks, 0 wrong.
Safety: strict, `-fanalyzer`, scan-build, ASan+UBSan, valgrind all PASS.

Border width (§11.5d) results: smoke `smoke-20261114-005830` **PASS** — previews drawn in 70/70 self-checks, 0 wrong.
Safety: strict, `-fanalyzer`, scan-build, ASan+UBSan, valgrind all PASS.

Smoke harness (2026-11-14): the test session now gets a private `XDG_RUNTIME_DIR` (a temp dir under the real
one, Wayland reached by its absolute socket path). With the shared runtime dir, the private session's
xdg-document-portal unmounted the user's `/run/user/1000/doc` on exit, and every Flatpak app then failed to
start ("bwrap: Can't find source path /run/user/1000/doc/by-app/…"; fixed for the user by restarting
`xdg-document-portal.service`). Verified: after a smoke run the user's portal mount is intact and Flatpak
apps start.


---

## 12. Feature 5 — Folder icons by name

The owner (a web developer) has many `node_modules` folders and a custom icon for them; setting it on each
folder by hand (Properties → custom icon) is a hassle. With this feature the user adds **rules**: a folder
name and an icon. Every folder **everywhere** whose name matches a rule exactly gets that icon, applied
**natively**: Files' own custom-icon setting is written on the folder, exactly as Properties does. Prefix
`fi_`, log domain `folder-icons`, dir `src/folder-icons/`, branch `dev-path-icon`.

### 12.1 Behaviour

- **Matching:** the folder's name equals the rule's name exactly (case-sensitive, whole name):
  `node_modules` matches `node_modules`, not `Node_Modules` or `node_modules_old`. Folders only, never files.
  Any location Files lists (local disks, network shares, …); where a location can't store metadata (some
  network shares, §10.7) the folder simply keeps its normal icon.
- **Applying (native, owner I2):** Files calls our info provider for every file it lists. For a folder whose
  name matches a rule, we read its custom icon and our marker in the background and, if needed, write
  Files' custom icon (`metadata::custom-icon`, the attribute Properties writes; Phase 0 confirms the name
  and format) plus our marker `metadata::nautilus-utils-icon-rule` (which rule and icon we set). Files then
  shows the icon everywhere it shows custom icons (grid, list, path bar, Properties). It stays if the
  extension is removed, like any custom icon.
- **The user's own custom icons are never touched:** a folder with a custom icon but no marker of ours is
  left alone. Only folders carrying our marker are ever updated or cleared.
- **Changing rules:** when a rule's icon changes, is removed, or the feature is switched off, a folder we
  marked is updated (new icon) or restored (custom icon and marker removed) the next time Files lists it.
  Folders that are never listed again keep the last icon (they can't be found without scanning the disk).
  If the user changed such a folder's icon by hand afterwards, only our marker is removed.
- **Icons:** SVG or PNG files (owner I1), picked with a file chooser. The chosen file is **copied** into
  `$XDG_DATA_HOME/nautilus-utils/folder-icons/` (content-hash name), so moving or deleting the original
  doesn't break the folders; the custom icon points at the copy.
- **Folder previews:** a folder with a custom icon gets no preview (§11.1 already), so rule folders show
  only their icon.

### 12.2 Settings tab "Folder Icons"

- Switch row **"Icons by Folder Name"** (feature on/off).
- Group "Rules": one row per rule — the folder name (editable entry), the icon (a preview; click to choose
  another file), and a remove button — plus an **"Add Rule…"** button row. Duplicate names: the first rule
  wins; the row shows a warning. Empty names are ignored.
- Like every setting, changes wait for **Apply** (N5).

### 12.3 Rules

1. All §4 rules. Never block the main thread: metadata reads/writes are async (`g_file_query_info_async`,
   `g_file_set_attributes_async`) with a `GCancellable`; the info provider only checks the name (a string
   compare against a hash set) and queues work.
2. Never write to a folder that doesn't match a rule or carry our marker. Never overwrite a custom icon
   without our marker. Each folder is checked at most once per rule change (an in-memory set of handled
   URIs, bounded) so listing a folder repeatedly costs nothing.
3. Bounded concurrency for metadata I/O (a big tree with thousands of `node_modules` entries must not flood
   gvfsd-metadata).
4. Nautilus-free core (`fi_rules.c`: parsing, matching, the decide-what-to-do logic) with unit tests; the
   GIO glue in `fi_apply.c`; the info provider in `fi_provider.c`.

### 12.4 Settings keys (added to `io.github.zyrotec.NautilusUtils`)

| Key | Type | Default |
|---|---|---|
| `folder-icons-enabled` | b | true |
| `folder-icon-rules` | a(ss) — (folder name, stored icon file path) | [] |
| `folder-icon-rule-history` | as — names of removed rules (restore only), max 100 | [] |

### 12.5 Decisions (owner, 2026-11-14)

| # | Decision |
|---|---|
| I1 | Icons are SVG or PNG files chosen with a file chooser. |
| I2 | Applied **natively**: Files' own custom-icon metadata on each matching folder (not a display-time swap). |
| I3 | Matches folders in every location Files lists (where metadata can be stored). |
| I4 | Shown wherever Files shows custom icons (the owner asked for grid and list view; native icons also show in the path bar and Properties). |
| I5 | Live update of folders on screen by a no-op access-time nudge (§12.7 item 3). |

### 12.6 Phases

| Phase | Work | Acceptance |
|---|---|---|
| 0 Spike | Which attribute and value format Files 51.1 reads for a folder's custom icon; whether Files picks up a custom icon written by another process while the folder is shown (or what makes it refresh); that the info provider is called for every listed folder (grid, list, search results) and how often; metadata read/write latency on ext4/btrfs, NTFS (`ntfs3`) and a network share. | Every question answered here; anything infeasible reported before building. |
| 1 Core | Schema keys, `fi_rules.c` (parse, match, decide), icon copy store, unit tests. | Tests + `check-safety.sh` pass. |
| 2 Apply | Info provider + async apply/restore with markers, bounded I/O, live rule changes. | Every matching folder gets the icon in grid and list view; own custom icons untouched; rule removal restores; smoke clean. |
| 3 Settings tab | "Folder Icons" tab (§12.2). | Rules editable; Apply applies. |
| 4 Hardening & docs | Smoke round (rules added/changed/removed while browsing a tree of many `node_modules`), `check-safety.sh`, README. | All PASS. |

### 12.7 Phase 0 findings (2026-11-14, Nautilus 51.1 / GTK 4.24.1)

Isolated Nautilus (own D-Bus session, private `XDG_RUNTIME_DIR`, own data/config/cache), test trees on ntfs3
(the project drive) and tmpfs.

1. **Attribute:** Files 51.1 reads `metadata::custom-icon` (a URI, e.g. `file:///…/icon.svg`; SVG works) and
   `metadata::custom-icon-name` (a theme icon name). Writing `metadata::custom-icon` from another process
   shows the icon natively the next time Files reads the folder's info.
2. **No live pick-up on its own:** a folder already on screen keeps its old icon after an external metadata
   write (gvfs metadata changes raise no file monitor event).
3. **Refresh nudge (decision I5):** setting the folder's access time to **its current value**
   (`utimensat` with the exact current atime and `UTIME_OMIT` for mtime) raises an inotify `IN_ATTRIB` event
   (verified on tmpfs and ntfs3); Files' directory monitor then re-reads that folder's info and shows the new
   icon at once (verified on both). Nothing visible changes: atime keeps its value, mtime is untouched (so
   folder previews and backups keyed on mtime are unaffected); only the inode's ctime moves, as with any
   metadata change. Non-local folders (no path) get no nudge: their icon appears the next time Files lists
   them.
4. **Revisiting** a folder re-reads metadata: an icon written while the folder was off screen shows on the
   next visit.
5. **Calls:** the info provider is called for every file Files lists (D3), so matching folders are seen in
   grid, list and search results; `nautilus_file_info_invalidate_extension_info()` (public) makes Files call
   it again for a file, which is how a rule change reaches folders already listed (they are tracked by weak
   reference, bounded).
6. The inotify nudge needs no Nautilus internals: the whole feature uses only public API
   (`NautilusInfoProvider`, `NautilusFileInfo`, GIO metadata) — no view hook.

Design refinements from the spike:

- **Which folders are checked:** names in the current rules **plus** names of removed rules (kept in a
  bounded `folder-icon-rule-history` key, max 100), so a folder we marked under a since-removed rule is
  still found and restored. Other folder names cost one hash lookup and no I/O.
- **Decision** (pure function, `fi_rules.c`): rule R matches the name (and the feature is on) · marker M
  (`1;<icon uri>`) · current custom icon C / icon name N:
  - M present, C ≠ M's icon → the user changed it: remove only M.
  - M present, C = M's icon: R with the same icon → nothing; R with another icon → set R's icon + M;
    no R → remove C and M (restore).
  - No M: R and no C and no N → set R's icon + M; otherwise nothing (no rule, or the user's own icon).
- After a write, the nudge (3) runs in a worker thread; at most `FI_MAX_INFLIGHT` (4) folders are processed
  at once, duplicates are merged.
- Stored icons are never deleted automatically: folders not visited since may still point at them.
