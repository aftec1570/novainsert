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
| Windows | ASIO (ReaRoute, FL Studio ASIO) | *pending* | *pending* | *pending* | *pending* |
| Windows | WASAPI / WaveOut (negative rows) | *pending* | *pending* | *pending* | *pending* |

## Rules that follow

- **Prefix match, not equality.** MODE carries device and format suffixes (`ALSA 0,0 16-bit`), so the driver gate
  matches the driver family by prefix (`ALSA`, `ASIO`).
- **Empty MODE is Unknown, not a mismatch.** macOS returns an empty MODE, so an empty value passes the gate (Unknown →
  allowed), never `DriverUnsupported`.
- **Default device.** An IDENT of `ALSA default` names no concrete device; it counts as unidentified, like the existing
  macOS cases (empty, `<default>`, `CoreAudio Default`).
- `Dummy Audio` is not ALSA: it is rejected unless `allowAnyDriver` is set (tier B in the container uses that).
