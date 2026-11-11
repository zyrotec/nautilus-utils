#!/usr/bin/env bash
# Runs every automated memory-safety check from PLAN.md §4.3 and exits non-zero on ANY finding.
#
#   1. strict compile   -O2 with -Wall -Wextra -Werror -Wformat=2 -Wnull-dereference -Wshadow (+ prototypes)
#   2. gcc -fanalyzer   GCC's static analyzer, warnings are errors
#   3. scan-build       Clang static analyzer (--status-bugs: any bug report fails)
#   4. asan+ubsan       AddressSanitizer + LeakSanitizer + UBSan over `meson test`
#   5. valgrind         memcheck over `meson test`, definite leaks and memory errors fail
#   6. coverage         INFORMATIONAL (never fails): which lines of src/ the tests never ran. Steps 4–5 can
#                       only find bugs on lines the tests execute, and steps 2–3 are weak on GObject code
#                       (PLAN.md §4.3), so untested lines are effectively unchecked.
#
# Step 5 of §7.3 (ASan build loaded into a real Nautilus) is tools/smoke-nautilus.sh.
#
# Usage: tools/check-safety.sh            run everything
#        tools/check-safety.sh asan       run only the named steps (strict analyzer scan-build asan valgrind)
#
# Full output of each step: safety-logs/<step>.log

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 2
LOGDIR="$ROOT/safety-logs"
mkdir -p "$LOGDIR"

GLIB_SUPP=/usr/share/glib-2.0/valgrind/glib.supp
VG_SUPP="$ROOT/tools/valgrind.supp"
LSAN_SUPP="$ROOT/tools/lsan.supp"

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing tool: $1 ($2)"; exit 2; }; }
need meson meson
need ninja ninja-build
need gcc gcc
need scan-build clang-analyzer
need valgrind valgrind
[ -f "$GLIB_SUPP" ] || { echo "missing $GLIB_SUPP (glib2-devel)"; exit 2; }

# meson setup that also works on an existing build dir (options are re-applied).
setup() {
  local dir=$1; shift
  if [ -f "$dir/build.ninja" ]; then
    meson setup --reconfigure "$dir" "$@"
  else
    meson setup "$dir" "$@"
  fi
}

step_strict() {
  setup build-strict --buildtype=debugoptimized -Dwerror=true &&
  ninja -C build-strict
}

step_analyzer() {
  setup build-analyzer --buildtype=debug -Dwerror=true -Danalyzer=true &&
  ninja -C build-analyzer
}

step_scan_build() {
  setup build-scan --buildtype=debug -Dwerror=true || return 1
  # Meson's scan-build target does not pass --status-bugs by itself; it honours $SCANBUILD.
  SCANBUILD="scan-build --status-bugs" ninja -C build-scan scan-build || return 1
  # Belt and braces: any HTML report means a finding.
  if find build-scan/meson-logs/scanbuild -name 'report-*.html' 2>/dev/null | grep -q .; then
    echo "scan-build produced reports in build-scan/meson-logs/scanbuild"
    return 1
  fi
}

step_asan() {
  setup build-asan --buildtype=debug -Db_sanitize=address,undefined -Db_lundef=false || return 1
  ninja -C build-asan || return 1
  ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:abort_on_error=1:detect_stack_use_after_return=1:strict_string_checks=1" \
  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
  LSAN_OPTIONS="suppressions='$LSAN_SUPP':print_suppressions=0" \
    meson test -C build-asan --print-errorlogs
}

step_valgrind() {
  setup build-valgrind --buildtype=debug || return 1
  ninja -C build-valgrind || return 1
  local supp="--suppressions='$GLIB_SUPP'"
  [ -f "$VG_SUPP" ] && supp="$supp --suppressions='$VG_SUPP'"
  G_DEBUG=gc-friendly G_SLICE=always-malloc \
    # --no-suite latency: valgrind serialises threads, so main-loop timing there is meaningless.
    meson test -C build-valgrind --print-errorlogs --timeout-multiplier=20 --no-suite latency \
      --wrap="valgrind --leak-check=full --errors-for-leak-kinds=definite --error-exitcode=1 --num-callers=40 $supp"
}

step_coverage() {
  setup build-coverage --buildtype=debug -Db_coverage=true || return 1
  ninja -C build-coverage || return 1
  find build-coverage -name '*.gcda' -delete
  meson test -C build-coverage || return 1
  (
    cd build-coverage || exit 1
    find . -name '*.gcda' -not -path './tests/*' | sort | while read -r gcda; do
      gcov -n "$gcda" 2>/dev/null | grep -A1 "^File '../src/" | grep -v '^--$' | paste - - |
        sed -E "s/File '..\/(.*)'\s*Lines executed:/  \1: /"
      gcov -t "$gcda" 2>/dev/null | grep -E '^ *#####' | sed 's/^ *#####: */      never run: line /' || true  # no uncovered lines is fine
    done
  )
}

ALL_STEPS=(strict analyzer scan-build asan valgrind coverage)
STEPS=("$@")
[ ${#STEPS[@]} -eq 0 ] && STEPS=("${ALL_STEPS[@]}")

declare -A RESULT
failed=0
for s in "${STEPS[@]}"; do
  fn="step_${s//-/_}"
  if ! declare -F "$fn" >/dev/null; then echo "unknown step: $s"; exit 2; fi
  log="$LOGDIR/$s.log"
  printf '%-12s ... ' "$s"
  if "$fn" >"$log" 2>&1; then
    RESULT[$s]=PASS
    echo PASS
    if [ "$s" = coverage ]; then
      RESULT[$s]="report (informational)"
      sed -n '/^  src\//,$p' "$log" | grep -E '^  src/|never run' || true
    fi
  else
    RESULT[$s]=FAIL
    failed=1
    echo "FAIL  (last lines of $log:)"
    tail -n 40 "$log" | sed 's/^/    | /'
  fi
done

echo
echo "Summary:"
for s in "${STEPS[@]}"; do printf '  %-12s %s\n' "$s" "${RESULT[$s]}"; done
echo "  smoke        manual: tools/smoke-nautilus.sh (PLAN.md §4.3, smoke test)"
exit $failed
