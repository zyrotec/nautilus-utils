#!/usr/bin/env bash
# In-Nautilus smoke test (PLAN.md §4.3, smoke test): run a real Nautilus with an AddressSanitizer + UBSan build of
# the extension (plus the test-only debug driver), browse, quit, and fail if
#   - any sanitizer report points into our code, or
#   - the view-hook self-check ever finds a preview on the wrong folder ("debug verify: … wrong=N>0"), or
#   - our log domain printed a WARNING/CRITICAL, or
#   - the sidebar-drives hook never became active (no OS row in its self-dump).
#
# Usage:
#   tools/smoke-nautilus.sh [DIR]                     interactive: browse/scroll/hover/zoom, then press Enter
#   tools/smoke-nautilus.sh --auto SECS [DIR]         scripted: scroll jumps, zoom levels, hover/leave, then
#                                                     random browsing until SECS have passed
#
# SMOKE_XDG_CACHE=DIR: give the test Nautilus its own cache dir (thumbnails + frames), e.g. the benchmark's.
# SMOKE_FAKE_NETWORK=LABEL: show that local drive as a network share in the sidebar (exercises the network
#   list; the smoke test never ejects anything).
# DP_STALL_SAMPLER=1: log where slow details-pane main-thread steps were (stacks sampled every ms, PLAN.md §13.14).
# SMOKE_PERF=1: an optimised build WITHOUT sanitizers (timings only, e.g. the details pane's main-thread steps,
#   PLAN.md §13.9 Phase 5); sanitizer checks then find nothing by construction.
# SMOKE_ONLY=dp (--auto only): run just the details-pane round (fast iteration on that feature).
# SMOKE_TOGGLES=N (--auto only, default 25): switch each feature off and on N times through the settings
#   (PLAN.md §9) and check that nothing of ours is left while off and everything comes back when on.
#
# Folder views (PLAN.md §10, --auto): a test tree under the log dir (on the drive of this repo) gets per-folder
# views set like a user would, revisited, opened in a background tab, toggled off/on and reset; every tab dump
# must show its folder's view, and named checks (fv-expect) must match.
#
# Settings: the test Nautilus uses an in-memory GSettings backend and the build's schema, so the smoke test
# never changes your real settings. It also gets its own XDG_DATA_HOME (exported before dbus-run-session, so
# gvfsd-metadata uses it too): the folder views it saves never reach your real gvfs metadata.
#
# Audio: the private session has no sound server, and opening ALSA's PipeWire device there hangs, which froze the
# test Nautilus when the details pane stopped a video (PLAN.md §13.11). GStreamer uses its fake audio sink.
#
# Isolation: the test Nautilus runs in its own D-Bus session (dbus-run-session), so your own Nautilus
# windows are NOT closed and the scripted driver only acts on DIR (FP_DEBUG_ROOT). A test window does
# appear on screen. Nautilus loads build/libnautilus-utils.so via the dev symlink (README.md); the script
# rebuilds build/ with sanitizers and ALWAYS restores the normal build on exit. Your already-running
# Nautilus keeps the module it loaded at its start.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 2

AUTO=""
if [ "${1:-}" = "--auto" ]; then AUTO="${2:?--auto needs seconds}"; shift 2; fi
DIR="$(realpath "${1:-$HOME}")"

STAMP="$(date +%Y%m%d-%H%M%S)"
LOG="$ROOT/safety-logs/smoke-$STAMP"
CMD_DIR="$LOG/driver"
mkdir -p "$CMD_DIR"

