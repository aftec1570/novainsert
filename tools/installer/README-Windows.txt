NovaInsert 1.1.0 for Windows x64 (VST3)
=======================================

Install
-------
1. Unpack this zip.
2. Copy the folder "NovaInsert.vst3" to:
       C:\Program Files\Common Files\VST3\
   (administrator rights are needed for that folder).
3. In REAPER: Options > Preferences > Plug-ins > VST > "Re-scan".

This zip is NOT code-signed. Windows SmartScreen or your browser may warn about an "unknown publisher" when you
download or unpack it. That is expected for an unsigned download: check the SHA-256 sum against the one published
with the release (NovaInsert-1.1.0-Windows-x64.zip.sha256), then choose "Keep" / "More info" > "Run anyway" if a
warning appears. Nothing in this package is an installer; you copy the plug-in folder yourself.

Requirements
------------
- Windows 10 or 11, 64-bit (x64 only; no 32-bit, no ARM64).
- REAPER (64-bit) with the ASIO audio driver (Options > Preferences > Audio > Device > Audio system: ASIO).
  On any other driver (WASAPI, WaveOut, DirectSound, Dummy) NovaInsert shows
  "REAPER's audio driver is not supported. NovaInsert needs ASIO on Windows / ALSA on Linux"
  and does not arm or measure. Switch REAPER to ASIO to use it.
- ASIO4ALL, FlexASIO and similar wrapper drivers are test tools, not supported for real work: use your interface's
  native ASIO driver.

Validation
----------
See VERSION.md for the validation tier this Windows build has reached.

More
----
- VERSION.md - release history and validation tiers.
- BUGS.md    - known and fixed bugs.
- LICENSE.txt
