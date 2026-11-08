# NovaInsert — version history

Latest version: **1.0.1** · [Download the latest signed and notarized DMG](https://github.com/aftec1570/novainsert/releases/latest) ·
[All releases](https://github.com/aftec1570/novainsert/releases) · [Bug and fix ledger](BUGS.md)

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
