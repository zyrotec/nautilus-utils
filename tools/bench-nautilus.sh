#!/usr/bin/env bash
# PLAN.md §6.5 Phase 6 benchmarks, in an isolated Nautilus (own D-Bus session AND own XDG cache, so neither your
# Files windows nor your ~/.cache are touched). Uses an optimised build of the extension with the test-only
# debug driver (for measurements); the normal build is always restored on exit.
#
# Usage: tools/bench-nautilus.sh [BENCH_DIR]      (default ~/.cache/folder-preview-bench, from make-bench.sh)
#
# Measures (PLAN.md §6.5 Phase 6 targets):
#   prime  — generate thumbnails once (the cold-open target assumes "thumbnails already cached")
#   cold   — our frame cache empty: time from an icon appearing to its preview being drawn (target ≤ 1 s)
#   warm   — relaunch, frames come from the disk cache (target: instant)
#   browse — scroll all folders at two zoom levels (= 1000 folder previews) + hovers: RSS growth
#            (target < 100 MB), main-loop stalls, and the longest time our own main-thread code ran
#            (target: no stall > 16 ms attributable to the extension)
# Results: safety-logs/bench-<time>/summary.txt (written under BENCH_DIR/logs during the run)
#
# BENCH_BASELINE=1: only the browse phase, with the view hook disabled (simulated "Nautilus internals
# changed"), to measure Nautilus' own memory growth and main-loop stalls for comparison.

set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 2

BENCH="$(realpath "${1:-$HOME/.cache/folder-preview-bench}")"
PARENT="${BENCH_PARENT:-$BENCH/parent}"   # BENCH_PARENT: browse another folder (e.g. images, for comparison)
XDG_CACHE="$BENCH/xdg-cache"
[ -d "$PARENT" ] || { echo "no benchmark tree at $PARENT — run tests/folder-preview/make-bench.sh first"; exit 2; }
N_FOLDERS="$(ls "$PARENT" | wc -l)"

STAMP="$(date +%Y%m%d-%H%M%S)"
# Logs are written on the benchmark's own (fast) filesystem during the run: Nautilus writes its log lines
# from the main thread, and on the NTFS project drive those writes blocked long enough to show up as
# main-loop stalls (PLAN.md §6.5). They're copied to safety-logs/ at the end.
LOG="$BENCH/logs/bench-$STAMP"
FINAL_LOG="$ROOT/safety-logs/bench-$STAMP"
CMD_DIR="$LOG/driver"
mkdir -p "$CMD_DIR"
SUMMARY="$LOG/summary.txt"

restore_build() {
  mkdir -p "$FINAL_LOG" && cp -r "$LOG"/. "$FINAL_LOG"/ 2>/dev/null
  echo "Restoring normal build in build/ ..."
  meson configure build --buildtype=debugoptimized -Db_sanitize=none -Ddebug_driver=false >/dev/null && ninja -C build >/dev/null ||
    echo "WARNING: could not restore build/" >&2
}
trap restore_build EXIT

echo "Building an optimised build with the debug driver ..."
meson configure build --buildtype=debugoptimized -Db_sanitize=none -Ddebug_driver=true >/dev/null || exit 2
ninja -C build >"$LOG/build.log" 2>&1 || { tail -20 "$LOG/build.log"; exit 2; }

PID="" BUS=""
start() { # $1 = label
  local before; before="$(pgrep -x nautilus | sort)"
  (
    cd "$LOG" || exit 1
    setsid dbus-run-session -- env XDG_CACHE_HOME="$XDG_CACHE" FP_DEBUG_DIR="$CMD_DIR" FP_DEBUG_ROOT="$PARENT" \
      ${BENCH_BASELINE:+FP_DEBUG_BREAK=types} ${BENCH_NODRAW:+FP_DEBUG_NODRAW=1} ${BENCH_NO_NICE:+FP_WORKERS_NO_NICE=1} FP_DEBUG_STALLS="$LOG/$1.stalls" \
      ${BENCH_NO_PYTHON:+LD_PRELOAD=$BENCH/no-python-shim.so} \
      ${BENCH_STALL_MS:+FP_DEBUG_STALL_MS=$BENCH_STALL_MS} \
      FP_DEBUG_PROBE=1 nautilus --new-window "$PARENT" >"$LOG/$1.log" 2>&1 &
  )
  PID=""
  for _ in $(seq 1 60); do
    sleep 0.25
    PID="$(comm -13 <(echo "$before") <(pgrep -x nautilus | sort) | head -1)"
    [ -n "$PID" ] && break
  done
  [ -n "$PID" ] || { echo "Nautilus failed to start"; exit 1; }
  BUS="$(tr '\0' '\n' < /proc/"$PID"/environ | sed -n 's/^DBUS_SESSION_BUS_ADDRESS=//p')"
}
stop() {
  DBUS_SESSION_BUS_ADDRESS="$BUS" nautilus -q >/dev/null 2>&1
  for _ in $(seq 1 60); do ps -p "$PID" >/dev/null || return 0; sleep 0.5; done
  kill "$PID" 2>/dev/null
}
cmd() { echo "$1" > "$CMD_DIR/cmd"; kill -USR2 "$PID" 2>/dev/null; sleep "${2:-1}"; }
last() { grep -E "$2" "$LOG/$1.log" | tail -1 | sed 's/.*Message: [0-9:.]* //'; }

