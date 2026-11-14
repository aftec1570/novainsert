#!/usr/bin/env bash
# Linux tier B in the novainsert-linux container: REAPER x86_64 headless under Xvfb, audio on a JACK dummy server whose
# loopback driver (jackd -L 63) is the software loop (no snd-aloop in OrbStack, tests/linux/README.md).
# NovaInsert's driver gate rejects JACK, so the harness sets allowAnyDriver (S1 `driver_gate` checks the rejection itself).
#
# Host: docker run --rm --platform linux/amd64 --cap-add SYS_NICE --ulimit rtprio=95 --ulimit memlock=-1 \
#         -v "$PWD":/work -w /work novainsert-linux tests/linux/run_tier_b.sh [options]
#       (the RT flags let jackd run its graph realtime; without them the dummy graph xruns under Rosetta)
#   --only a,b       run only these runners: f0_1 f0_2 f0_4 f0_3 f0_6 f3_1 gui_cal gui_levels measure pdc faults
#   --device-snd     add S1 default_device (ALSA "default"; needs docker --device /dev/snd --group-add 18)
#   env NI_BS (256), NI_JACKD_OPTS (-S = synchronous; set empty for none), NI_SCENARIOS (all = each runner's full list), NI_LONG_RUN (fl_long_run seconds, 120)
# Evidence: tests/results/linux/<run-id>/ (each runner's matrix [+ summary] and console log); exit 0 = every runner PASS.
#
# What runs: the F0 spike runners (tests/f0/run_f0{1,2,4,3,6}.py; spike bundles from NI_SPIKE_VST3_DIR or a -DNI_BUILD_SPIKES=ON
# build), S1 bridge (tests/f3/run_f3_1.py), the F4 GUI subset (gui_cal, gui_levels) and the current product gates, the loop-bench
# runner tests/f3/apollo/run_apollo.py (S4 --measure, S5 --pdc, S11 --faults) on the JACK device profile (bench_profile.py).
# Not here: F0.5 (a CoreAudio/BlackHole loop fixture; the JACK loop is measured by F0.2 hw_loop_lag and S4), F3 S2/S3
# (run_f3_2.py / run_f3_3.py predate measurement: their gates expect Queued and a configured 2B loop), S6 --render beyond the render-dialog subset (its
# other desktop actions are AppleScript), the HID/screenshot F4 GUI checks (cg_input.swift, osascript, screencapture: macOS-only).
set -uo pipefail
cd /work
ONLY=""; SND=0
while [ $# -gt 0 ]; do case "$1" in --only) ONLY="$2"; shift 2;; --device-snd) SND=1; shift;; *) echo "unknown option $1"; exit 2;; esac; done
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)"
OUT="/work/tests/results/linux/$RUN_ID"; mkdir -p "$OUT"
BS="${NI_BS:-256}"   # Rosetta-emulated x86_64: 64-sample periods underrun constantly; 256 keeps the JACK graph stable
SCEN="${NI_SCENARIOS:-all}"; LONG="${NI_LONG_RUN:-120}"

export NI_VST3="${NI_VST3:-/work/build-linux/src/NovaInsert_artefacts/Release/VST3/NovaInsert.vst3}"; VST3="$NI_VST3"
[ -d "$VST3" ] || { echo "tier-b FAIL (no $VST3: run tests/linux/run_tier_a.sh first)"; exit 1; }

export DISPLAY=:99 NI_REAPER=/opt/reaper/REAPER/reaper NI_ROOT=/work NI_DEVICE=JACK NI_ALLOW_ANY_DRIVER=1
export NI_CFG="$HOME/ni-cfg" NI_BENCH=jack NI_BS="$BS"
Xvfb :99 -screen 0 1600x1000x24 >/dev/null 2>&1 &
# Loop: jackd's loopback driver. The dummy backend has no system ports (-C 0 -P 0), so loopback:playback_<n>/capture_<n> are the
# only physical ports and REAPER's own JACK auto-connect (jack_options=0, its default) wires REAPER:out<n> -> loopback:playback_<n>
# and loopback:capture_<n> -> REAPER:in<n> (1 block) inside its device open, before audio runs. That matters: REAPER re-registers
# its client around every online render, and an external keeper (port-registration callback) needed ~10 ms per jack_connect,
# 0.5-0.7 s for the set, so renders started with late outboard channels. 63 ports: REAPER:in64 has no partner and stays unfed
# (the no-return fixture, bench_profile.JACK_NO_RETURN).
jackd ${NI_JACKD_OPTS--S} -L 63 -d dummy -r 48000 -p "$BS" -C 0 -P 0 >"$OUT/jackd.log" 2>&1 &
for _ in $(seq 20); do jack_lsp >/dev/null 2>&1 && break; sleep 0.5; done

# isolated cfg: only the startup hook; REAPER fills the rest on first start, the harness sets the audio keys per launch
rm -rf "$NI_CFG"; mkdir -p "$NI_CFG/Scripts"
cp tests/reaper-test-config/Scripts/__startup.lua "$NI_CFG/Scripts/"
printf '[REAPER]\nverchk=0\nsplashfast=1\nautosaveint=0\naudiocloseinactive=0\nrenderaheadlen=200\njack_options=0\n' >"$NI_CFG/reaper.ini"


rc=0
want() { [ -z "$ONLY" ] || [[ ",$ONLY," == *",$1,"* ]]; }
# F0 spike bundles (tests/f0/f0_platform.spike_vst3): build them into build-linux unless NI_SPIKE_VST3_DIR supplies them
if [ -z "${NI_SPIKE_VST3_DIR:-}" ] && { want f0_1 || want f0_2 || want f0_4 || want f0_3 || want f0_6; }; then
  echo "== spikes (build-linux, -DNI_BUILD_SPIKES=ON)"
  { cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DNI_BUILD_SPIKES=ON >/dev/null &&
    cmake --build build-linux --target F0Bridge_VST3 F02Hook_VST3 F03Pdc_VST3 F04Render_VST3 F06Coord_VST3; } >"$OUT/spikes-build.log" 2>&1 ||
    { echo "tier-b FAIL (spike build, $OUT/spikes-build.log)"; exit 1; }
