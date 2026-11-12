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
# SMOKE_TOGGLES=N (--auto only, default 25): switch each feature off and on N times through the settings
#   (PLAN.md §9) and check that nothing of ours is left while off and everything comes back when on.
#
# Settings: the test Nautilus uses an in-memory GSettings backend and the build's schema, so the smoke test
# never changes your real settings.
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

restore_build() {
  echo "Restoring normal build in build/ ..."
  meson configure build -Db_sanitize=none -Ddebug_driver=false >/dev/null && ninja -C build >/dev/null ||
    echo "WARNING: could not restore build/; run: meson configure build -Db_sanitize=none -Ddebug_driver=false && ninja -C build" >&2
}
trap restore_build EXIT

echo "Building build/ with address,undefined sanitizers + debug driver ..."
[ -f build/build.ninja ] || meson setup build >/dev/null || exit 2
meson configure build -Db_sanitize=address,undefined -Db_lundef=false -Ddebug_driver=true >/dev/null || exit 2
ninja -C build >"$LOG/build.log" 2>&1 || { tail -30 "$LOG/build.log"; exit 2; }

ASAN_RT="$(ldd build/libnautilus-utils.so | awk '/libasan\.so/ {print $3}')"
[ -f "$ASAN_RT" ] || { echo "cannot find ASan runtime (dnf install libasan)"; exit 2; }

before_pids="$(pgrep -x nautilus | sort)"
echo "Starting an isolated Nautilus on $DIR (logs: $LOG)"
(
  cd "$LOG" || exit 1
  # LD_PRELOAD only for nautilus itself, not for the private dbus-daemon / portals.
  setsid dbus-run-session -- env \
    LD_PRELOAD="$ASAN_RT" \
    ASAN_OPTIONS="log_path='$LOG/asan':detect_leaks=1:verify_asan_link_order=0" \
    UBSAN_OPTIONS="log_path='$LOG/ubsan':print_stacktrace=1" \
    LSAN_OPTIONS="suppressions='$ROOT/tools/lsan.supp':print_suppressions=0" \
    G_MESSAGES_DEBUG="nautilus-utils folder-preview sidebar-drives settings" \
    GSETTINGS_BACKEND=memory GSETTINGS_SCHEMA_DIR="$ROOT/build/data" \
    SD_DEBUG_FAKE_NETWORK="${SMOKE_FAKE_NETWORK:-}" \
    FP_DEBUG_DIR="$CMD_DIR" FP_DEBUG_ROOT="$DIR" ${SMOKE_XDG_CACHE:+XDG_CACHE_HOME=$SMOKE_XDG_CACHE} \
    nautilus --new-window "$DIR" >"$LOG/nautilus.log" 2>&1 &
)

PID=""
for _ in $(seq 1 40); do
  sleep 0.5
  PID="$(comm -13 <(echo "$before_pids") <(pgrep -x nautilus | sort) | head -1)"
  [ -n "$PID" ] && break
done
[ -n "$PID" ] || { echo "Nautilus failed to start:"; tail -30 "$LOG/nautilus.log"; exit 1; }
BUS="$(tr '\0' '\n' < /proc/"$PID"/environ | sed -n 's/^DBUS_SESSION_BUS_ADDRESS=//p')"
echo "test Nautilus pid $PID"
sleep 5

cmd() { echo "$1" > "$CMD_DIR/cmd"; kill -USR2 "$PID" 2>/dev/null; sleep "${2:-1}"; }
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

if [ -n "$AUTO" ]; then
  end=$(( $(date +%s) + AUTO ))
  style_round
  eject_round
  cmd st-dump 0.3; cmd st-open 1; cmd st-close 0.5
  for _ in $(seq 1 "${SMOKE_TOGGLES:-25}"); do toggle_round; done
  sidebar_round
  cmd verify 1
  for f in 0.5 1.0 0.25 0.75 0.0; do cmd "scroll $f" 2; cmd verify 0.3; done
  cmd hover 2.5; cmd leave 0.5
  for a in view.zoom-in view.zoom-in view.zoom-out view.zoom-out view.zoom-out view.zoom-standard; do
    cmd "action $a" 1.5; cmd "scroll 0.$((RANDOM % 10))" 1.5; cmd verify 0.3
  done
  while [ "$(date +%s)" -lt "$end" ]; do
    case $((RANDOM % 7)) in
      0|1|2) cmd "scroll 0.$((RANDOM % 100))" 0.3 ;;
      3)     cmd hover 1.6; cmd leave 0.3 ;;
      4)     cmd "action view.zoom-$([ $((RANDOM % 2)) = 0 ] && echo in || echo out)" 1 ;;
      5)     cmd verify 0.3 ;;
      6)     sidebar_round ;;
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
ps -p "$PID" >/dev/null && { echo "test Nautilus did not exit; killing"; kill "$PID"; }

# ---- Analysis ----
fail=0
shopt -s nullglob
reports=("$LOG"/asan.* "$LOG"/ubsan.*)
# ASan prints our frames as "../src/<feature>/<file>.c:<line>" (with debug info) or "(…/libnautilus-utils.so+0x…)".
our_files="$(cd "$ROOT/src" && find . -name '*.c' | sed 's,^[.]/,,; s/[.]c$//' | paste -sd'|')"
ours_pattern="libnautilus-utils[.]so|src/(${our_files})[.]c:"
for r in "${reports[@]}"; do
  hits="$(awk -v RS= -v pat="$ours_pattern" '$0 ~ pat {print; print ""}' "$r")"
  if [ -n "$hits" ]; then
    echo "FINDING in $r:"
    echo "$hits" | sed 's/^/    | /'
    fail=1
  fi
done

ours_warnings="$(grep -E '(nautilus-utils|folder-preview|sidebar-drives|settings)-(WARNING|CRITICAL)' "$LOG/nautilus.log" || true)"
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

loaded="$(grep -c 'folder-preview loaded' "$LOG/nautilus.log" || true)"
sidebar_ok="$(grep -c 'sd dump: .*OS-ROW' "$LOG/nautilus.log" || true)"
[ "$sidebar_ok" -gt 0 ] || { echo "FINDING: the sidebar-drives hook never showed its OS row"; fail=1; }
echo
echo "Smoke summary:"
echo "  module loaded:          $([ "$loaded" -gt 0 ] && echo yes || echo NO)"
echo "  self-checks (verify):   $n_verify, wrong previews: $([ -z "$wrong" ] && echo 0 || echo SOME)"
echo "  sidebar self-dumps:     $sidebar_ok"
echo "  sliding eject (shown):  $n_eject"
echo "  style dumps:            $n_style"
echo "  OS drive menus:         $n_menu"
echo "  settings toggles:       sidebar off-dumps $n_off, preview off-states $n_fp_off, preview re-hooked $n_fp_on"
echo "  sanitizer report files: ${#reports[@]} (only blocks mentioning our code count)"
echo "  result:                 $([ $fail -eq 0 ] && [ "$loaded" -gt 0 ] && echo PASS || echo FAIL)"
[ "$loaded" -gt 0 ] || fail=1
exit $fail