# Waits until every visible folder icon has its preview (or $2 seconds), then logs the latency stats.
settle() { # $1 = label, $2 = timeout s
  local end=$(( $(date +%s) + $2 ))
  while [ "$(date +%s)" -lt "$end" ]; do
    cmd latency 0.4
    # Ready = at least one icon visible and none still waiting (baseline: nothing to wait for).
    if [ -n "${BENCH_BASELINE:-}" ]; then return 0; fi
    if [ -n "${BENCH_NODRAW:-}" ]; then sleep 0.6; return 0; fi  # frames never count as ready
    last "$1" 'debug latency' | grep -qE 'mapped=[1-9][0-9]* .*pending=0 ' && return 0
  done
}

{
  echo "Benchmark $(date -Iseconds) — $N_FOLDERS folders, $(find "$PARENT" -mindepth 2 | wc -l) files"
  echo "Machine: $(nproc) CPUs, $(free -g | awk '/Mem:/ {print $2}') GB RAM; build: debugoptimized"
} > "$SUMMARY"

[ -n "${BENCH_BASELINE:-}" ] && echo "BASELINE: extension's view hook disabled (Nautilus alone)" >> "$SUMMARY"
[ -n "${BENCH_NODRAW:-}" ] && echo "NODRAW: full pipeline, frames never drawn" >> "$SUMMARY"
[ -n "${BENCH_NO_NICE:-}" ] && echo "NO_NICE: worker threads at normal priority" >> "$SUMMARY"
[ -n "${BENCH_NO_PYTHON:-}" ] && echo "NO_PYTHON: nautilus-python not loaded (LD_PRELOAD shim, tools/no-python-shim.c)" >> "$SUMMARY"

# ---- prime: make sure the first thumbnails of every folder exist ------------------------------------
if [ -z "${BENCH_BASELINE:-}" ] && [ ! -d "$XDG_CACHE/thumbnails/large" ]; then
  echo "Priming thumbnails (first run only) ..."
  start prime
  sleep 4
  for f in $(seq 0 0.04 1.0) 1.0; do cmd "scroll $f" 0.2; settle prime 60; done
  stop
  echo "prime: thumbnails generated: $(ls "$XDG_CACHE/thumbnails/large" | wc -l)" >> "$SUMMARY"
fi

if [ -z "${BENCH_BASELINE:-}${BENCH_NODRAW:-}${BENCH_BROWSE_ONLY:-}" ]; then
# ---- cold: thumbnails cached, our frame cache empty ------------------------------------------------
echo "Cold open ..."
rm -rf "$XDG_CACHE/nautilus-utils/folder-preview"
start cold
sleep 2
settle cold 30
echo "cold:  $(last cold 'debug latency')" >> "$SUMMARY"
stop

# ---- warm: relaunch, frames from disk --------------------------------------------------------------
echo "Warm reopen ..."
start warm
sleep 2
settle warm 30
echo "warm:  $(last warm 'debug latency')" >> "$SUMMARY"
stop
fi

# ---- browse: 2 zoom levels × all folders, hovers; RSS, stalls, our callback times -------------------
echo "Browsing ..."
start browse
sleep 3
settle browse 30
cmd stats-reset 0.5
cmd stats 0.5
rss0="$(last browse 'debug stats' | grep -oE 'rss=[0-9]+' | cut -d= -f2)"
for zoom in standard in; do
  [ "$zoom" = in ] && { cmd "action view.zoom-in" 2; settle browse 20; }
  for f in $(seq 0 0.02 1.0) 1.0; do
    cmd "scroll $f" 0.15
    settle browse 15
    case "$f" in *0|*5) cmd hover 1.6; cmd leave 0.3 ;; esac
  done
done
cmd "action view.zoom-standard" 2
cmd verify 0.5
cmd stats 0.5
rss1="$(last browse 'debug stats' | grep -oE 'rss=[0-9]+' | cut -d= -f2)"
{
  echo "browse: scroll commands without a grid: $(grep -c 'debug cmd: no grid' "$LOG/browse.log")"
  echo "browse: $(last browse 'debug stats')"
  echo "browse: RSS growth $(( (rss1 - rss0) / 1024 )) MB (from $(( rss0 / 1024 )) MB to $(( rss1 / 1024 )) MB)"
  echo "browse: $(last browse 'debug verify')"
  echo "browse: start $(grep 'debug malloc' "$LOG/browse.log" | head -1 | sed 's/.*malloc: //')"
  echo "browse: end   $(last browse 'debug malloc' | sed 's/.*malloc: //')"
  echo "browse: slowest callbacks: $(grep -E 'debug timing' "$LOG/browse.log" | tail -12 | sed 's/.*timing: //' | paste -sd ';')"
} >> "$SUMMARY"
stop

echo
cat "$SUMMARY"
