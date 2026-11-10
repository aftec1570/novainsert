# NovaInsert — version history

Latest version: **1.1.1** · [Download the latest release (macOS DMG, Linux tar.gz)](https://github.com/aftec1570/novainsert/releases/latest) ·
[All releases](https://github.com/aftec1570/novainsert/releases) · [Bug and fix ledger](BUGS.md)

## 1.1.1 — 2026-11-10

Bug-fix release for macOS and Linux. Projects, presets (`devices.json`) and settings from 1.0.x and 1.1.0 load unchanged.

- **Fixed: a latency measurement started while the transport plays could fail with "no return".** This happened when a
  project with measure-on-open was opened during playback, or when an instance was inserted or re-measured while playing.
  The program that was still travelling round the hardware loop landed in the silence before the measuring click and
  confused the arrival detection. NovaInsert now holds the send silent and lets the loop drain (about 1.8 s) before it
  sends the click whenever the transport is playing. Measurements with the transport stopped are unchanged.
- macOS: validation tier unchanged (hardware-validated on a UA Apollo bench). Linux: functionally validated in a virtual
  environment; hardware timing validation pending (beta).
- Windows x64 (ASIO) is not in this release; it follows in a later release.

SHA-256:
- `NovaInsert-1.1.1-macOS-arm64.dmg`: `7c64d28ebb082e76bab73dc540ac4ec0c9b9f8e73ef8cecc51ec6a247804f14c`
- `NovaInsert-1.1.1-Linux-x86_64.tar.gz`: `481571ad23c87f3b44915e7e3ba4b69b8b897c09a5a1c071106f6e176f586c4f`

## 1.1.0 — 2026-11-09

Linux release. Projects, presets (`devices.json`) and settings from 1.0.x load unchanged on every platform.

- **New: Linux x86_64 build** (`NovaInsert-1.1.0-Linux-x86_64.tar.gz`, glibc 2.35 or newer, ALSA). `install.sh` copies the
  plug-in to `~/.vst3`. Validation: functionally validated in a virtual environment; hardware timing validation pending (beta).
- **New: audio-driver check.** NovaInsert arms and measures only on REAPER's native driver: CoreAudio on macOS, ALSA on
  Linux. On any other driver (JACK, PulseAudio, Dummy Audio, …) it stays unarmed and shows "REAPER's audio driver is not
  supported. NovaInsert needs ASIO on Windows / ALSA on Linux".
- macOS: unchanged features; validation tier unchanged (hardware-validated on a UA Apollo bench).
- Windows x64 (ASIO) is not in this release; it follows in 1.1.1.

SHA-256:
- `NovaInsert-1.1.0-macOS-arm64.dmg`: `0b4b519215ffdca3ed31baef335e861e9b68ce846b454641d256113049b16185`
- `NovaInsert-1.1.0-Linux-x86_64.tar.gz`: `528d34d012ef9766f0c9611d11cad81933acdfa3c3ba51b63d83771e7530098a`

## 1.0.1 — 2026-11-08

Bug-fix release. Projects, presets (`devices.json`) and settings from 1.0.0 load unchanged.

- **Fixed: mouse-wheel direction on the trim knobs could be reversed** ([#1](https://github.com/aftec1570/novainsert/issues/1)).
  Wheel up raises a trim, following the macOS scroll setting. A scroll utility that reverses only the mouse
  (Logi Options+, Mos, Scroll Reverser, LinearMouse…) leaves macOS's "natural scrolling" flag set, so the knobs turned
  backwards and no plug-in can detect that. New global option **SETUP → Mouse wheel: Reverse** turns them the other way.
- **Fixed: "Interface" in PRESET SETUP went back to Apollo when saving a Custom preset**
  ([#2](https://github.com/aftec1570/novainsert/issues/2)). The hardwired interface list (Apollo x, Apollo Twin MkII,
  Custom) is gone. PRESET SETUP and SETUP now show the audio device as REAPER reports it; the dBu-at-0-dBFS values are
  entered directly (default +24.0 dBu), since REAPER does not report a converter's analog level.

## 1.0.0 — 2026-11-06

First public release: REAPER-only VST3 hardware insert for macOS (Apple silicon) with automatic, sample-accurate
round-trip latency measurement, device presets, mono/stereo routing per direction, VU and bar meters, level calibration
(SEND CAL / RETURN CAL), Insert and Effect modes and automatic realtime rendering.
