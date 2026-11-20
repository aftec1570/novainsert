# NovaInsert – version history

Latest version: **1.4.4** · [Download the latest release (macOS DMG, Windows zip, Linux tar.gz)](https://github.com/aftec1570/novainsert/releases/latest) ·
[All releases](https://github.com/aftec1570/novainsert/releases) · [Bug and fix ledger](BUGS.md)

## 1.4.4 – 2026-11-20

Bug-fix release for macOS, Windows and Linux. Projects, presets, calibrations and settings load unchanged.

- **Fix:** choosing "No device" in the device list now sets the four channel offsets in SETUP back to 0.0 dB. Before, the
  offsets of the previously selected preset (for example the stored RETURN CAL result) stayed visible and active. The main
  send and return trims and LINK are not touched, and a project restore keeps the offsets the project saved
  ([BUGS.md #13](BUGS.md)).
- Manual: chapters 6 and 10 say what "No device" does to the offsets.
- Validation: a new unit test (preset offsets, then "No device" gives 0.0 dB, then the preset again gives them back); the
  full unit suite on macOS, Windows and Linux; pluginval.

SHA-256:
- `NovaInsert-1.4.4-macOS-arm64.dmg`: `02eec279b644623032ffb3fa19fbf030aee372fb7009126456410cb90dc2f2fa`
- `NovaInsert-1.4.4-Windows-x64.zip`: `a822f9ee0ee47c34c0a339c1718f63933176fa17b7d87634c852a78bafddffb1`
- `NovaInsert-1.4.4-Linux-x86_64.tar.gz`: `87a9d80a6b680e5ce70e04dde8199fd2044d7d0123cb94dcb0eac755eba5f46f`

## 1.4.3 – 2026-11-18

Bug-fix release for macOS, Windows and Linux. Projects, presets, calibrations and settings load unchanged.

- **Fix (macOS):** the 1.4.1 and 1.4.2 installers for macOS carried a plug-in built before the version number was set, so
  the panel said 1.3.0. That build had the bypass handover of 1.4.1 but not the later changes. 1.4.3 is built from the
  release sources and says 1.4.3 ([BUGS.md #11](BUGS.md)). Windows and Linux were not affected.
- **Change:** with the meter unit on dBu, the RETURN meter and its readout now show the level at the converter input,
  before the return trim, so a dBu reading is the analog level whatever the trim does. A neutral loop driven at +4 dBu reads
  0 VU on both meters. With the unit on dBFS the RETURN meter shows the level after the trim against the SETUP 0 VU
  reference, as before ([BUGS.md #12](BUGS.md)). The audio is not changed.
- Manual: the meter chapter now says what the dBu / dBFS switch does to each meter.
- Validation: a new unit test for the meter tap; the full unit suite on macOS, Windows and Linux; pluginval; the macOS
  installer now refuses to package a plug-in that does not carry the release version.

## 1.4.2 – 2026-11-17

Bug-fix release for Windows; macOS and Linux behave as in 1.4.1. Projects, presets, calibrations and settings load unchanged.

- **Fix (Windows):** saving a preset, a setting or a calibration could fail now and then with "write failed" when another
  program (an antivirus scan, the search indexer, another NovaInsert instance) had the file open for a moment. The save
  now waits up to a quarter of a second for the file instead of giving up ([BUGS.md #10](BUGS.md)).
- Validation: a new unit test holds the file open during a save (failed on Windows before the fix); the full unit suite
  on Windows, macOS and Linux.

## 1.4.1 – 2026-11-17

Release for macOS, Windows and Linux. Projects, presets, calibrations and settings from 1.3.0 (and the 1.4beta pre-release)
load unchanged.

- **New: bypassing an instance hands its channels to an instance waiting for them** (forum request). Example: the drums
  were compressed in stereo and printed; bypass that instance and the vocal and guitar instances using the compressor's
  left and right channels as mono presets take over and measure within about a second; the bypassed instance keeps its
  device. Un-bypassed, it claims the channels back once they are free and measures again. A bypass with nobody waiting
  changes nothing, so A/B bypassing never re-measures.
- **Fix:** when a stereo instance released its channels, only one of two waiting mono instances (left or right) got them;
  now both do.
- **New on Linux (beta feature): JACK and PipeWire-JACK**, from the 1.4beta pre-release. NovaInsert arms on REAPER's JACK
  driver, including PipeWire's JACK layer. JACK names no hardware, so every loop is measured whenever a project opens and a
  saved latency is never used as a fallback; re-calibrate after changing JACK connections (the panel says "JACK (beta)").
  Tested on a virtual JACK server only.
- Validation: unit tests on all three platforms; the bypass handover scenario on the Windows bench (RME Babyface Pro, cable
  loops), the Apollo bench (SSL EQ, macOS) and the Linux virtual JACK loop; the Linux tier B suite passes.

## 1.4beta – 2026-11-15 (Linux pre-release)

Pre-release for Linux testers; macOS and Windows stay on 1.3.0 (no change for them). Same projects, presets and settings
as 1.3.0.

- **New (beta): JACK and PipeWire-JACK on Linux.** NovaInsert arms on REAPER's JACK driver, including PipeWire's JACK
  layer, so REAPER on a PipeWire desktop no longer has to be switched to ALSA. JACK names no hardware, so on JACK every
  loop is measured whenever a project opens and a saved latency is never used as a fallback; re-calibrate after changing
  JACK connections (README "JACK and PipeWire on Linux").
- **A beta build says so:** the editor footer reads "BETA BUILD – not a production release" in amber. It installs as the
  same NovaInsert plug-in and replaces the installed 1.3.0; reinstall 1.3.0 to go back. (The first upload, "1.4.0-beta.1",
  only showed the version number; please use 1.4beta.)
- Validation: the Linux tier B suite on a virtual JACK server (measure, delay compensation, render, faults, multi-instance)
  passes; no real interface and no PipeWire system tested yet.

## 1.3.0 – 2026-11-14

Feature release. Projects, presets (`devices.json`), calibrations and settings from 1.0.x to 1.2.1 load unchanged on every
platform; a stored calibration stays valid at the rate it was measured at.

- **New: 44.1, 48, 88.2, 96 and 192 kHz.** NovaInsert used to run at 48 kHz only. Every timing inside it (meters, fades,
  the level CAL tone and window, the delay-compensation checks, the millisecond readouts) now follows the device rate.
  Other rates (176.4, 32 kHz, ...) still show "Unsupported rate".
- **New: changing the sample rate re-measures every loop on its own** ("Sample rate changed"). Change the rate only while
  the transport is stopped, never during playback or recording. A calibration is stored per rate, so going back to a rate
  reuses it.
- **New: no limit on active instances.** Up to 20 could run at once before; a 21st silently got no loop. More than 20 are
  allowed now; 20 is what we have stress-tested, so beyond that the CPU at very small buffers is the limit.
- **Fixed: a stereo unit whose channels sit half a sample apart failed with "Channel asymmetry"** now and then. Two
  channels whose loops lie within half a sample are one loop and lock on the same delay; a real L/R skew of a sample or
  more still fails. ([BUGS.md #7](BUGS.md))
- **Changed: "Unvalidated host version" appears only for REAPER older than 7.6.** It is a notice, not an error.
  ([#8](BUGS.md))
- Validation: macOS hardware-validated on the UA Apollo bench at all five rates (four direct analog loops measure, lock and
  render sample-aligned at each; ADAT loops at 44.1 and 48 kHz) and on REAPER 7.86; the live rate change was proven with the
  transport stopped. Windows (RME Babyface Pro, native ASIO) and Linux: the new rates and the instance limit are covered by
  the unit tests, not yet by hardware runs; Linux stays beta.

SHA-256:
- `NovaInsert-1.3.0-macOS-arm64.dmg`: `cbfec8c15419f3369ec71ca9bbba3d660544e631b01dd7f0a71a124919fc94c5`
- `NovaInsert-1.3.0-Windows-x64.zip`: `38accb9e236f0d260a156ee7958fd908388ed4c666c882673b676499212a2cd6`
- `NovaInsert-1.3.0-Linux-x86_64.tar.gz`: `7bdadc860e844baa58e6c3061f80c7b05479794d4b043fe0c34dbeddf25d68e6`

## 1.2.1 – 2026-11-12

Bug-fix release for the first Windows reports. Projects, presets (`devices.json`) and settings from 1.0.x, 1.1.x and 1.2.0
load unchanged on every platform.

- **Fixed: "Audio device lost" although the interface works** (all platforms; reported on Windows). When REAPER reports
  more channels than it hands out audio for, NovaInsert no longer treats the device as lost. A preset whose own channel
  gets no audio now says so and names the channel; enable it in Preferences > Audio > Device. ([BUGS.md #4](BUGS.md))
- **Fixed: the meter view and unit are remembered.** VU / TUBE / LED / TWIN and dBu / dBFS are stored in `settings.json` and
  kept when REAPER restarts. ([#5](BUGS.md))
- **Windows: ASIO4ALL and FlexASIO are refused.** With these wrappers (or any non-ASIO driver) NovaInsert shows a warning
  and passes the audio through dry; use the interface's own ASIO driver. ([#6](BUGS.md))
- Validation tiers unchanged: macOS hardware-validated (UA Apollo bench); Windows real hardware (RME Babyface Pro, native
  ASIO, cable loops); Linux functionally validated in a virtual environment, hardware timing validation pending (beta).

SHA-256:
- `NovaInsert-1.2.1-macOS-arm64.dmg`: `c8465b1f767393b794efca6e1f89f3502358e5e990b530f743174f07412c1e13`
- `NovaInsert-1.2.1-Windows-x64.zip`: `5c987d4b21407883d084a18e957084889ad3a981552f3c00bd95329b1dc9f885`
- `NovaInsert-1.2.1-Linux-x86_64.tar.gz`: `860b1b113aad023e9351cf9d2e124ddc3c140d25235737544a2b70e3cda2ad8a`

## 1.2.0 – 2026-11-10

Windows release. Projects, presets (`devices.json`) and settings from 1.0.x and 1.1.x load unchanged on every platform.

- **New: Windows x64 build** (`NovaInsert-1.2.0-Windows-x64.zip`, Windows 10 or 11, 64-bit, ASIO). Unpack it and copy
  `NovaInsert.vst3` to `C:\Program Files\Common Files\VST3\`. The zip is not code-signed; check it against its SHA-256.
- Windows validation: real hardware, an RME Babyface Pro on its native ASIO driver with analog cable loops: latency
  measurement at buffer sizes 64 to 512, delay compensation changes, online render and fault handling. Other ASIO
  interfaces and outboard have not been tested yet; reports are welcome.
- macOS and Linux: no functional change from 1.1.1; validation tiers unchanged (macOS hardware-validated on a UA Apollo
  bench; Linux functionally validated in a virtual environment, hardware timing validation pending, beta).

SHA-256:
- `NovaInsert-1.2.0-macOS-arm64.dmg`: `3e4d7fad0acd1512183a97ea4b232e59158f32eb55019f27fa4198950c3c5d30`
- `NovaInsert-1.2.0-Windows-x64.zip`: `7ff5b6628313f850dee25aac934c6a0bcfc642a52e5e8403b0009e842f56616b`
- `NovaInsert-1.2.0-Linux-x86_64.tar.gz`: `a007eae36d928ffc30ececfa92f945cd367034fd783a86057ed05c49b40a1aaf`

## 1.1.1 – 2026-11-10

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

## 1.1.0 – 2026-11-09

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

## 1.0.1 – 2026-11-08

Bug-fix release. Projects, presets (`devices.json`) and settings from 1.0.0 load unchanged.

- **Fixed: mouse-wheel direction on the trim knobs could be reversed** ([#1](https://github.com/aftec1570/novainsert/issues/1)).
  Wheel up raises a trim, following the macOS scroll setting. A scroll utility that reverses only the mouse
  (Logi Options+, Mos, Scroll Reverser, LinearMouse…) leaves macOS's "natural scrolling" flag set, so the knobs turned
  backwards and no plug-in can detect that. New global option **SETUP → Mouse wheel: Reverse** turns them the other way.
- **Fixed: "Interface" in PRESET SETUP went back to Apollo when saving a Custom preset**
  ([#2](https://github.com/aftec1570/novainsert/issues/2)). The hardwired interface list (Apollo x, Apollo Twin MkII,
  Custom) is gone. PRESET SETUP and SETUP now show the audio device as REAPER reports it; the dBu-at-0-dBFS values are
  entered directly (default +24.0 dBu), since REAPER does not report a converter's analog level.

## 1.0.0 – 2026-11-06

First public release: REAPER-only VST3 hardware insert for macOS (Apple silicon) with automatic, sample-accurate
round-trip latency measurement, device presets, mono/stereo routing per direction, VU and bar meters, level calibration
(SEND CAL / RETURN CAL), Insert and Effect modes and automatic realtime rendering.
