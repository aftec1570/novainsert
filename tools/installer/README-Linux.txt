NovaInsert 1.1.0 for Linux x86_64 (VST3)
========================================

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
- REAPER (x86_64) with the ALSA audio driver (Options > Preferences > Audio > Device > Audio system: ALSA).
  On any other driver (JACK, PulseAudio, Dummy) NovaInsert shows
  "REAPER's audio driver is not supported. NovaInsert needs ASIO on Windows / ALSA on Linux"
  and does not arm or measure. Switch REAPER to ALSA to use it.

Validation
----------
Linux is functionally validated in a virtual environment; hardware timing validation pending (beta).
See VERSION.md for details, and please report results from real ALSA interfaces.

More
----
- VERSION.md - release history and validation tiers.
- BUGS.md    - known and fixed bugs.
- LICENSE.txt
