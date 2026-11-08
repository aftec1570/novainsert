# NovaInsert — bug and fix ledger

Every reported bug, its root cause, the fix and the release that carries it. Report new bugs at
[GitHub Issues](https://github.com/aftec1570/novainsert/issues) or in the
[REAPER forum thread](https://forum.cockos.com/showthread.php?t=312429).
Latest version: [download](https://github.com/aftec1570/novainsert/releases/latest) · [version history](VERSION.md)

| # | Reported | Summary | Status | Fixed in |
|---|---|---|---|---|
| [1](https://github.com/aftec1570/novainsert/issues/1) | 2026-11-08 | Mouse wheel direction on trim knobs is reversed | Fixed | 1.0.1 |
| [2](https://github.com/aftec1570/novainsert/issues/2) | 2026-11-08 | "Interface" in Preset Setup goes back to Apollo when saving a Custom preset | Fixed | 1.0.1 |

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
