# NovaInsert — bug and fix ledger

Every reported bug, its root cause, the fix and the release that carries it. Report new bugs at
[GitHub Issues](https://github.com/aftec1570/novainsert/issues) or in the
[REAPER forum thread](https://forum.cockos.com/showthread.php?t=312429).
Latest version: [download](https://github.com/aftec1570/novainsert/releases/latest) · [version history](VERSION.md)

| # | Reported | Summary | Status | Fixed in |
|---|---|---|---|---|
| [1](https://github.com/aftec1570/novainsert/issues/1) | 2026-11-08 | Mouse wheel direction on trim knobs is reversed | Fixed | 1.0.1 |
| [2](https://github.com/aftec1570/novainsert/issues/2) | 2026-11-08 | "Interface" in Preset Setup goes back to Apollo when saving a Custom preset | Fixed | 1.0.1 |
| 3 | 2026-11-10 | Measuring while the transport plays fails with "no return" | Fixed | 1.1.1 |
| 4 | 2026-11-12 | "Audio device lost" on Windows although the interface works in ReaInsert | Fixed | 1.2.1 |
| 5 | 2026-11-12 | The meter view (VU / TUBE / LED / TWIN) comes back as VU | Fixed | 1.2.1 |
| 6 | 2026-11-12 | ASIO4ALL and FlexASIO pass the Windows driver check although they are not supported | Fixed | 1.2.1 |

## #1 — Mouse wheel direction on trim knobs is reversed

- **Reported:** 2026-11-08, GitHub issue #1 (thanks to sguyader).
- **Symptom:** wheel down turned the SEND / RETURN trim clockwise (up), wheel up counter-clockwise (down).
- **Root cause:** the knobs follow the physical wheel direction by undoing macOS "natural scrolling" with the flag the
  system puts on each scroll event. Mouse utilities that reverse only the mouse (Logi Options+, Mos, Scroll Reverser,
  LinearMouse…) flip the wheel but leave that flag set, so the knob ends up backwards. A plug-in cannot tell the two
  cases apart.
- **Fix (1.0.1):** new global option SETUP → "Mouse wheel: Reverse" (stored as `reverseWheel` in `settings.json`, shared
  by all instances). Default unchanged (follows the system).
- **Verified:** unit test for the setting (default off, persisted, shared between instances, a corrupt file is never
  overwritten); in REAPER 7.85 with HID wheel events over the SEND trim: wheel up raised it, and with Reverse ticked the
  same wheel lowered it.

## #2 — "Interface" in Preset Setup goes back to Apollo when saving a Custom preset

- **Reported:** 2026-11-08, GitHub issue #2 (thanks to sguyader).
- **Symptom:** choosing "Custom" as Interface and pressing SAVE showed "Apollo" again unless a dBu value was edited.
- **Root cause:** the Interface list was not stored; it was guessed back from the two dBu values, and 24.0 / 24.0 always
  read as "Apollo x". The SETUP page re-guessed it on every refresh as well.
- **Fix (1.0.1):** the hardwired interface list (Apollo x, Apollo Twin MkII, Custom) is removed. PRESET SETUP and SETUP
  show the audio device REAPER reports (read-only); the dBu-at-0-dBFS values are typed in (REAPER does not report the
  analog level). Nothing to choose, so nothing can revert. Presets saved with 1.0.0 load unchanged.
- **Verified:** unit test for the interface line; in REAPER 7.85 a preset saved through PRESET SETUP keeps its typed
  levels and shows "CoreAudio BlackHole 64ch" as reported by REAPER, after SAVE and after reopening.

## #4 — "Audio device lost" on Windows although the interface works in ReaInsert

- **Reported:** 2026-11-12 in the REAPER forum thread (REAPER 7.85, Windows 10, Yamaha Steinberg MR816 CSX on its native
  FireWire ASIO driver 1.7.8).
- **Symptom:** "Select a free device first", then "Audio device lost"; the interface's channels were recognised and worked
  in ReaInsert.
- **Root cause:** the reporter's diagnostics showed REAPER reporting 40 inputs but handing out audio only for inputs 1–24.
  NovaInsert treated any single channel without audio as a lost device, so after half a second it stopped running, although
  the preset's own channels (output 17, input 19) were fine.
- **Fix (1.2.1):** a device counts as lost only when REAPER hands out no channel at all in a direction. A preset whose own
  channel is missing says so: "REAPER delivers no audio on this preset's channel. Enable it in Preferences > Audio > Device
  (input/output range)", with the channel named (e.g. "Not delivered by REAPER: input 25"); it measures as soon as the
  channel is delivered. An independent review of the fix also found that an instance could stay "Queued" after its device
  came back while a measurement had been cancelled; that is fixed in the same release.
- **Verified:** new unit tests reproduce the logged condition (40 reported, 24 delivered) and the stuck-queue case; full
  unit suite on macOS (release, debug, ASan, TSan) and Windows; pluginval strictness 10 on both; live in REAPER on macOS
  (BlackHole) and on the Windows Babyface bench (PDC, fault scenarios). The exact driver behaviour could not be reproduced
  on the bench (REAPER's own input range shrinks the reported count instead).

## #5 — The meter view (VU / TUBE / LED / TWIN) comes back as VU

- **Reported:** 2026-11-12 in the REAPER forum thread (same report as #4).
- **Root cause:** the meter view and the dBu / dBFS unit were kept only while REAPER ran, so every restart opened on VU.
- **Fix (1.2.1):** both are stored in `settings.json` (`meterView`, `meterUnit`), one setting for every instance, kept across
  restarts; not saved in the project.
- **Verified:** unit test (stored, read by a new process, other settings kept, invalid values read as the defaults).

## #6 — ASIO4ALL and FlexASIO pass the Windows driver check

- **Reported:** 2026-11-12, found while investigating #4.
- **Root cause:** REAPER reports these wrappers as ASIO, so the driver check accepted them although they are not supported.
- **Fix (1.2.1, Windows only):** an ASIO device whose name contains ASIO4ALL or FlexASIO, and any non-ASIO driver, shows
  "Audio driver not supported, NovaInsert is bypassed. Use the interface's own ASIO driver (not ASIO4ALL or FlexASIO)" and
  passes the audio through dry. macOS and Linux are unchanged.
- **Verified:** unit tests; live on the Windows bench with ASIO4ALL 2.22, FlexASIO 1.10b, DirectSound and the Babyface's
  own driver as the control.

## #3 — Measuring while the transport plays fails with "no return"

- **Reported:** 2026-11-10, found by the Hillside Studios test bench (UA Apollo, scenario `ap_pdc_running_defer`);
  reproduced on BlackHole.
- **Symptom:** a measurement started during playback (project with measure-on-open opened while playing, an instance
  inserted or re-measured while playing) failed both attempts with "no return".
- **Root cause:** the program sent until the probe started was still travelling round the hardware loop. It arrived in the
  click's 20 ms pre-silence and raised the arrival threshold above the click.
- **Fix (1.1.1):** while the transport plays, NovaInsert holds the send silent from the settle on and extends the settle
  to the loop drain (post-silence + maximum loop length, about 1.8 s); play starting during the settle is caught at the
  next tick. Stopped-transport timing is unchanged.
- **Verified:** two new unit tests (measuring while playing locks the click; play starting during the settle silences the
  send), failing before the fix and passing after it; full unit suite on release, debug and ASan builds; live in REAPER on
  BlackHole and on the UA Apollo bench.
