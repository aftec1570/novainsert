#!/bin/bash
# Configures, builds and runs the unit tests. Prints one "<preset> PASS|FAIL" line each; exit 1 on any FAIL.
# Usage: tests/run_unit.sh [preset ...|all]   (default: release — the quick loop; a freeze gate runs "all" = debug release asan tsan)
# The presets build and test side by side, sharing the machine's cores (NI_JOBS overrides the core count).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PRESETS=("$@"); [ ${#PRESETS[@]} -eq 0 ] && PRESETS=(release)
[ "${PRESETS[0]}" = all ] && PRESETS=(debug release asan tsan)
NCPU="${NI_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)}"
JOBS=$(( NCPU / ${#PRESETS[@]} )); [ "$JOBS" -lt 1 ] && JOBS=1

run_preset() {   # $1 = preset; writes "<preset> PASS|FAIL ..." to build-<preset>.result
  local p=$1 bt flags dir log
  case $p in
    debug)   bt=Debug;   flags="" ;;
    release) bt=Release; flags="" ;;
    asan)    bt=Debug;   flags="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" ;;
    tsan)    bt=Debug;   flags="-fsanitize=thread" ;;
    *) echo "$p UNKNOWN" >"$ROOT/build-$p.result"; return ;;
  esac
  dir="$ROOT/build-$p"
  log="$dir/unit-tests.log"
  if cmake -S "$ROOT" -B "$dir" -DCMAKE_BUILD_TYPE=$bt -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="$flags" \
        -DCMAKE_SHARED_LINKER_FLAGS="$flags" >"$dir.configure.log" 2>&1 \
     && cmake --build "$dir" --target novainsert_unit_tests --parallel "$JOBS" >"$dir.build.log" 2>&1 \
     && (cd "$dir" && ctest -j "$JOBS" --output-on-failure >"$log" 2>&1); then
    echo "$p PASS ($(grep -E 'tests passed' "$log" | head -1))" >"$dir.result"
  else
    echo "$p FAIL (see $dir.configure.log, $dir.build.log, $log)" >"$dir.result"
  fi
}

for p in "${PRESETS[@]}"; do rm -f "$ROOT/build-$p.result"; done
for p in "${PRESETS[@]}"; do
  run_preset "$p" &
done
wait
rc=0
for p in "${PRESETS[@]}"; do
  line=$(cat "$ROOT/build-$p.result" 2>/dev/null || echo "$p FAIL (no result)")
  echo "$line"; case $line in *" PASS "*) ;; *) rc=1 ;; esac
done
exit $rc
