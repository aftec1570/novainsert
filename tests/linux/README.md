# Linux x86_64 test rig (OrbStack container on the Mac mini)

Image `novainsert-linux`: Ubuntu 22.04 amd64 (runs under Rosetta in OrbStack), CMake from the Kitware apt repo,
Ninja, Python 3, Xvfb, alsa-utils, the nine JUCE build dependencies, REAPER 7.85 x86_64 at `/opt/reaper/REAPER/reaper`,
non-root user `ni` (passwordless sudo).

    docker build --platform linux/amd64 -t novainsert-linux tests/linux
    docker run --rm -it --platform linux/amd64 -v "$PWD":/work novainsert-linux bash

## Findings (P0.1, 2026-11-09)

| Question | Answer |
|---|---|
| `uname -m` | `x86_64` |
| `snd-aloop` | **not available**: the OrbStack kernel (7.2.12-orbstack) ships no `snd-aloop` module and has no `/lib/modules` |
| Sound card | OrbStack exposes one **virtio-snd** card (playback 1, capture 1), bridged to the Mac's audio. Visible in a container only with `--device /dev/snd --group-add 18` (host `audio` gid is 18, the image's is 29) |
| REAPER headless | starts under `xvfb-run -a /opt/reaper/REAPER/reaper -nosplash -new -cfgfile <dir>/reaper.ini [script.lua]`; the resource path is the ini's folder |
| Lua harness | scripts given on the command line run (`tests/reaper/devinfo_probe.lua` writes `<ResourcePath>/NovaInsert/devinfo.txt`). `f0_1_lifecycle.lua` and others log to `$HOME/Library/Logs/...`, a macOS path: Task 9 moves logs under `reaper.GetResourcePath()` |

## REAPER audio keys on Linux (`[REAPER]` in reaper.ini)

`linux_audio_mode`: 0 = JACK, **1 = ALSA**, **2 = Dummy Audio**, 3 = PulseAudio/PipeWire (0/3 do not open in the
container; not supported by NovaInsert anyway). ALSA device: `alsa_indev`, `alsa_outdev` (`default`, `hw:0,0`,
`plughw:0,0`), plus `linux_audio_srate`, `linux_audio_bsize`, `linux_audio_bufs`, `linux_audio_nch_in`,
`linux_audio_nch_out`, `linux_audio_bits`, `alsa_mmap`, `alsa_rtprio`.

`GetAudioDeviceInfo` readings (see `docs/port/driver-identity.md`): `MODE` = `ALSA default` / `ALSA 0,0 16-bit` /
`ALSA plughw:0,0`, `IDENT_IN`/`IDENT_OUT` = `ALSA default` / `ALSA 0,0` / `ALSA plughw:0,0`; Dummy Audio:
`MODE = IDENT_* = Dummy Audio`. `GetOS()` returns `Other`.

## Tier A and tier B (Task 10)

    docker run --rm --platform linux/amd64 -v "$PWD":/work -w /work novainsert-linux tests/linux/run_tier_a.sh
    docker run --rm --platform linux/amd64 --cap-add SYS_NICE --ulimit rtprio=95 --ulimit memlock=-1 \
        -v "$PWD":/work -w /work -e NI_JACKD_OPTS=-S novainsert-linux tests/linux/run_tier_b.sh [--only a,b]

Tier A: configure, build, unit tests, pluginval strictness 10. Tier B: REAPER headless under Xvfb, evidence in
`tests/results/linux/<run-id>/` (each runner's matrix + summary, console logs, `README.md`), exit 0 = every runner PASS.
The F0 runners need the spike bundles: configure the build with `-DNI_BUILD_SPIKES=ON`, or point `NI_SPIKE_VST3_DIR` at a
folder holding the five `NovaInsert F0 * Spike.vst3` bundles.

**Audio = JACK dummy server, loop = jackd's loopback driver.** No `snd-aloop` (above) and ALSA Dummy Audio has no loop,
so tier B starts `jackd -L 63 -d dummy -r 48000 -p $NI_BS -C 0 -P 0` (default 256: Rosetta-emulated x86_64 underruns at
64) and REAPER in JACK mode. With no system ports, `loopback:playback_<n>` / `loopback:capture_<n>` are the only physical
ports, and REAPER's own JACK auto-connect (`jack_options=0`, its default; 2 turns it off) wires `REAPER:out<n> ->
loopback:playback_<n>` and `loopback:capture_<n> -> REAPER:in<n>`. `REAPER:in64` has no partner (the no-return
fixture). The loop is **1 block** (BlackHole on macOS: 2 blocks); F0.2 measures it (`hw_loop_lag`) and its lag
invariant uses the measured value.

Why not an external connector: REAPER re-registers its JACK client around every online render, which drops every
connection. A keeper reacting to JACK's port-registration callback needed ~10 ms per `jack_connect` (0.5–0.7 s for 55
pairs), so renders with many outboard channels started with late channels (F0.6 `load20`: 13 of 21 pairs 1–8 blocks
late). REAPER's auto-connect runs inside its device open, before audio starts: every render exact.

- NovaInsert's driver gate rejects JACK, so the harness sets `allowAnyDriver` (`NI_ALLOW_ANY_DRIVER=1`); S1
  `driver_gate` checks the rejection itself. The positive ALSA gate check is `--device-snd` (`--device /dev/snd
  --group-add 18`, `alsa_*dev=default`: the Mac's default audio device through virtio; the project is silent).
- `NI_JACKD_OPTS=-S` (synchronous mode) and the RT flags (`SYS_NICE`, `rtprio`, `memlock`) keep the dummy graph stable.
- The JACK dummy period is a VM timer (jackd logs `XRun = 5..13 usec` late wake-ups nearly every cycle): F0.2 late cycles
  and F0.6 xrun-timestamp changes are notes on JACK, failures on real devices; sample integrity stays gated by the renders.
- Not on Linux: F0.5 (CoreAudio/BlackHole loop fixture), S6 render agent (AppleScript), the HID/screenshot F4 GUI
  checks (gui_devices, gui_bugfix, gui_bars, gui_skin, gui_resize). The Lua-driven GUI checks gui_cal and gui_levels run.
- Measured loop lag on Linux is a property of the JACK graph, not a hardware fact.

Limitations: x86_64 under Rosetta (slow, timing not representative), no real hardware, no `snd-aloop`, no ALSA loop.