fi
run() {   # run <name> <results dir> <command...>: the command gets --tag _<name><RUN_ID>; its run dir is collected into OUT
  local name="$1" dir="$2"; shift 2
  local tag="_${name}${RUN_ID}"
  echo "== $name"
  "$@" --tag "$tag" >"$OUT/$name.log" 2>&1; local r=$?
  grep -E '^(OVERALL|   )' "$OUT/$name.log" | cut -c1-300
  local d; d="$(ls -d "$dir"/*"$tag"/ 2>/dev/null | head -1)"
  if [ -n "$d" ] && ls "$d"/*-matrix.json >/dev/null 2>&1; then
    mkdir -p "$OUT/$name"; cp "$d"/*-matrix.json "$OUT/$name/"; cp "$d"/*-summary.md "$OUT/$name/" 2>/dev/null
  else echo "   no matrix for $name"; r=1; fi
  [ "$r" = 0 ] || rc=1
}

S1="load1,load20,unload,sr44,sr176,driver_gate"; [ "$SND" = 1 ] && S1="$S1,default_device"
BENCH=(python3 tests/f3/apollo/run_apollo.py --buffers "$BS" --scenarios "$SCEN")
# F0.2 (hook <-> processBlock relation, the spike plugin; scenario A needs anticipative FX, the gate runs the _noant set)
# F0.1 (REAPER API from a VST3, lifecycle; the bridge spike)
want f0_1    && run f0_1    tests/results/f0/f01    python3 tests/f0/run_f01.py --buffers "$BS"
F02="${NI_F02_SCEN:-A_noant,B_noant,C_noant,D_noant,E_noant,F_noant,G_noant,M2_noant,M3_noant,S20_noant}"
want f0_2    && run f0_2    tests/results/f0/f02    python3 tests/f0/run_f02.py --buffers "$BS" --repeats 1 --scenario "$F02"
# F0.4 (online render: hook detection, render-start alignment; the spike plugin)
F04="${NI_F04_SCEN:-first,rpt,playlock,off_ctrl,rt_ctrl,keep,nogui,two,chain3,s20,bypass_all,disabled_all,mixed,stems,regions,dryrun,abort}"
want f0_4    && run f0_4    tests/results/f0/f04    python3 tests/f0/run_f04.py --buffers "$BS" --repeats 1 --scenario "$F04"
# F0.3 (PDC: reported latency changes, chains, routing; the spike plugin) and F0.6 (multi-instance coordination)
want f0_3    && run f0_3    tests/results/f0/f03    python3 tests/f0/run_f03.py --buffers "$BS" --repeats 1 ${NI_F03_SCEN:+--scenario "$NI_F03_SCEN"}
want f0_6    && run f0_6    tests/results/f0/f06    python3 tests/f0/run_f06.py --buffers "$BS" --repeats 1 --longsecs 30 ${NI_F06_SCEN:+--scenario "$NI_F06_SCEN"}
want f3_1    && run f3_1    tests/results/f3/s1     python3 tests/f3/run_f3_1.py --buffers "$BS" --scenario "$S1"
# F4 GUI subset (Lua-driven, logged view-model state; the HID/screenshot GUI checks are macOS-only): level CAL / LINK / conflicts
# and trims / CAL tone / meters, on JACK channels 9/10 (0-based 8/9, looped by jackd's loopback driver)
want gui_cal    && run gui_cal    tests/results/f4/gui_cal    python3 tests/f4/gui_cal_check.py --buffer "$BS"
want gui_levels && run gui_levels tests/results/f4/gui_levels python3 tests/f4/gui_levels_check.py --buffer "$BS"
want measure && run measure tests/results/f3/apollo "${BENCH[@]}" --measure
want pdc     && run pdc     tests/results/f3/apollo "${BENCH[@]}" --pdc
want faults  && run faults  tests/results/f3/apollo "${BENCH[@]}" --faults --long-run "$LONG"
# F3 S6 render-dialog subset (final review 2026-11-09): SWELL render-window detection in the VST3 .so, closed with Escape by the
# xdotool agent (tests/f3/apollo/x11_ui.py); rn_abort and the GUI-shot scenarios need the macOS Accessibility agent
RN="${NI_RENDER_SCEN:-rn_first,rn_dialog_close,rn_dialog_render}"
want render  && run render  tests/results/f3/apollo python3 tests/f3/apollo/run_apollo.py --buffers "$BS" --scenarios "$RN" --render

{ echo "# Linux tier B $RUN_ID"; echo
  echo "REAPER $(uname -m) headless (Xvfb), JACK dummy 48 kHz buffer $BS (jackd ${NI_JACKD_OPTS--S}), loop = jackd loopback driver + REAPER auto-connect (1 block), allowAnyDriver"; echo
  for f in "$OUT"/*/*-matrix.json; do [ -f "$f" ] || continue
    python3 -c "import json,sys; m=json.load(open(sys.argv[1])); print(f\"- {sys.argv[1].split('/')[-2]}: {m['overall']}\" + (f\" ({m['actual_cases']}/{m['expected_cases']})\" if 'actual_cases' in m else ''))" "$f"
  done; } >"$OUT/README.md"
cat "$OUT/README.md"
[ $rc = 0 ] && echo "tier-b PASS" || echo "tier-b FAIL"
exit $rc
