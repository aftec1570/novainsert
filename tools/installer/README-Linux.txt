NovaInsert for Linux x86_64 (VST3)
==================================

Install
-------
Current user (no sudo):
    ./install.sh
  copies NovaInsert.vst3 to ~/.vst3/ (replacing an older copy).

All users (manual):
    sudo cp -r NovaInsert.vst3 /usr/lib/vst3/

Then in REAPER: Options > Preferences > Plug-ins > VST > "Re-scan".

Requirements
------------
- Linux x86_64 with glibc 2.35 or newer (Ubuntu 22.04 or newer, or an equivalent distribution). No ARM builds.
- REAPER (x86_64) with the ALSA audio driver (Options > Preferences > Audio > Device > Audio system: ALSA),
  or, as a beta, the JACK driver (JACK or PipeWire's JACK layer, pipewire-jack). On any other driver (PulseAudio,
  Dummy) NovaInsert shows
  "REAPER's audio driver is not supported. NovaInsert needs ASIO on Windows / ALSA or JACK on Linux"
  and does not arm or measure.

JACK / PipeWire (beta)
----------------------
- JACK does not tell REAPER which interface is behind the graph, so on JACK every instance measures its loop whenever
  a project opens, and a failed measurement is never replaced by the latency saved with the project.
- NovaInsert's channel numbers are REAPER's JACK ports (REAPER:out11, REAPER:in9, ...). Set up your JACK connections
  before you calibrate, and press RE-CALIBRATION after changing one: NovaInsert does not see a re-patch.
- PipeWire may change its quantum (buffer size) or sample rate when another application asks; NovaInsert re-measures
  when REAPER reports it. To keep them fixed:
      pw-metadata -n settings 0 clock.force-quantum 256
      pw-metadata -n settings 0 clock.force-rate 48000
- Tested on a virtual JACK server only, not yet with a real interface. Reports with the diagnostics CSV are welcome.

Validation
----------
Linux is functionally validated in a virtual environment; hardware timing validation pending (beta).
See VERSION.md for details, and please report results from real ALSA or JACK/PipeWire interfaces.

More
----
- VERSION.md - release history and validation tiers.
- BUGS.md    - known and fixed bugs.
- LICENSE.txt
