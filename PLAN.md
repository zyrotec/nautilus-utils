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
| 1 | Folder preview — Dolphin-style thumbnails on folder icons, cycling on hover | `src/folder-preview/` | **v1 done** (grid view) |
| 2 | Sidebar drives — usage bars on drive rows, OS drive named after the OS, network shares below a separator | `src/sidebar-drives/` | **planned** (§8), awaiting approval |

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
