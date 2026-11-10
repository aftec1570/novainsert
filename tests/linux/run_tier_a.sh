#!/usr/bin/env bash
# Linux tier A in the novainsert-linux container: Release build, unit tests (ctest), pluginval strictness 10.
# Host: docker run --rm --platform linux/amd64 -v "$PWD":/work -w /work novainsert-linux tests/linux/run_tier_a.sh
set -uo pipefail
cd /work
B=build-linux
VST3="$B/src/NovaInsert_artefacts/Release/VST3/NovaInsert.vst3"
ok=1
cmake -S . -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build "$B" --target novainsert_unit_tests NovaInsert_VST3 || ok=0
if [ $ok = 1 ]; then (cd "$B" && ctest -j "$(nproc)" --output-on-failure) || ok=0; fi
if [ $ok = 1 ]; then
  # pluginval loads the plugin outside REAPER (state UnsupportedHost by design) and opens its editor: Xvfb for the GUI tests
  xvfb-run -a pluginval --strictness-level 10 --validate-in-process --timeout-ms 600000 "$VST3" || ok=0
fi
[ $ok = 1 ] && echo "tier-a PASS" || echo "tier-a FAIL"
[ $ok = 1 ]
