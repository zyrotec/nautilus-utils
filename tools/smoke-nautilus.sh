#!/usr/bin/env bash
# In-Nautilus smoke test (PLAN.md §4.3, smoke test): run a real Nautilus with an AddressSanitizer + UBSan build of
# the extension (plus the test-only debug driver), browse, quit, and fail if
#   - any sanitizer report points into our code, or
#   - the view-hook self-check ever finds a preview on the wrong folder ("debug verify: … wrong=N>0"), or
#   - our log domain printed a WARNING/CRITICAL.
#
# Usage:
#   tools/smoke-nautilus.sh [DIR]                     interactive: browse/scroll/hover/zoom, then press Enter
#   tools/smoke-nautilus.sh --auto SECS [DIR]         scripted: scroll jumps, zoom levels, hover/leave, then
#                                                     random browsing until SECS have passed
#
# SMOKE_XDG_CACHE=DIR: give the test Nautilus its own cache dir (thumbnails + frames), e.g. the benchmark's.
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
    G_MESSAGES_DEBUG="nautilus-utils folder-preview" \
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

if [ -n "$AUTO" ]; then
  end=$(( $(date +%s) + AUTO ))
  cmd verify 1
  for f in 0.5 1.0 0.25 0.75 0.0; do cmd "scroll $f" 2; cmd verify 0.3; done
  cmd hover 2.5; cmd leave 0.5
  for a in view.zoom-in view.zoom-in view.zoom-out view.zoom-out view.zoom-out view.zoom-standard; do
    cmd "action $a" 1.5; cmd "scroll 0.$((RANDOM % 10))" 1.5; cmd verify 0.3
  done
  while [ "$(date +%s)" -lt "$end" ]; do
    case $((RANDOM % 6)) in
      0|1|2) cmd "scroll 0.$((RANDOM % 100))" 0.3 ;;
      3)     cmd hover 1.6; cmd leave 0.3 ;;
      4)     cmd "action view.zoom-$([ $((RANDOM % 2)) = 0 ] && echo in || echo out)" 1 ;;
      5)     cmd verify 0.3 ;;
    esac
  done
  cmd "action view.zoom-standard" 1.5; cmd verify 0.5
else
  echo
  echo "Browse in the test window: open folders, scroll fast, hover icons, zoom in/out (Ctrl +/-)."
  read -r -p "Press Enter when done ... " _
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

ours_warnings="$(grep -E '(nautilus-utils|folder-preview)-(WARNING|CRITICAL)' "$LOG/nautilus.log" || true)"
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

loaded="$(grep -c 'folder-preview loaded' "$LOG/nautilus.log" || true)"
echo
echo "Smoke summary:"
echo "  module loaded:          $([ "$loaded" -gt 0 ] && echo yes || echo NO)"
echo "  self-checks (verify):   $n_verify, wrong previews: $([ -z "$wrong" ] && echo 0 || echo SOME)"
echo "  sanitizer report files: ${#reports[@]} (only blocks mentioning our code count)"
echo "  result:                 $([ $fail -eq 0 ] && [ "$loaded" -gt 0 ] && echo PASS || echo FAIL)"
[ "$loaded" -gt 0 ] || fail=1
exit $fail
