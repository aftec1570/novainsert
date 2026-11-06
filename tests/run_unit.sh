#!/bin/bash
# Configures, builds and runs the unit tests. Prints one "<preset> PASS|FAIL" line each; exit 1 on any FAIL.
# Usage: tests/run_unit.sh [preset ...|all]   (default: release — the quick loop; a freeze gate runs "all" = debug release asan tsan)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PRESETS=("$@"); [ ${#PRESETS[@]} -eq 0 ] && PRESETS=(release)
[ "${PRESETS[0]}" = all ] && PRESETS=(debug release asan tsan)
rc=0
for p in "${PRESETS[@]}"; do
  case $p in
    debug)   bt=Debug;   flags="" ;;
    release) bt=Release; flags="" ;;
    asan)    bt=Debug;   flags="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" ;;
    tsan)    bt=Debug;   flags="-fsanitize=thread" ;;
    *) echo "$p UNKNOWN"; rc=1; continue ;;
  esac
  dir="$ROOT/build-$p"
  log="$dir/unit-tests.log"
  if cmake -S "$ROOT" -B "$dir" -DCMAKE_BUILD_TYPE=$bt -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="$flags" \
        -DCMAKE_SHARED_LINKER_FLAGS="$flags" >"$dir.configure.log" 2>&1 \
     && cmake --build "$dir" --target novainsert_unit_tests --parallel >"$dir.build.log" 2>&1 \
     && (cd "$dir" && ctest --output-on-failure >"$log" 2>&1); then
    echo "$p PASS ($(grep -E 'tests passed' "$log" | head -1))"
  else
    echo "$p FAIL (see $dir.configure.log, $dir.build.log, $log)"; rc=1
  fi
done
exit $rc