# Private runtime dir for the test session (on the real runtime dir's tmpfs). The private D-Bus session starts
# its own portals; with the shared $XDG_RUNTIME_DIR its xdg-document-portal mounted over, and on exit
# unmounted, the user's /run/user/$UID/doc, after which every Flatpak app failed to start ("bwrap: Can't find
# source path /run/user/1000/doc/by-app/…"). Wayland is reached through the absolute socket path instead.
REAL_RUNTIME="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
TEST_RUNTIME="$(mktemp -d "$REAL_RUNTIME/nu-smoke.XXXXXX")" || exit 2
case "${WAYLAND_DISPLAY:-}" in
  "") TEST_WAYLAND="" ;;
  /*) TEST_WAYLAND="$WAYLAND_DISPLAY" ;;
  *)  TEST_WAYLAND="$REAL_RUNTIME/$WAYLAND_DISPLAY" ;;
esac

cleanup_runtime() {
  # Portals of the private session may leave their FUSE mount behind; only ever touch our own dir.
  if mountpoint -q "$TEST_RUNTIME/doc" 2>/dev/null; then fusermount3 -u "$TEST_RUNTIME/doc" 2>/dev/null || fusermount -u "$TEST_RUNTIME/doc" 2>/dev/null; fi
  rm -rf --one-file-system "$TEST_RUNTIME" 2>/dev/null
}

restore_build() {
  cleanup_runtime
  echo "Restoring normal build in build/ ..."
  meson configure build -Db_sanitize=none -Ddebug_driver=false -Dbuildtype=debug >/dev/null && ninja -C build >/dev/null ||
    echo "WARNING: could not restore build/; run: meson configure build -Db_sanitize=none -Ddebug_driver=false -Dbuildtype=debug && ninja -C build" >&2
}
trap restore_build EXIT

[ -f build/build.ninja ] || meson setup build >/dev/null || exit 2
if [ -n "${SMOKE_PERF:-}" ]; then
  echo "Building build/ optimised (no sanitizers) + debug driver, for timings ..."
  meson configure build -Db_sanitize=none -Dbuildtype=debugoptimized -Ddebug_driver=true >/dev/null || exit 2
else
  echo "Building build/ with address,undefined sanitizers + debug driver ..."
  meson configure build -Db_sanitize=address,undefined -Db_lundef=false -Ddebug_driver=true >/dev/null || exit 2
fi
ninja -C build >"$LOG/build.log" 2>&1 || { tail -30 "$LOG/build.log"; exit 2; }

ASAN_RT=""
if [ -z "${SMOKE_PERF:-}" ]; then
  ASAN_RT="$(ldd build/libnautilus-utils.so | awk '/libasan\.so/ {print $3}')"
  [ -f "$ASAN_RT" ] || { echo "cannot find ASan runtime (dnf install libasan)"; exit 2; }
fi

echo "Starting an isolated Nautilus on $DIR (logs: $LOG)"
FV_TREE="$LOG/fvtree"
# Folder icons by name (PLAN.md §12): projects with node_modules folders (and one "mine" with its own icon).
FI_TREE="$LOG/fitree"
for p in web-a web-b web-c; do mkdir -p "$FI_TREE/$p/node_modules/dep/node_modules" "$FI_TREE/$p/src"; done
for f in A B C D; do mkdir -p "$FV_TREE/$f/sub"; for i in 1 2 3 4 5 6; do : > "$FV_TREE/$f/$f-$i.txt"; done; done
# Details pane (PLAN.md §13): the fixtures, a picture folder (folder preview in the pane) and a big folder.
DP_TREE="$LOG/dptree"
mkdir -p "$DP_TREE/pics" "$DP_TREE/big"
cp "$ROOT"/tests/details-pane/data/{photo.jpg,pic.png,clip.webm,clip.mp4,tone.ogg,doc.pdf,notes.txt} "$DP_TREE/"
cp "$ROOT"/tests/details-pane/data/{photo.jpg,pic.png} "$DP_TREE/pics/"
(cd "$DP_TREE/big" && for i in $(seq -w 1 10000); do : > "f$i.txt"; done)
(
  cd "$LOG" || exit 1
  # Own gvfs metadata + data dir for the whole private session (daemons included).
  export XDG_DATA_HOME="$LOG/xdg-data"
  mkdir -p "$XDG_DATA_HOME"
  # The user's icon themes (read-only link): without them GTK falls back to its built-in folder-symbolic
  # icon, which (correctly) gets no previews (PLAN.md §11.1), and the preview checks would test nothing.
  [ -d "$HOME/.local/share/icons" ] && ln -s "$HOME/.local/share/icons" "$XDG_DATA_HOME/icons"
  export XDG_RUNTIME_DIR="$TEST_RUNTIME"
  [ -n "$TEST_WAYLAND" ] && export WAYLAND_DISPLAY="$TEST_WAYLAND"
  # LD_PRELOAD only for nautilus itself, not for the private dbus-daemon / portals.
  setsid dbus-run-session -- env \
    LD_PRELOAD="$ASAN_RT" \
    ASAN_OPTIONS="log_path='$LOG/asan':detect_leaks=1:verify_asan_link_order=0" \
    UBSAN_OPTIONS="log_path='$LOG/ubsan':print_stacktrace=1" \
    LSAN_OPTIONS="suppressions='$ROOT/tools/lsan.supp':print_suppressions=0" \
    G_MESSAGES_DEBUG="nautilus-utils folder-preview sidebar-drives settings folder-views folder-icons details-pane" \
    GSETTINGS_BACKEND=memory GSETTINGS_SCHEMA_DIR="$ROOT/build/data" \
    GST_PLUGIN_FEATURE_RANK="fakeaudiosink:MAX" \
    DP_STALL_SAMPLER="${DP_STALL_SAMPLER:-}" \
    SD_DEBUG_FAKE_NETWORK="${SMOKE_FAKE_NETWORK:-}" \
    FP_DEBUG_DIR="$CMD_DIR" FP_DEBUG_ROOT="$DIR" ${SMOKE_XDG_CACHE:+"XDG_CACHE_HOME=$SMOKE_XDG_CACHE"} \
    nautilus --new-window "$DIR" >"$LOG/nautilus.log" 2>&1 &
)

# The test Nautilus is the one carrying our FP_DEBUG_DIR. Never pick a process by "started after us": the
# user's own Files may start (D-Bus activation) at the same moment, and SIGUSR2 quits a Files without the debug
# driver.
test_pid() {
  local p
  for p in $(pgrep -x nautilus); do
    tr '\0' '\n' < /proc/"$p"/environ 2>/dev/null | grep -qxF "FP_DEBUG_DIR=$CMD_DIR" && { echo "$p"; return; }
  done
}
PID=""
for _ in $(seq 1 40); do
  sleep 0.5
  PID="$(test_pid)"
  [ -n "$PID" ] && break
done
[ -n "$PID" ] || { echo "Nautilus failed to start:"; tail -30 "$LOG/nautilus.log"; exit 1; }
BUS="$(tr '\0' '\n' < /proc/"$PID"/environ | sed -n 's/^DBUS_SESSION_BUS_ADDRESS=//p')"
echo "test Nautilus pid $PID"
# Commands go by SIGUSR2, which quits Nautilus until the extension's driver handles it: wait for the driver.
for _ in $(seq 1 60); do grep -q "debug driver enabled" "$LOG/nautilus.log" && break; sleep 0.5; done
grep -q "debug driver enabled" "$LOG/nautilus.log" || { echo "debug driver not loaded:"; tail -30 "$LOG/nautilus.log"; exit 1; }
sleep 3

cmd() { [ "$(test_pid)" = "$PID" ] || return 0; echo "$1" > "$CMD_DIR/cmd"; kill -USR2 "$PID" 2>/dev/null; sleep "${2:-1}"; }
DIR_URI="$(python3 -I -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).as_uri())' "$DIR")"
# Sidebar drives (PLAN.md §8): OS row, Home, network row, refresh; then back to DIR for the preview checks.
sidebar_round() {
  cmd sd-dump 0.3; cmd sd-click-os 1.5; cmd sd-open-home 1.5; cmd sd-click-net 1.5
  cmd sd-os-menu 0.6; cmd sd-os-menu-dump 0.3; cmd sd-os-menu-close 0.3
  cmd sd-refresh 1; cmd sd-dump 0.3; cmd "sd-open $DIR_URI" 2
}

# Settings (PLAN.md §9.5 Phase 3): live on/off of both features; the dialog opens and closes.
toggle_round() {
  cmd "st-set sidebar-drives-enabled false" 0.6; cmd sd-dump 0.3
  cmd "st-set folder-preview-enabled false" 0.6; cmd fp-state 0.3
  cmd "st-set sidebar-drives-enabled true" 0.8; cmd sd-dump 0.3
  cmd "st-set folder-preview-enabled true" 1.2; cmd fp-state 0.3; cmd verify 0.3
  cmd "st-set folder-views-enabled false" 0.6; cmd fv-dump 0.3
  cmd "st-set folder-views-enabled true" 1; cmd fv-dump 0.3
}

# Folder views (PLAN.md §10): user changes saved per folder, revisits, a background tab, off/on, Reset All.
# fv-expect <view> <zoom> checks the selected tab (view 1 = list, 2 = grid; zoom = Nautilus' level).
fv_open() { cmd "fv-open $FV_TREE/$1" "${2:-1.5}"; }
fv_round() {
  fv_open A; cmd fv-dump 0.3
  cmd "fv-view 1" 1; cmd "fv-zoom in" 0.8; cmd "fv-expect 1 2" 0.3     # A: list, large
  fv_open B; cmd "fv-expect 2 2" 0.3                                  # B: never changed → default (grid medium)
  cmd "fv-zoom out" 0.8; cmd "fv-expect 2 1" 0.3                      # B: grid small-plus
  fv_open C; cmd "fv-expect 2 2" 0.3
  fv_open A; cmd "fv-expect 1 2" 0.3                                  # revisit (cache)
  cmd fv-new-tab 1.5; cmd "fv-tab-open 0 $FV_TREE/B" 1.5; cmd fv-dump 0.3   # background tab
  cmd "st-set folder-views-enabled false" 0.8; cmd fv-dump 0.3
  fv_open D; cmd "fv-view 1" 1                                        # while off: not saved
  cmd "st-set folder-views-enabled true" 1.2; cmd fv-dump 0.3
  fv_open D; cmd "fv-expect 2 2" 0.3
  fv_open A; cmd "fv-expect 1 2" 0.3
  cmd fv-reset 1.2; cmd fv-dump 0.3; cmd "fv-expect 2 2" 0.3          # Reset All: default at once
  fv_open B; cmd "fv-expect 2 2" 0.3                                  # old value ignored
  cmd "sd-open $DIR_URI" 2
}

# Bar colours and show/hide (PLAN.md §9.5 Phase 5): every combination once, dumped for the visibility check.
style_round() {
  cmd "st-set bar-colour '#e01b24'" 0.3; cmd "st-set bar-colour-mode 'custom'" 0.5; cmd sd-dump 0.3
  cmd "st-set bar-colour-mode 'gradient'" 0.5; cmd sd-dump 0.3
  for b in false true; do for l in false true; do
    cmd "st-set show-usage-bars $b" 0.4; cmd "st-set show-size-labels $l" 0.5; cmd sd-dump 0.3
  done; done
  cmd "st-set bar-colour-mode 'accent'" 0.5; cmd sd-dump 0.3
}

# Sliding eject (PLAN.md §9.5 Phase 6): slide in/out, feature off/on while sliding, back to fixed.
eject_round() {
  cmd "st-set eject-slide true" 0.6; cmd sd-dump 0.3
  cmd "sd-eject-reveal 1" 0.5; cmd sd-dump 0.3; cmd "sd-eject-reveal 0" 0.5
  cmd "st-set sidebar-drives-enabled false" 0.6; cmd sd-dump 0.3
  cmd "st-set sidebar-drives-enabled true" 0.8; cmd sd-dump 0.3
  cmd "st-set eject-slide false" 0.6; cmd sd-dump 0.3
}

# Folder preview v2 (PLAN.md §11): every layout/style/icon option while scrolling, hovering and zooming,
# including an SVG that isn't one; every change must redraw the right folder's preview (verify).
FP_SVG_DIR="$ROOT/tests/folder-preview/data"
look_round() {
  cmd "st-set folder-preview-layout 'stacked'" 1.2; cmd verify 0.3; cmd hover 1.6; cmd leave 0.3
  cmd "st-set folder-preview-folder-icon '$FP_SVG_DIR/back.svg'" 1; cmd verify 0.3
  cmd "st-set folder-preview-overlay-icon '$FP_SVG_DIR/overlay.svg'" 1; cmd verify 0.3
  cmd "action view.zoom-in" 1.5; cmd verify 0.3; cmd hover 1.6; cmd leave 0.3
  cmd "st-set folder-preview-stack-step 'three'" 1; cmd hover 2.2; cmd leave 0.3
  cmd "st-set folder-preview-border false" 0.8; cmd "st-set folder-preview-shadow false" 0.8
  cmd "st-set folder-preview-rotation false" 0.8; cmd "st-set folder-preview-border-colour '#e01b24'" 0.8
  cmd "st-set folder-preview-border true" 1; cmd verify 0.3
  cmd "st-set folder-preview-border-width 4.5" 0.8; cmd "st-set folder-preview-border-width 0.5" 0.8; cmd verify 0.3
  cmd "st-set folder-preview-keep-aspect false" 1; cmd verify 0.3
  for d in none transparency darken blur; do cmd "st-set folder-preview-depth-effect '$d'" 0.8; done
  cmd "st-set folder-preview-fade 60" 0.8; cmd "st-set folder-preview-view-top-radius 6" 0.8
  cmd "st-set folder-preview-view-bottom-radius 10" 0.8; cmd "st-set folder-preview-corner-radius 9" 0.8
  for p in top bottom centre; do cmd "st-set folder-preview-stack-position '$p'" 0.6; cmd verify 0.3; done
  cmd "st-set folder-preview-stack-padding 0" 0.8; cmd "st-set folder-preview-stack-padding 20" 0.8; cmd verify 0.3
  cmd "st-set folder-preview-stack-offset -12" 0.6; cmd "st-set folder-preview-stack-offset 16" 0.6; cmd verify 0.3
  cmd "st-set folder-preview-layout 'grid'" 1.2; cmd verify 0.3; cmd "st-set folder-preview-layout 'stacked'" 1.2
  cmd "st-set folder-preview-folder-icon '$FP_SVG_DIR/notes.txt'" 1; cmd verify 0.3   # not an SVG: theme icon
  cmd "st-set folder-preview-cycle false" 0.6; cmd hover 1.6; cmd leave 0.3
  cmd "st-set folder-preview-enabled false" 0.8; cmd fp-state 0.3
  cmd "st-set folder-preview-enabled true" 1.2; cmd fp-state 0.3; cmd verify 0.3
  cmd "action view.zoom-standard" 1.5
  for k in folder-preview-layout folder-preview-cycle folder-preview-stack-step folder-preview-border \
           folder-preview-border-colour folder-preview-shadow folder-preview-rotation \
           folder-preview-keep-aspect folder-preview-depth-effect folder-preview-corner-radius folder-preview-fade \
           folder-preview-view-top-radius folder-preview-view-bottom-radius folder-preview-stack-position \
           folder-preview-stack-offset folder-preview-stack-padding folder-preview-border-width folder-preview-folder-icon folder-preview-overlay-icon; do
    cmd "st-reset $k" 0.4
  done
  cmd verify 0.5
}

# Folder icons (PLAN.md §12): add a rule while the folders are on screen, change its icon, remove it,
# switch the feature off/on; afterwards no folder may keep our metadata (checked below with gio).
FI_SVG_DIR="$ROOT/tests/folder-preview/data"
FI_URI="$(python3 -I -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).as_uri())' "$FI_TREE")"
icons_round() {
  cmd "sd-open $FI_URI" 1.5; cmd fi-dump 0.3
  cmd "st-set folder-icon-rules [('node_modules', '$FI_SVG_DIR/back.svg')]" 1
  cmd "sd-open $FI_URI/web-a" 1.5; cmd "sd-open $FI_URI/web-a/node_modules/dep" 1.5; cmd fi-dump 0.3
  cmd "st-set folder-icon-rules [('node_modules', '$FI_SVG_DIR/overlay.svg'), ('src', '$FI_SVG_DIR/back.svg')]" 1
  cmd "sd-open $FI_URI/web-b" 1.5; cmd fi-dump 0.3
  cmd "st-set folder-icons-enabled false" 1; cmd "sd-open $FI_URI/web-a" 1.5; cmd fi-dump 0.3
  cmd "st-set folder-icons-enabled true" 1; cmd fi-dump 0.3
  cmd "st-set folder-icon-rules @a(ss) []" 1
  for p in web-a web-b web-c web-a/node_modules/dep; do cmd "sd-open $FI_URI/$p" 1.2; done
  cmd fi-dump 0.3
  cmd "st-reset folder-icon-rule-history" 0.5
  cmd "sd-open $DIR_URI" 2
}

# Details pane (PLAN.md §13): toggle, every kind of file, video play/stop, folder preview, several selected,
# nothing selected, list view, a second tab, width, feature off/on, a fast "arrow key" run through a big folder.
DP_URI="$(python3 -I -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).as_uri())' "$DP_TREE")"
dp_expect() { cmd "dp-expect $1" 0.3; }
dp_pick() { cmd "dp-select none" 0.3; cmd "dp-select name $1" "${2:-1.5}"; }
dp_round() {
  cmd "sd-open $DP_URI" 2; cmd "fv-view 2" 1.5
  cmd dp-dump 0.3
  cmd dp-toggle 1.5; cmd dp-dump 0.3
  dp_expect 'title="dptree"'; dp_expect 'Contents=9 items'
  dp_pick photo.jpg; dp_expect 'Camera=Canon EOS R6'; dp_expect 'Dimensions=120 × 80 px'
  dp_expect '[Permissions: You=Read,Write,;'
  dp_pick clip.webm 2.5; dp_expect 'Video codec=VP8'; dp_expect 'Audio codec=Vorbis'
  cmd dp-play 2.5; cmd dp-dump 0.3; dp_expect 'playing=1'
  cmd dp-stop 0.5; dp_expect 'playing=0'
  cmd dp-play 1.5; dp_pick clip.mp4 2.5; dp_expect 'playing=0'; dp_expect 'Video codec=H.264'
  cmd dp-play 2; cmd dp-toggle 1; cmd dp-dump 0.3; cmd dp-toggle 1.5; dp_expect 'playing=0'
  dp_pick tone.ogg; dp_expect 'Codec=Vorbis'; dp_expect 'Title=Test Tone'
  dp_pick doc.pdf; dp_expect 'Pages=2'; dp_expect 'Page size=A4'
  dp_pick pics 3; dp_expect 'Contents=2 items'; dp_expect 'preview=folder-preview'
  cmd "dp-select all" 2; dp_expect 'items selected'; dp_expect 'preview=stack'
  cmd "dp-select none" 1.5; dp_expect 'title="dptree"'
  cmd "fv-view 1" 2; dp_pick photo.jpg; dp_expect 'Camera=Canon EOS R6'; cmd "fv-view 2" 2
  # A second tab, then back to the first (not closed: closing a tab makes the owner's Python extension keep GTK
  # objects alive at exit, which LeakSanitizer reports under our test driver's frame — PLAN.md §13.9 Phase 5).
  cmd fv-new-tab 2; cmd dp-dump 0.3; cmd "dp-tab 0" 1.5; cmd dp-dump 0.3; dp_expect 'title="photo.jpg"'
  cmd "dp-width 400" 1; cmd dp-dump 0.3; cmd "dp-width 320" 1
  cmd st-open 1; cmd "st-tab details-pane" 1; cmd st-dump 0.3; cmd "st-tab git" 1; cmd st-dump 0.3; cmd st-close 0.5
  # v2 look (PLAN.md §13.10): stack options, a floating card, the header line, the bottom strip.
  cmd "dp-select all" 2; dp_expect 'preview=stack'
  cmd "st-set details-pane-stack-depth-effect 'darken'" 1; cmd "st-set details-pane-stack-card false" 1.5
  dp_expect 'preview=stack'
  cmd "st-set details-pane-margin 10" 0.5; cmd "st-set details-pane-radius 14" 0.5
  cmd "st-set details-pane-header-border true" 1; cmd dp-dump 0.3; cmd dp-geom 0.3
  cmd "st-set details-pane-margin 0" 0.5; cmd dp-geom 0.3; cmd "dp-png $LOG/dp-header-line.png" 1
  cmd "st-set details-pane-margin 10" 0.5; cmd "dp-png $LOG/dp-floating.png" 1
  # v3 (§13.12): icon size, snug media box, the Windows 7 summary at the bottom.
  cmd "st-set details-pane-icon-size 64" 1; dp_pick notes.txt 2; dp_expect 'box=icon'
  cmd "dp-select all" 2.5; dp_expect 'preview=stack'; cmd "dp-png $LOG/dp-stack.png" 1
  cmd "st-set details-pane-icon-size 256" 1.5; dp_expect 'preview=stack'
  # (Snug media boxes need thumbnails, which can't be made in this session, D8: checked with the demo.)
  # v4 (§13.13): centred name without icon, media in a fixed box, the folder previews' look, folder art in stacks.
  cmd "st-set details-pane-title-centered true" 0.5; cmd "st-set details-pane-show-icon false" 0.5
  cmd "st-set details-pane-media-fit 'box'" 0.5; dp_pick photo.jpg; dp_expect 'Camera=Canon EOS R6'
  cmd "dp-png $LOG/dp-centred.png" 1
  # v5 (§13.14): the stack is live layers; adding to a selection keeps it on screen (no reset in between).
  cmd "dp-select name photo.jpg" 0.5; cmd "dp-select add pic.png" 2; dp_expect 'stack=cards:2'
  cmd "dp-select add notes.txt" 0.05; dp_expect 'preview=stack'; cmd dp-dump 1.5; dp_expect 'stack=cards:3'
  cmd "st-set details-pane-stack-media-square true" 1; dp_expect 'stack=icons:3'
  cmd "st-set details-pane-stack-border true" 0.5; cmd "st-set details-pane-stack-border-colour '#e01b24'" 0.5
  cmd "st-set details-pane-stack-border-width 6.5" 0.5; cmd "st-set details-pane-stack-corner-radius 20" 0.5
  cmd "st-set details-pane-stack-fade 50" 0.5; cmd "st-set details-pane-stack-spacing 40" 0.5
  cmd "st-set details-pane-stack-media-square false" 1
  dp_expect 'stack=cards:3'
  cmd "dp-select all" 3; dp_expect 'stack-art=yes'; dp_expect 'stack=cards:3'; cmd "dp-png $LOG/dp-stack-mixed.png" 1
  # DIR's own picture folders (thumbnails already cached there): folders with their pictures, plain (no border).
  cmd "sd-open $DIR_URI" 3; cmd "st-reset details-pane-stack-spacing" 0.3
  cmd "dp-select name Wallpapers" 1; cmd "dp-select add Screenshots" 4; dp_expect 'stack=icons:2'
  dp_expect 'stack-art=yes'; cmd "dp-png $LOG/dp-folder-art.png" 1
  # Pictures with thumbnails: cards with the border; one picture with the blurred background.
  # Loading thumbnails runs GTK's image loader in child processes, which inherit the ASan preload and fail (D8):
  # the thumbnail checks run in SMOKE_PERF=1 runs only.
  cmd "sd-open $DIR_URI/Wallpapers" 3; cmd "dp-select all" 4; dp_expect 'stack=cards:3'
  [ -n "${SMOKE_PERF:-}" ] && dp_expect 'thumbs=3'
  cmd "dp-png $LOG/dp-stack-media.png" 1
  cmd "st-set details-pane-stack-media-square true" 2; dp_expect 'stack=icons:3'; cmd "dp-png $LOG/dp-stack-square.png" 1
  cmd "st-set details-pane-preview-backdrop true" 0.5; cmd "dp-select 0" 3
  [ -n "${SMOKE_PERF:-}" ] && dp_expect 'backdrop=yes'
  cmd "dp-png $LOG/dp-backdrop.png" 1; cmd "dp-select none" 1
  cmd "sd-open $DP_URI" 2
  for k in details-pane-title-centered details-pane-show-icon details-pane-media-fit details-pane-preview-backdrop \
           details-pane-stack-media-square details-pane-stack-border details-pane-stack-border-colour \
           details-pane-stack-border-width details-pane-stack-corner-radius details-pane-stack-fade \
           details-pane-stack-spacing; do
    cmd "st-reset $k" 0.4
  done
  dp_pick photo.jpg; cmd "st-set details-pane-position 'bottom'" 2; cmd dp-dump 0.3
  dp_expect '[Summary: Dimensions=120 × 80 px;'; dp_expect 'Camera=Canon EOS R6;'; cmd "dp-png $LOG/dp-bottom.png" 1
  dp_pick clip.webm 2; dp_expect 'Codec=VP8;'; dp_pick pics 2; dp_expect 'Contents=2 items;'
  cmd "dp-select all" 2.5; dp_expect '[Summary: Size='; dp_pick photo.jpg
  cmd "st-set details-pane-position 'right'" 2; cmd dp-dump 0.3
  for k in details-pane-stack-depth-effect details-pane-stack-card details-pane-margin details-pane-radius \
           details-pane-header-border details-pane-position details-pane-icon-size; do cmd "st-reset $k" 0.4; done
  cmd "dp-png $LOG/dp-right.png" 1
  cmd "st-set details-pane-enabled false" 1; cmd dp-dump 0.3
  cmd "st-set details-pane-enabled true" 2; cmd dp-dump 0.3; dp_expect 'title="photo.jpg"'
  # Holding an arrow key through a big folder: a selection change every 20 ms, then it settles.
  cmd "sd-open $DP_URI/big" 3
  for i in $(seq 0 150); do cmd "dp-select $i" 0.02; done
  cmd "dp-select 151" 2; cmd dp-dump 0.3; dp_expect 'title="f00152.txt"'; dp_expect 'pending=0'
  cmd "sd-open $DP_URI" 2; cmd dp-toggle 1; cmd dp-dump 0.3
  cmd "st-reset details-pane-visible" 0.5; cmd "st-reset details-pane-width" 0.5
  cmd "sd-open $DIR_URI" 2
}

# Git (PLAN.md §14): a repository with a local bare remote (file://, so push / pull / sync work offline) and a
# second clone that pushes, so the counts move. The status bar under the folder view: branch, counts, sync, pull,
# push, create + publish a branch, switch to a remote branch, a folder outside any repository, feature off / on.
GT_DIR="$LOG/gt"
GT_REPO="$GT_DIR/repo"
gt_make_repo() {
  ( set -e
    export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1 GIT_AUTHOR_NAME="Smoke Test" GIT_AUTHOR_EMAIL=smoke@example.invalid \
           GIT_COMMITTER_NAME="Smoke Test" GIT_COMMITTER_EMAIL=smoke@example.invalid
    mkdir -p "$GT_DIR" && cd "$GT_DIR"
    git init -q --bare -b main remote.git
    git clone -q remote.git seed 2>/dev/null && cd seed && git checkout -q -b main
    mkdir -p src && echo one > README.md && echo a > src/a.c && printf 'node_modules/\n*.log\n' > .gitignore
    git add -A && git commit -qm "Initial commit" && git push -q origin main
    git checkout -q -b feature && echo f > feature.txt && git add feature.txt && git commit -qm "Feature work" && git push -q origin feature
    cd .. && git clone -q -b main remote.git repo && git clone -q -b main remote.git other
    cd other && echo two >> README.md && git commit -qam "Update from elsewhere" && git push -q origin main
    cd "$GT_DIR" && git init -q -b main local && cd local && echo l > notes.txt && git add -A && git commit -qm "Local only"
    true ) || echo "gt: could not make the test repositories"
  # A folder outside any repository (the log folder is inside this checkout's own repository).
  GT_PLAIN="$(mktemp -d "${TMPDIR:-/tmp}/nu-gt-plain.XXXXXX")"
}
gt_expect() { cmd "gt-expect $1" 0.3; }
gt_round() {
  gt_make_repo
  local uri plain; uri="$(python3 -I -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).as_uri())' "$GT_REPO")"
  plain="$(python3 -I -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).as_uri())' "$GT_PLAIN")"
  cmd "sd-open $uri" 3; cmd gt-dump 0.3
  gt_expect 'bar=1'; gt_expect 'branch="main"'; gt_expect 'counts="↑0 ↓0"'; gt_expect 'remote=origin'
  cmd "dp-png $LOG/gt-bar.png" 1
  cmd gt-sync 3; cmd gt-dump 0.3; gt_expect 'counts="↑0 ↓1"'; gt_expect 'progress="Sync: done"'
  cmd gt-pull 3; gt_expect 'counts="↑0 ↓0"'; gt_expect 'progress="Pull: done"'
  ( cd "$GT_REPO" && GIT_CONFIG_GLOBAL=/dev/null GIT_AUTHOR_NAME=S GIT_AUTHOR_EMAIL=s@x.invalid GIT_COMMITTER_NAME=S \
      GIT_COMMITTER_EMAIL=s@x.invalid git commit -q --allow-empty -m "Local work" )
  cmd gt-dump 1.5; gt_expect 'counts="↑1 ↓0"'                      # seen through the git folder monitor
  cmd gt-push 3; gt_expect 'counts="↑0 ↓0"'; gt_expect 'progress="Push: done"'
  cmd "gt-create topic/smoke" 2.5; gt_expect 'branch="topic/smoke"'; gt_expect 'counts="Not published"'
  cmd gt-push 3; gt_expect 'counts="↑0 ↓0"'; gt_expect 'progress="Push: done"'
  cmd "gt-switch-remote origin/feature" 2.5; gt_expect 'branch="feature"'; gt_expect 'counts="↑0 ↓0"'
  cmd "gt-switch no-such-branch" 2; gt_expect 'progress="Switch to no-such-branch: failed"'; gt_expect 'branch="feature"'
  cmd "gt-switch main" 2.5; gt_expect 'branch="main"'
  cmd dp-toggle 1.5; cmd "dp-png $LOG/gt-bar-pane.png" 1
  # The details pane's Details | Git (§14.2 item 4, §14.11): the branch card, the history (graph / list), the hover card.
  cmd "dp-select none" 1; dp_expect 'view=details'; cmd "dp-extra on" 2; dp_expect 'view=extra'
  dp_expect 'card=[main|Local work|S · '
  dp_expect 'Mainline: origin/main · 0 ahead, 0 behind'; dp_expect 'graph=1 commits=3 lanes=1'
  dp_expect 'commit0=Local work{main}'; dp_expect 'commit1=Update from elsewhere commit2=Initial commit'
  cmd "dp-select name README.md" 2; dp_expect '|README.md · Unchanged]'
  cmd "dp-png $LOG/gt-pane-git.png" 1
  cmd "gt-card 1" 2.5; dp_expect 'hover=[Update from elsewhere|'; dp_expect '1 file changed ·|+1|−0|'
  cmd "dp-png $LOG/gt-pane-card.png" 1
  ( cd "$GT_REPO" && echo y >> README.md ); cmd "dp-select none" 0.5; cmd "dp-select name README.md" 2
  dp_expect '|README.md · Modified]'
  cmd "dp-select name src" 0.5; cmd "dp-select add README.md" 2; dp_expect '|2 selected · 1 with changes]'
  cmd "st-set git-history-graph false" 2; dp_expect 'graph=0 commits=3'; cmd "dp-png $LOG/gt-pane-list.png" 1
  cmd "st-reset git-history-graph" 2; dp_expect 'graph=1'
  cmd "st-set details-pane-position 'bottom'" 1.5; dp_expect 'view=details-only'
  gt_expect 'at=bottom'; cmd "dp-png $LOG/gt-bar-bottom.png" 1                            # below the strip
  cmd "st-set details-pane-position 'right'" 1.5; dp_expect 'view=extra'; gt_expect 'at=column'
  cmd "dp-extra off" 1; dp_expect 'view=details'; cmd "dp-extra on" 1
  cmd "sd-open $uri" 2; cmd "sd-open $plain" 2; dp_expect 'view=details-only'                # outside: no toggle
  cmd "sd-open $uri" 2; cmd "dp-select none" 1; dp_expect 'view=extra'                        # the choice is kept
  ( cd "$GT_REPO" && git checkout -q -- README.md )
  cmd "dp-extra off" 1; cmd "st-reset details-pane-position" 0.3; cmd dp-toggle 1
  # Git columns (§14.2 item 3): values Files has for each file, through its public API; changes made outside.
  gt_attr() { cmd "gt-attr $1" 0.3; }
  cmd gt-cols 0.5; cmd "fv-view 1" 3; cmd gt-dump 1; cmd "dp-png $LOG/gt-columns.png" 1
  gt_attr 'README.md status=|commit=Update from elsewhere|'; gt_attr 'README.md author=Smoke Test|'
  gt_attr 'src status=|commit=Initial commit|'
  ( cd "$GT_REPO" && echo u > untracked.txt && echo l > debug.log && echo m >> src/a.c && echo x >> README.md )
  cmd gt-dump 3
  gt_attr 'untracked.txt status=Untracked|commit=|'; gt_attr 'debug.log status=Ignored|'
  gt_attr 'README.md status=Modified|'; gt_attr 'src status=Contains changes|'
  ( cd "$GT_REPO" && GIT_CONFIG_GLOBAL=/dev/null GIT_AUTHOR_NAME="Col Test" GIT_AUTHOR_EMAIL=c@x.invalid \
      GIT_COMMITTER_NAME="Col Test" GIT_COMMITTER_EMAIL=c@x.invalid git commit -qam "Commit from a terminal" )
  cmd gt-dump 3                                                                   # the repository changed
  gt_attr 'README.md status=|commit=Commit from a terminal|'; gt_attr 'README.md author=Col Test|'
  gt_attr 'src status=|commit=Commit from a terminal|'; gt_attr 'untracked.txt status=Untracked|'
  cmd "fv-view 2" 2
  # A repository without any remote (`git remote` prints nothing): the bar, not a crash (owner's crash 2026-11-15).
  local local_uri; local_uri="$(python3 -I -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).as_uri())' "$GT_DIR/local")"
  cmd "sd-open $local_uri" 2.5; cmd gt-dump 0.3; gt_expect 'bar=1'; gt_expect 'branch="main"'; gt_expect 'remote=-'
  gt_expect 'counts="Not published"'
  cmd "sd-open $uri/src" 2; gt_expect 'bar=1'; gt_expect 'branch="main"'          # a folder inside
  cmd "sd-open $plain" 2; cmd gt-dump 0.3; gt_expect 'bar=0'                       # outside any repository
  cmd "sd-open $uri" 2; gt_expect 'bar=1'
  cmd "st-set git-status-bar false" 1; gt_expect 'bar=0'
  cmd "st-set git-status-bar true" 1; gt_expect 'bar=1'
  cmd "st-set git-enabled false" 1; cmd gt-dump 0.3
  cmd "st-set git-enabled true" 2; gt_expect 'bar=1'; gt_expect 'branch="main"'
  # Error state and loading (owner, 2026-11-15): a broken index makes `git status` fail → "Git error" with a refresh
  # button; repaired + refresh → back. A repository whose first read takes seconds (a slow fsmonitor hook) shows a
  # spinner while it loads.
  cmd "sd-open $uri" 2.5; gt_expect 'loading=0'
  ( cd "$GT_REPO" && cp .git/index "$GT_DIR/index.good" && printf 'garbage' > .git/index )
  cmd gt-dump 2; gt_expect 'branch="Git error"'; gt_expect 'refresh=1'
  cp "$GT_DIR/index.good" "$GT_REPO/.git/index"; cmd gt-refresh 2.5; gt_expect 'branch="main"'; gt_expect 'refresh=0'
  # (The hook goes in the temp folder: git runs it through the shell, and this checkout's path has a space.)
  local hook_dir; hook_dir="$(mktemp -d "${TMPDIR:-/tmp}/nu-gt-hook.XXXXXX")"
  printf '#!/bin/sh\nsleep 4\nexit 1\n' > "$hook_dir/slow-fsmonitor"; chmod +x "$hook_dir/slow-fsmonitor"
  ( cd "$GT_DIR" && git clone -q remote.git slow && git -C slow config core.fsmonitor "$hook_dir/slow-fsmonitor" )
  local slow_uri; slow_uri="$(python3 -I -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).as_uri())' "$GT_DIR/slow")"
  cmd dp-toggle 1.5; cmd "dp-extra on" 1
  cmd "sd-open $slow_uri" 1.5; cmd gt-dump 0.3; gt_expect 'loading=1'; gt_expect 'branch="Loading…"'
  dp_expect '[extra: loading=1 refresh=0 spinner=0 card=[Loading…|'   # (the history, a separate read, is already in)
  cmd gt-dump 12; gt_expect 'loading=0'; gt_expect 'branch="main"'
  dp_expect '[extra: loading=0 refresh=0 spinner=0 card=[main|'
  cmd "dp-extra off" 1; cmd dp-toggle 1
  rm -rf "$hook_dir"
  # Clone Repository… (§14.2 items 5–6): into a folder outside any repository (the bar shows the clone there), the
  # dialog's checks, then from a local smart-HTTP server with basic auth through the sign-in dialog.
  # The driver splits "URL NAME" at a space, and this checkout's path has one: clone from a copy in the temp folder.
  local src; src="$(mktemp -d "${TMPDIR:-/tmp}/nu-gt-src.XXXXXX")"; git clone -q --bare "$GT_DIR/remote.git" "$src/remote.git"
  cmd "sd-open $plain" 2; cmd gt-clone-open 1
  cmd "gt-clone not-a-url" 0.5; cmd "gt-clone $src/remote.git" 0.5           # a path, not a URL: refused
  cmd "gt-clone file://$src/remote.git" 4; gt_expect 'bar=1'; gt_expect 'progress="Clone remote: done"'; gt_expect 'selected=remote'
  [ -f "$GT_PLAIN/remote/README.md" ] && echo "gt clone: local OK" >> "$LOG/gt-checks" || echo "gt clone: local MISSING" >> "$LOG/gt-checks"
  cmd gt-clone-open 1; cmd "gt-clone file://$src/remote.git" 0.5            # "remote" exists now: refused
  ( cd "$GT_DIR" && git clone -q --bare remote.git srv/repo.git && git -C srv/repo.git config http.receivepack true )
  local port_file="$GT_DIR/srv.port"
  python3 -I "$ROOT/tests/git/data/authserver.py" "$GT_DIR/srv" > "$port_file" 2>/dev/null &
  local srv_pid=$!
  for _ in 1 2 3 4 5 6 7 8 9 10; do [ -s "$port_file" ] && break; sleep 0.3; done
  local port; port="$(head -1 "$port_file")"
  cmd "gt-clone http://127.0.0.1:$port/repo.git authed" 4
  cmd "gt-auth-expect Sign In to 127.0.0.1"; cmd "gt-auth u bad" 4
  cmd "gt-auth-expect The remote refused them."; cmd "gt-auth u tok" 5
  gt_expect 'progress="Clone authed: done"'; cmd gt-dump 1; gt_expect 'selected=authed'
  [ -f "$GT_PLAIN/authed/README.md" ] && echo "gt clone: authed OK" >> "$LOG/gt-checks" || echo "gt clone: authed MISSING" >> "$LOG/gt-checks"
  kill "$srv_pid" 2>/dev/null
  cmd "sd-open $DIR_URI" 2
  rm -rf "$GT_PLAIN" "$src"
}

if [ -n "$AUTO" ] && [ "${SMOKE_ONLY:-}" = gt ]; then
  gt_round
elif [ -n "$AUTO" ] && [ "${SMOKE_ONLY:-}" = dp ]; then
  dp_round
elif [ -n "$AUTO" ]; then
  end=$(( $(date +%s) + AUTO ))
  dp_round
  gt_round
  icons_round
  look_round
  style_round
  eject_round
  cmd st-dump 0.3; cmd st-open 1; cmd st-close 0.5
  fv_round
  for _ in $(seq 1 "${SMOKE_TOGGLES:-25}"); do toggle_round; done
  sidebar_round
  cmd verify 1
  for f in 0.5 1.0 0.25 0.75 0.0; do cmd "scroll $f" 2; cmd verify 0.3; done
  cmd hover 2.5; cmd leave 0.5
  for a in view.zoom-in view.zoom-in view.zoom-out view.zoom-out view.zoom-out view.zoom-standard; do
    cmd "action $a" 1.5; cmd "scroll 0.$((RANDOM % 10))" 1.5; cmd verify 0.3
  done
  look_round
  while [ "$(date +%s)" -lt "$end" ]; do
    case $((RANDOM % 9)) in
      0|1|2) cmd "scroll 0.$((RANDOM % 100))" 0.3 ;;
      3)     cmd hover 1.6; cmd leave 0.3 ;;
      4)     cmd "action view.zoom-$([ $((RANDOM % 2)) = 0 ] && echo in || echo out)" 1 ;;
      5)     cmd verify 0.3 ;;
      6)     sidebar_round ;;
      7)     cmd "fv-view $((1 + RANDOM % 2))" 1.2; cmd fv-dump 0.3 ;;
      8)     cmd "st-set folder-preview-layout '$([ $((RANDOM % 2)) = 0 ] && echo stacked || echo grid)'" 1
             cmd verify 0.3 ;;
    esac
  done
  cmd "action view.zoom-standard" 1.5; cmd verify 0.5
else
  echo
  echo "Browse in the test window: open folders, scroll fast, hover icons, zoom in/out (Ctrl +/-)."
  read -r -p "Press Enter when done ... " _
  cmd sd-dump 0.3
  cmd verify 0.5
fi

echo "Quitting the test Nautilus (LeakSanitizer runs at exit) ..."
DBUS_SESSION_BUS_ADDRESS="$BUS" nautilus -q >/dev/null 2>&1
for _ in $(seq 1 120); do ps -p "$PID" >/dev/null || break; sleep 0.5; done
[ "$(test_pid)" = "$PID" ] && { echo "test Nautilus did not exit; killing"; kill "$PID"; }

# ---- Analysis ----
fail=0
shopt -s nullglob
reports=("$LOG"/asan.* "$LOG"/ubsan.*)
# ASan prints our frames as "../src/<feature>/<file>.c:<line>" (with debug info) or "(…/libnautilus-utils.so+0x…)".
our_files="$(cd "$ROOT/src" && find . -name '*.c' | sed 's,^[.]/,,; s/[.]c$//' | paste -sd'|')"
ours_pattern="libnautilus-utils[.]so|src/(${our_files})[.]c:"
# A leak whose allocation was made by another extension's Python runtime (pygi/libpython) is that extension's,
# even when our code is further down the stack (e.g. our GSettings write ran its "changed" handler,
# PLAN.md §10.8 Phase 2). Decided by the first frame below the allocator and GLib/GObject/GIO.
# Likewise fontconfig: Pango's font map caches one FcPattern per font style for the whole process, created by
# whichever label first uses that style (before the details pane: one block per run, from Files' own labels;
# with it: one from the pane's title). Never memory we own (PLAN.md §13.8 D7). And Mesa's EGL display: GTK
# initialises GL once per process the first time something needs it (here: the details pane's first video
# sink) and never terminates EGL at exit (D11).
third_party_alloc='_gi[.]cpython|libpython|pygi_|libfontconfig|libEGL_mesa'
for r in "${reports[@]}"; do
  hits="$(awk -v RS= -v pat="$ours_pattern" -v tp="$third_party_alloc" '
    $0 !~ pat { next }
    /leak of/ {
      n = split($0, lines, "\n")
      for (i = 1; i <= n; i++) {
        if (lines[i] !~ /^ *#[0-9]+ /) continue
        # Skip the allocator, GLib, libc (vasprintf…) and libgirepository (PyGObject builds its GI info
        # cache through it, PLAN.md §13.9 Phase 5) to find who asked for the memory.
        if (lines[i] ~ /libasan|libglib-2[.]0|libgobject-2[.]0|libgio-2[.]0|libc[.]so|libgirepository-2[.]0/) continue
        if (lines[i] ~ tp) next
        break
      }
    }
    { print; print "" }' "$r")"
  if [ -n "$hits" ]; then
    echo "FINDING in $r:"
    echo "$hits" | sed 's/^/    | /'
    fail=1
  fi
done

ours_warnings="$(grep -E '(nautilus-utils|folder-preview|sidebar-drives|settings|folder-views|folder-icons|details-pane)-(WARNING|CRITICAL)' "$LOG/nautilus.log" || true)"
if [ -n "$ours_warnings" ]; then
  echo "FINDING: warnings/criticals from our log domain:"
  echo "$ours_warnings" | sed 's/^/    | /'
  fail=1
fi

wrong="$(grep -oE 'debug verify: ok=[0-9]+ wrong=[0-9]+' "$LOG/nautilus.log" | awk -F'wrong=' '$2 > 0' || true)"
n_verify="$(grep -c 'debug verify:' "$LOG/nautilus.log" || true)"
if [ -n "$wrong" ]; then
  echo "FINDING: preview shown on the wrong folder:"
  grep -E 'debug verify: WRONG|wrong=[1-9]' "$LOG/nautilus.log" | sed 's/^/    | /'
  fail=1
fi

# Folder icons: icons were set and restored, without errors, and no folder of the test tree keeps our
# marker or a custom icon afterwards.
fi_set="$(grep -oE 'fi dump: rules=[0-9]+ set=[1-9][0-9]*' "$LOG/nautilus.log" | tail -1)"
fi_cleared="$(grep -oE 'fi dump: .* cleared=[1-9][0-9]*' "$LOG/nautilus.log" | tail -1)"
fi_errors="$(grep -oE 'fi dump: .* errors=[1-9][0-9]*' "$LOG/nautilus.log" | tail -1)"
if [ -n "$AUTO" ]; then
  if [ -z "$fi_set" ] || [ -z "$fi_cleared" ] || [ -n "$fi_errors" ]; then
    echo "FINDING: folder icons: set/restore/error counters wrong:"
    grep 'fi dump:' "$LOG/nautilus.log" | sed 's/^/    | /'
    fail=1
  fi
  # The rules were removed at the end: every folder must be restored. The test session's gvfs metadata is
  # read straight from its own data dir (reads need no daemon).
  fi_left="$(find "$FI_TREE" -type d | while IFS= read -r d; do
    XDG_DATA_HOME="$LOG/xdg-data" gio info -a 'metadata::*' "$d" 2>/dev/null |
      grep -qE 'metadata::(custom-icon|nautilus-utils-icon-rule):' && echo "$d"; done)"
  if [ -n "$fi_left" ]; then
    echo "FINDING: folder icons: folders still carry a custom icon / our marker after the rules were removed:"
    echo "$fi_left" | sed 's/^/    | /'
    fail=1
  fi
fi

# Previews must actually be drawn somewhere (DIR needs subfolders with pictures and the plain folder icon),
# otherwise the wrong-folder checks above checked nothing.
n_shown="$(grep -cE 'debug verify: ok=[1-9]' "$LOG/nautilus.log" || true)"
if [ "$n_shown" -eq 0 ]; then
  echo "FINDING: no self-check ever saw a drawn preview (DIR without picture folders, or the plain folder icon wasn't found)"
  fail=1
fi

left="$(grep -E 'sd dump: OFF .*leftovers=[1-9]' "$LOG/nautilus.log" || true)"
[ -z "$left" ] || { echo "FINDING: sidebar-drives left traces while switched off:"; echo "$left" | sed 's/^/    | /'; fail=1; }
stale="$(grep -E 'fp state: off=1 .*installed=[1-9]' "$LOG/nautilus.log" || true)"
[ -z "$stale" ] || { echo "FINDING: folder previews still shown while switched off:"; echo "$stale" | sed 's/^/    | /'; fail=1; }
n_off="$(grep -c 'sd dump: OFF' "$LOG/nautilus.log" || true)"
# Bars/captions of rows with a known size must be visible exactly when the settings say so.
vis_bad="$(awk '/sd dump: style bars=/ { match($0, /bars=[01]/); bars = substr($0, RSTART + 5, 1); match($0, /labels=[01]/); labels = substr($0, RSTART + 7, 1); next }
  /sd dump: +(OS-ROW|drive) / && /caption=.[0-9]/ { match($0, / bar=[01]/); b = substr($0, RSTART + 5, 1); match($0, /cap-visible=[01]/); c = substr($0, RSTART + 12, 1);
    if (bars != "" && (b != bars || c != labels)) print }' "$LOG/nautilus.log")"
[ -z "$vis_bad" ] || { echo "FINDING: bar/label visibility doesn't follow the settings:"; echo "$vis_bad" | head -5 | sed 's/^/    | /'; fail=1; }
n_style="$(grep -c 'sd dump: style' "$LOG/nautilus.log" || true)"
# While sliding, Nautilus' eject button must sit in our revealer with its clicked handler; when the option is
# off again, no row may still be in slide mode.
ej_bad="$(awk '/st: set eject-slide = / { slide = $NF; next }
  /sd dump: +eject: slide/ { n++; if (slide != "true" || $0 !~ /button-in-revealer=1 clicked-handler=1/) print }
  END { if (n == 0) print "no sliding eject rows were ever dumped" }' "$LOG/nautilus.log")"
[ -z "$ej_bad" ] || { echo "FINDING: sliding eject:"; echo "$ej_bad" | head -5 | sed 's/^/    | /'; fail=1; }
n_eject="$(grep -c 'sd dump: *eject: slide revealed=1' "$LOG/nautilus.log" || true)"
# OS drive menu (PLAN.md §9.1.3): Nautilus' own menu, with Properties and without Unmount/Format.
n_menu="$(grep -c 'sd os-menu: shown=1 .*Properties' "$LOG/nautilus.log" || true)"
bad_menu="$(grep -E 'sd os-menu: shown=0|sd os-menu: .*(Unmount|Format|Eject)|OS menu UNAVAILABLE' "$LOG/nautilus.log" || true)"
[ -z "$bad_menu" ] || { echo "FINDING: OS drive menu missing or wrong:"; echo "$bad_menu" | sed 's/^/    | /'; fail=1; }
[ "$n_menu" -gt 0 ] || { echo "FINDING: the OS drive menu never opened"; fail=1; }
n_fp_on="$(grep -cE 'fp state: off=0 hooks=[1-9]' "$LOG/nautilus.log" || true)"
n_fp_off="$(grep -c 'fp state: off=1' "$LOG/nautilus.log" || true)"

# Folder views: every tab dump shows its folder's view; named checks match; nothing attached while off.
fv_bad="$(grep -E 'fv dump: tab=.* ok=0|fv expect: ok=0|fv: .*FAILED' "$LOG/nautilus.log" || true)"
[ -z "$fv_bad" ] || { echo "FINDING: folder views:"; echo "$fv_bad" | head -8 | sed 's/^/    | /'; fail=1; }
fv_off_bad="$(grep 'fv dump: off' "$LOG/nautilus.log" | grep -v 'leftover=0 keys-at-default=1' || true)"
[ -z "$fv_off_bad" ] || { echo "FINDING: folder views left traces while switched off:"; echo "$fv_off_bad" | head -5 | sed 's/^/    | /'; fail=1; }
n_fv_checked="$(grep -c 'fv dump: tab=.* checked=1 ok=1' "$LOG/nautilus.log" || true)"
n_fv_expect="$(grep -c 'fv expect: ok=1' "$LOG/nautilus.log" || true)"
n_fv_off="$(grep -c 'fv dump: off' "$LOG/nautilus.log" || true)"
n_fv_trans="$(grep -c 'transition: done' "$LOG/nautilus.log" || true)"
if [ -n "$AUTO" ] && [ "$n_fv_checked" -eq 0 ]; then echo "FINDING: folder views never checked a tab"; fail=1; fi

# Details pane: every expectation held; the button was shown; nothing of ours stays while switched off.
gt_fail="$(grep -E 'gt expect FAIL|gt: unknown command' "$LOG/nautilus.log" || true)$(grep -h MISSING "$LOG/gt-checks" 2>/dev/null || true)"
[ -z "$gt_fail" ] || { echo "FINDING: git:"; echo "$gt_fail" | head -10 | sed 's/^/    | /'; fail=1; }
dp_fail="$(grep -E 'dp expect FAIL|dp: (no window|unknown command)' "$LOG/nautilus.log" || true)"
[ -z "$dp_fail" ] || { echo "FINDING: details pane:"; echo "$dp_fail" | head -10 | sed 's/^/    | /'; fail=1; }
dp_look_bad="$(grep -E 'dp png: FAILED' "$LOG/nautilus.log" || true)"
n_dp_bottom="$(grep -c 'dp look: bottom=1 paned-vertical=1' "$LOG/nautilus.log" || true)"
n_dp_line="$(grep -c 'dp look: .* header-line=1' "$LOG/nautilus.log" || true)"
if [ -n "$AUTO" ] && { [ -n "$dp_look_bad" ] || [ "$n_dp_bottom" -eq 0 ] || [ "$n_dp_line" -eq 0 ]; }; then
  echo "FINDING: details pane look: bottom=$n_dp_bottom header-line=$n_dp_line $dp_look_bad"; fail=1
fi
# Nothing of ours on Files' header bar once the pane is back to defaults (line off).
dp_line_left="$(grep 'dp look: bottom=0' "$LOG/nautilus.log" | tail -1 | grep 'header-line=1' || true)"
[ -z "$dp_line_left" ] || { echo "FINDING: header line left after it was switched off"; fail=1; }
dp_off_bad="$(grep 'dp dump: enabled=0' "$LOG/nautilus.log" | grep -v 'windows=0' || true)"
[ -z "$dp_off_bad" ] || { echo "FINDING: details pane left traces while switched off:"; echo "$dp_off_bad" | sed 's/^/    | /'; fail=1; }
n_dp_ok="$(grep -c 'dp expect ok' "$LOG/nautilus.log" || true)"
n_gt_ok="$(grep -c 'gt expect ok' "$LOG/nautilus.log" || true)"
n_dp_button="$(grep -c 'dp dump: enabled=1 windows=[1-9].* button=1' "$LOG/nautilus.log" || true)"
if [ -n "$AUTO" ] && { [ "$n_dp_ok" -eq 0 ] || [ "$n_dp_button" -eq 0 ]; }; then
  echo "FINDING: details pane never checked (expectations ok: $n_dp_ok, button shown: $n_dp_button)"; fail=1
fi

loaded="$(grep -c 'folder-preview loaded' "$LOG/nautilus.log" || true)"
sidebar_ok="$(grep -c 'sd dump: .*OS-ROW' "$LOG/nautilus.log" || true)"
[ "$sidebar_ok" -gt 0 ] || { echo "FINDING: the sidebar-drives hook never showed its OS row"; fail=1; }
echo
echo "Smoke summary:"
echo "  module loaded:          $([ "$loaded" -gt 0 ] && echo yes || echo NO)"
echo "  previews drawn in:      $n_shown self-checks"
echo "  folder icons:           $(grep 'fi dump:' "$LOG/nautilus.log" | tail -1 | sed 's/.*fi dump: //')"
echo "  self-checks (verify):   $n_verify, wrong previews: $([ -z "$wrong" ] && echo 0 || echo SOME)"
echo "  sidebar self-dumps:     $sidebar_ok"
echo "  sliding eject (shown):  $n_eject"
echo "  style dumps:            $n_style"
echo "  OS drive menus:         $n_menu"
echo "  settings toggles:       sidebar off-dumps $n_off, preview off-states $n_fp_off, preview re-hooked $n_fp_on"
echo "  folder views:           tab checks $n_fv_checked, named checks $n_fv_expect, off-dumps $n_fv_off, smooth transitions $n_fv_trans, wrong: $([ -z "$fv_bad" ] && echo 0 || echo SOME)"
echo "  details pane:           expectations ok $n_dp_ok, failed: $([ -z "$dp_fail" ] && echo 0 || echo SOME), button shown in $n_dp_button dumps"
echo "  git:                    expectations ok $n_gt_ok, failed: $([ -z "$gt_fail" ] && echo 0 || echo SOME)"
echo "  sanitizer report files: ${#reports[@]} (only blocks mentioning our code count)"
echo "  result:                 $([ $fail -eq 0 ] && [ "$loaded" -gt 0 ] && echo PASS || echo FAIL)"
[ "$loaded" -gt 0 ] || fail=1
exit $fail
