# Driver identity strings per OS (P0.3, 2026-11-09)

Probe: `tests/reaper/devinfo_probe.lua` (REAPER 7.85), which writes
`GetOS()|MODE|IDENT_IN|IDENT_OUT|SRATE|BSIZE` from `GetAudioDeviceInfo` to `<ResourcePath>/NovaInsert/devinfo.txt`.

| OS | Configuration | GetOS | MODE | IDENT_IN | IDENT_OUT |
|---|---|---|---|---|---|
| macOS | CoreAudio, BlackHole 64ch | `macOS-arm64` | *(empty)* | `CoreAudio BlackHole 64ch` | `CoreAudio BlackHole 64ch` |
| macOS | CoreAudio, device names empty | `macOS-arm64` | *(empty)* | *(empty)* | *(empty)* |
| Linux | ALSA, `default` | `Other` | `ALSA default` | `ALSA default` | `ALSA default` |
| Linux | ALSA, `hw:0,0` | `Other` | `ALSA 0,0 16-bit` | `ALSA 0,0` | `ALSA 0,0` |
| Linux | ALSA, `plughw:0,0` | `Other` | `ALSA plughw:0,0` | `ALSA 0,0` | `ALSA 0,0` |
| Linux | Dummy Audio | `Other` | `Dummy Audio` | `Dummy Audio` | `Dummy Audio` |
| Windows | ASIO, RME Babyface Pro (`ASIO Fireface USB`) | *(not logged)* | `ASIO` | `ASIO ASIO Fireface USB` | `ASIO ASIO Fireface USB` |
| Windows | ASIO4ALL 2.22 (negative row) | *(not logged)* | `ASIO` | `ASIO ASIO4ALL v2` | `ASIO ASIO4ALL v2` |
| Windows | FlexASIO 1.10b (negative row) | *(not logged)* | `ASIO` | `ASIO FlexASIO` | `ASIO FlexASIO` |
| Windows | DirectSound (negative row) | *(not logged)* | `DirectSound` | `DirectSound Primary Sound Capture Driver` | `DirectSound Primary Sound Driver` |

Windows rows: REAPER 7.85 x64 on szakimzk, 2026-11-12, from the S1 driver-gate scenarios (`tests/f3/run_f3_1.py --scenario
dg_asio4all,dg_flexasio,dg_directsound,dg_control`, `f3-driver.csv` `devinfo_*` rows), not from the P0.3 probe.

## Rules that follow

- **Prefix match, not equality.** MODE carries device and format suffixes (`ALSA 0,0 16-bit`), so the driver gate
  matches the driver family by prefix (`ALSA`, `ASIO`).
- **Empty MODE is Unknown, not a mismatch.** macOS returns an empty MODE, so an empty value passes the gate (Unknown →
  allowed), never `DriverUnsupported`.
- **Default device.** An IDENT of `ALSA default` names no concrete device; it counts as unidentified, like the existing
  macOS cases (empty, `<default>`, `CoreAudio Default`).
- **Generic ASIO wrappers show only in IDENT.** ASIO4ALL and FlexASIO report MODE `ASIO` like a native driver; the
  wrapper's name appears in IDENT_IN / IDENT_OUT (`ASIO <registered driver name>`), so the Windows gate checks all three.
- `Dummy Audio` is not ALSA: it is rejected unless `allowAnyDriver` is set (tier B in the container uses that).
