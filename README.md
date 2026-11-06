# NovaInsert

**A REAPER-only VST3 hardware insert for macOS (Apple silicon) that measures its own latency.**
Version 0.1.0 · free to use, source shared, provided as is without any warranty (see [License](#license)).

![NovaInsert in REAPER](docs/manual/img/reaper_window.png)

NovaInsert puts a piece of outboard gear into a REAPER track like any other plug-in. It sends the track to a physical
output of your audio interface, takes the return from a physical input, measures the exact round-trip delay through the
converters, cables and the device itself, and tells REAPER that delay so the return lines up to the sample. It also keeps
the analog levels where the outboard expects them, with VU meters, trims and a one-press level calibration.

The full [user manual](docs/manual/NovaInsert-User-Manual.md) ([PDF](docs/manual/NovaInsert-User-Manual.pdf)) ships in
the DMG. This page is the short version.

## What it does

- **Measures latency itself.** No ping button. When you pick a device, NovaInsert plays a short sweep through the loop,
  three times, and only accepts the delay when all three runs agree to the sample. The result shows as **Locked** with the
  loop length in samples.
- **Keeps the result right.** If the buffer size, the audio device, the routing or the mode changes, the plug-in
  re-measures or re-applies a stored calibration on its own and tells you why. A wrong value is never used silently.
- **Compensates exactly.** The delay reported to REAPER is the measured loop plus a two-block margin, rounded up to whole
  blocks. REAPER's plug-in delay compensation does the rest.
- **Renders without surprises.** Starting a render switches REAPER to realtime rendering automatically and restores your
  setting afterwards. A render is refused, with a message naming the track, if any insert is not ready.
- **Handles many devices.** Up to twenty instances in one project. Each physical channel has one owner; a second instance
  on the same channels waits instead of corrupting the first one.
- **Gets the levels right.** A SEND meter referenced to +4 dBu, a RETURN meter referenced to your converter's alignment,
  trims from −24 to +12 dB, LINK, and a level calibration that puts the send at the outboard's working point and brings the
  return back to digital unity.
- **Explicit mono and stereo.** Each preset says whether its send and its return are mono or stereo. A mono send carries
  the track's mix on one output; a mono return comes back on both sides. A stereo unit can also be used one side at a time.
- **A device library.** Every unit is a named preset. Pick "Pultec" and the FX slot in the mixer reads "Pultec".

## Safety notice

NovaInsert sends signals to real outputs and reads real inputs. A wrong channel number, a cable into a monitor path or an
accidental feedback loop can produce loud, sustained sound.

- Start with the monitor level low the first time you select a device.
- Never choose output channels that feed your monitors, headphones or a cue mix.
- Never calibrate into a monitoring path: the CAL tone is a steady sine at 0 VU.
- Check the preset's channel numbers against the interface's own routing before use.

You are responsible for the routing, the monitoring level and the hardware.

## Requirements

| Requirement | Value |
|---|---|
| Host | REAPER 7.83 or newer (validated on 7.83 and 7.85), macOS, Apple silicon (arm64) |
| Plug-in format | VST3 only |
| Sample rate | 48 kHz |
| Audio device | one specific CoreAudio device selected in REAPER, not the system default |
| Instances | up to 20 active instances validated |
| Loop length | up to 65 536 samples |

## Install

**From the DMG** (`NovaInsert-0.1.0.dmg`, built by `tools/make_installer.sh`):

1. Open the DMG and double-click "Install NovaInsert 0.1.0.pkg".
2. Read and accept the license agreement; the installer copies the plug-in to `/Library/Audio/Plug-Ins/VST3`.
3. The package is signed with an Apple Developer ID and notarized by Apple, so macOS opens it directly.
4. In REAPER: Options → Preferences → Plug-ins → VST → "Re-scan". Add "VST3: NovaInsert" to a track.


## First use: define your devices

A device is a preset: a name, a mono or stereo send with its output channel(s), a mono or stereo return with its input
channel(s), the mode and the level settings. Presets are created and edited on the DEVICES page of the editor; the name
you give a preset is what the device list and the FX slot in REAPER's mixer show.

![The DEVICES page](docs/manual/img/devices_page.png)

Open the editor, press DEVICES, then NEW:

1. **Name** — the preset name; it appears in the FX slot the moment the preset is saved.
2. **Send** — MONO or STEREO, then the physical output channel(s), picked by the name REAPER reports ("3 · Apollo Out 3").
   A mono send carries the average of the track's two channels on its one output.
3. **Return** — MONO or STEREO, then the physical input channel(s). A mono return feeds both channels of the track.
4. **Mode** — INSERT for compressors, equalisers and other gear that keeps timing; EFFECT for reverbs and delays.
5. **Interface / dBu at 0 dBFS / 0 VU reference / Outboard max** — the level settings, stored with the preset.
6. **SAVE** — writes the preset, selects it in this instance and starts the measurement at once. Nothing about latency is
   stored with a preset: every selection measures the loop again, so a preset is always checked against the hardware as
   it is now.

DUPLICATE is the quickest way to store the two channels of a stereo unit as "SSL EQ 1", "SSL EQ 2" and "SSL EQ stereo".
DELETE is refused while another track uses the preset. The presets live in
`~/Library/Application Support/REAPER/NovaInsert/devices.json` and can also be edited by hand with REAPER closed.

## The editor

![Editor overview with numbered areas](docs/manual/img/overview.png)

| № | Area | What it does |
|---|---|---|
| 1 | Device selector | names the selected device and the physical channels it owns; click to pick another preset or "No device" |
| 2 | Status lamp | the measurement state in colour and text, with the measured LOOP and the PDC reported to REAPER |
| 3 | SEND meter | VU of the signal leaving for the outboard; 0 VU = +4 dBu at the converter output |
| 4 | RETURN meter | VU of the signal coming back; 0 VU = the dBFS reference set in SETUP |
| 5 | SEND TRIM | gain before the converter, −24 to +12 dB; double-click resets to 0 |
| 6 | CAL +4 dBu | starts the send calibration with a reference tone |
| 7 | LINK | couples the two trims: more send, less return, same mix level |
| 8 | CAL (return) | measures the return and sets the return trim to digital unity |
| 9 | RETURN TRIM | gain after the converter input, −24 to +12 dB |
| 10 | MODE | INSERT (measured loop) or EFFECT (driver latency, tails preserved) |
| 11 | SEND and RETURN | shown for a stereo send / return only: STEREO uses both channels, L or R uses that one side of the unit |
| 12 | DEVICES | the preset editor |
| 13 | SETUP | converter alignment, 0 VU reference, channel offsets, editor size |
| 14 | Status lines | calibration messages, the failure reason, warnings and tips |
| 15 | Footer | time of the last measurement, the algorithm version and the plug-in version |
| 16 | VU / BARS | switches the meter area between the two VU faces and the level bars |

The window resizes from its lower right corner between 50 % and 300 %; everything is vector-drawn, so it stays sharp at
any size and on Retina displays.

### States and the status lamp

| Colour | State | Meaning |
|---|---|---|
| grey | Idle | no device selected |
| blue | Queued | waiting for other instances to finish measuring (the position is shown) |
| blue | Measuring | the sweep is running; the track output is muted while it runs |
| blue | Applying latency | measured; waiting for REAPER to pick up the new delay on the next play |
| green | Locked | aligned; LOOP is the measured round trip, PDC the delay reported to REAPER |
| amber | Stale | something changed since the measurement; a re-measure is pending |
| red | Failed | the measurement did not lock; the reason is on the status line |
| red | Unavailable | the channels are owned by another instance, or the audio device is gone |
| red | Unsupported host | a REAPER setting NovaInsert needs is off (anticipative FX) or the REAPER version lacks an API |
| red | Unsupported sample rate | the project is not at 48 kHz |

![Measuring](docs/manual/img/measuring.png)

A failed measurement names its cause: no signal came back (cable or channel number), the return clipped, the loop had two
candidate delays (an echo or crosstalk from a neighbouring unit), or the return was too weak. Fix the cause and re-select
the device to measure again.

### Meters, trims and LINK

![PEAK and CLIP lit](docs/manual/img/peak_clip.png)

The two VU meters move like real ones (IEC 60268-17 ballistics): a mono device shows one needle, a stereo device two,
black for L and red for R, with a dBFS sub-scale under the VU scale. PEAK lights when the send reaches −0.1 dBFS, CLIP
when a returned sample is above 0 dBFS (the converter input overloaded). Press BARS for level bars in the style of REAPER's
own meters, with dBu above and dBFS below, RMS band, peak hold and the 0 VU reference line.

![The BARS meter view](docs/manual/img/bars.png)

SEND TRIM sits before the converter output, RETURN TRIM after the converter input, each −24 to +12 dB; both are REAPER
parameters and can be automated. With LINK on, raising the send by 3 dB lowers the return by 3 dB, so you can drive the
outboard harder without changing the level of the mix.

### Level calibration

Most outboard expects line level around +4 dBu. A converter aligned to +24 dBu at 0 dBFS sends a full-scale DAW signal
20 dB above that. The calibration solves this per device, once:

1. **SETUP:** choose the interface preset ("Apollo x / +4 · 24.0", "Apollo Twin MkII · 20.2" or Custom, separately for
   send and return) and the 0 VU reference you work with.
2. Put the outboard in a neutral state: unity gain, EQ flat, compression off.
3. **CAL +4 dBu:** NovaInsert sets the send trim that puts 0 VU at +4 dBu at the converter output and plays a steady tone
   at exactly 0 VU. Read the outboard's own input meter, adjust SEND TRIM if needed, press again to finish.
4. **CAL (return):** the tone goes through the neutral outboard; NovaInsert measures what comes back and sets the return
   trim so the plug-in's output equals its input. Stereo units are calibrated per channel. If the return clips at the
   converter, nothing is written and you are asked to lower the outboard's output.

![RETURN CAL finished](docs/manual/img/return_cal_done.png)

The result is stored in the instance and as the default of the device preset.

## Insert and Effect mode

**INSERT** is for gear whose return is the processed version of the send: equalisers, compressors, summing, tape. The loop
is measured and the return is placed exactly.

**EFFECT** is for gear with a tail: reverbs, delays. Nothing is ever measured; NovaInsert reports the audio driver's own
round trip plus the two-block margin, re-reads it on every buffer change, and leaves the return alone. Alignment in Effect
mode is as exact as the driver's figure, not sample-measured.

## Mono and stereo routing

- **Stereo send** — L to the first output, R to the second. No summing, no swapping.
- **Mono send** — (L + R) / 2 to the one output; a mono track leaves at exactly its own level.
- **Stereo return** — the first input to L, the second to R.
- **Mono return** — the one input to both channels of the track.

For a stereo send or return the main panel shows a SEND or RETURN selector: STEREO uses both channels of the unit, L or R
uses only that side as a mono loop. The selectors are part of the calibration key, so changing them re-measures.

## Rendering

Hardware cannot run faster than real time, so a render through NovaInsert is a realtime render, and you do not have to
remember this: starting a render sets REAPER's render speed to realtime and restores your setting afterwards. Before the
render starts every active instance is checked; if one is not Locked the render is refused and a message names the
tracks. Freeze, apply FX and the render queue cannot go through hardware and are refused while an instance is active.

REAPER fills the compensation delay from the start of each play or render pass, so the first "loop length" samples of a
pass have no hardware return yet. NovaInsert reports the gap instead of hiding it; enable REAPER's pre-roll and set the
"Pre-roll lead-in" parameter to clear the tip.

## Several devices, anticipative FX, files

- Each physical channel has one owner. A second instance sharing a channel shows "Device in use by <track>", stays muted
  and takes over when the owner releases the channels. Measurements of different units run in parallel; a sweep disturbed
  by a neighbour's sweep is repeated alone.
- REAPER's "anticipative FX processing" breaks a live hardware loop. NovaInsert turns it off for every track that holds an
  instance (and the folders above it) and restores it when the last instance leaves. The global preference is never touched.
- `…/REAPER/NovaInsert/devices.json` holds the presets, `…/REAPER/NovaInsert/calibrations.json` the measured loops (one
  per interface, channels, buffer size, routing and mode). The REAPER project stores a snapshot of the instance's preset and
  calibration, so a later edit of the library cannot change how an old project sounds.

## Parameters

| Parameter | Values | Notes |
|---|---|---|
| Device | 0 = none, 1… = library index | the same as the device selector |
| Mode | Insert, Effect | |
| Send / Return | Stereo, L, R | the side of a stereo send / return; a mono direction ignores it |
| Send trim, Return trim | −24 … +12 dB | |
| Link | off/on | |
| Pre-roll lead-in | off/on | hides the start-gap tip |
| Re-apply anticipative FX fix | trigger | the same as the button |
| State, Physical loop, Reported PDC, Inherent gap, CAL action, CAL status | read only | for scripts and controllers |

## Limitations of version 0.1.0

- 48 kHz, REAPER 7.83 or newer (validated on 7.83 and 7.85), macOS Apple silicon, VST3 only.
- Presets are stored by position: deleting one shifts the later ones in projects not reopened since.
- The first "loop length" samples of each play or render pass have no hardware return.
- Effect mode alignment is driver-reported, not sample-measured.
- Freeze, apply FX and the render queue are refused while an instance is active.
- A change of the outboard's own settings after the calibration is not detected.
- Analog overload inside the outboard is not detectable.

## License

Free to use, including in professional and commercial audio work; the source is shared. Provided **as is, without any
warranty**; the author accepts no liability for damage to equipment, hearing or data. Commercial reuse of the source code
or the GUI design requires prior consultation with the author. Full terms: `LICENSE.md`. Built with JUCE 8 (JUCE 8 EULA)
and the VST3 SDK (MIT). VST is a trademark of Steinberg Media Technologies GmbH. REAPER is a trademark of Cockos
Incorporated.
