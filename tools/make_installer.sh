#!/bin/bash
# Build the NovaInsert macOS installer: component pkg -> product pkg (welcome, license to agree, conclusion) -> DMG.
#   tools/make_installer.sh [build dir] [out dir]
# Uses the Release VST3 already built in <build dir> (default build-release). Unsigned unless
#   NI_SIGN_IDENTITY      "Developer ID Application: Name (TEAMID)"  signs the VST3 (hardened runtime, timestamp)
#   NI_INSTALLER_IDENTITY "Developer ID Installer: Name (TEAMID)"    signs the product pkg
#   NI_NOTARY_PROFILE     keychain profile from `xcrun notarytool store-credentials <name>`: notarises + staples pkg and DMG
# are set (README.md "Install" has the release checklist). The READ ME in the DMG states the resulting Gatekeeper situation.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build-release}"; OUT="${2:-$ROOT/dist}"
VST3="$BUILD/src/NovaInsert_artefacts/Release/VST3/NovaInsert.vst3"
[ -d "$VST3" ] || { echo "no Release VST3 at $VST3 (build NovaInsert_VST3 first)" >&2; exit 2; }
VERSION=$(sed -n 's/.*kVersion = "\([^"]*\)".*/\1/p' "$ROOT/src/engine/version.hpp"); [ -n "$VERSION" ] || { echo "version not found" >&2; exit 2; }
PLIST_VER=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$VST3/Contents/Info.plist")
[ "$PLIST_VER" = "$VERSION" ] || echo "note: bundle CFBundleShortVersionString $PLIST_VER != kVersion $VERSION (installer uses $VERSION)" >&2
WORK=$(mktemp -d "${TMPDIR:-/tmp}/novainsert-installer.XXXXXX"); trap 'rc=$?; rm -rf "$WORK"; exit $rc' EXIT
mkdir -p "$WORK/root/Library/Audio/Plug-Ins/VST3" "$WORK/res" "$WORK/dmg" "$OUT"
cp -R "$VST3" "$WORK/root/Library/Audio/Plug-Ins/VST3/"
xattr -cr "$WORK/root/Library/Audio/Plug-Ins/VST3/NovaInsert.vst3"
BUNDLE="$WORK/root/Library/Audio/Plug-Ins/VST3/NovaInsert.vst3"
if [ -n "${NI_SIGN_IDENTITY:-}" ]; then
    # one Mach-O inside, no nested code: sign the bundle itself (--deep is deprecated); ad-hoc "-" is for local tests only
    TS=--timestamp; [ "$NI_SIGN_IDENTITY" = "-" ] && TS=--force   # bash 3.2: no empty arrays under set -u
    codesign --force --options runtime "$TS" --sign "$NI_SIGN_IDENTITY" "$BUNDLE"
    codesign --verify --strict --verbose=1 "$BUNDLE"
fi
# installer resources: the license the user must agree to is the repo LICENSE.md, shown as plain text
cp "$ROOT/LICENSE.md" "$WORK/res/LICENSE.txt"
for f in welcome conclusion; do sed "s/@VERSION@/$VERSION/g" "$ROOT/tools/installer/$f.html" > "$WORK/res/$f.html"; done
sed "s/@VERSION@/$VERSION/g" "$ROOT/tools/installer/distribution.xml" > "$WORK/distribution.xml"
pkgbuild --root "$WORK/root" --identifier com.hillsidestudios.NovaInsert.vst3 --version "$VERSION" --install-location / \
    --ownership recommended "$WORK/NovaInsert-vst3.pkg" >/dev/null
PRODUCT="$WORK/dmg/Install NovaInsert $VERSION.pkg"
if [ -n "${NI_INSTALLER_IDENTITY:-}" ]; then
    productbuild --distribution "$WORK/distribution.xml" --resources "$WORK/res" --package-path "$WORK" --sign "$NI_INSTALLER_IDENTITY" --timestamp "$PRODUCT" >/dev/null
else
    productbuild --distribution "$WORK/distribution.xml" --resources "$WORK/res" --package-path "$WORK" "$PRODUCT" >/dev/null
fi
notarize() {   # $1 = file; submit, wait, staple; the notarytool log stays in $OUT for the evidence trail
    xcrun notarytool submit "$1" --keychain-profile "$NI_NOTARY_PROFILE" --wait 2>&1 | tee "$OUT/notarize-$(basename "$1").log" | grep -E 'id:|status:' | tail -n 2
    grep -q 'status: Accepted' "$OUT/notarize-$(basename "$1").log" || { echo "notarisation of $1 not accepted (see $OUT)" >&2; exit 3; }
    xcrun stapler staple "$1" >/dev/null
}
if [ -n "${NI_NOTARY_PROFILE:-}" ]; then
    [ -n "${NI_INSTALLER_IDENTITY:-}" ] && [ -n "${NI_SIGN_IDENTITY:-}" ] || { echo "NI_NOTARY_PROFILE needs both signing identities" >&2; exit 2; }
    notarize "$PRODUCT"
    GATE="This package is signed with an Apple Developer ID and notarized by Apple: it opens like any other installer."
elif [ -n "${NI_INSTALLER_IDENTITY:-}" ] || [ -n "${NI_SIGN_IDENTITY:-}" ]; then
    GATE="This package is signed but not notarized. If macOS refuses to open it, allow it under System Settings >
Privacy & Security > \"Open Anyway\"."
else
    GATE="This package is not signed with an Apple Developer ID. If macOS refuses to open it (\"unidentified developer\"):
right-click the .pkg > Open, or allow it under System Settings > Privacy & Security > \"Open Anyway\"."
fi
cp "$ROOT/LICENSE.md" "$WORK/dmg/LICENSE.md"
# the user manual (docs/manual, built by tools/manual_build.py; the PDF is committed so a release needs no Chrome)
MANUAL="$ROOT/docs/manual/NovaInsert-User-Manual.pdf"
[ -f "$MANUAL" ] || { echo "missing $MANUAL — run: python3 -I tools/manual_build.py docs/manual/NovaInsert-User-Manual.md docs/manual/NovaInsert-User-Manual.html $MANUAL" >&2; exit 2; }
cp "$MANUAL" "$WORK/dmg/NovaInsert User Manual.pdf"
cat > "$WORK/dmg/READ ME FIRST.txt" <<TXT
NovaInsert $VERSION — REAPER-only VST3 hardware insert plug-in for macOS (Apple silicon)

1. Double-click "Install NovaInsert $VERSION.pkg" and follow the steps (you must accept the license).
2. In REAPER: Options > Preferences > Plug-ins > VST > Re-scan, then add "VST3: NovaInsert" to a track.
3. Read "NovaInsert User Manual.pdf": create your device presets on the editor's DEVICES page before first use (manual chapter 4).

The plug-in is free to use and comes AS IS, WITHOUT ANY WARRANTY (see LICENSE.md). Start at low monitoring
levels; never calibrate into a monitoring path.

$GATE
TXT
DMG="$OUT/NovaInsert-$VERSION.dmg"; rm -f "$DMG"
hdiutil create -quiet -volname "NovaInsert $VERSION" -srcfolder "$WORK/dmg" -ov -format UDZO "$DMG"
if [ -n "${NI_SIGN_IDENTITY:-}" ] && [ "$NI_SIGN_IDENTITY" != "-" ]; then   # the container too (Apple: sign, then notarise the DMG)
    codesign --timestamp --sign "$NI_SIGN_IDENTITY" "$DMG"
fi
if [ -n "${NI_NOTARY_PROFILE:-}" ]; then
    notarize "$DMG"
    spctl --assess --type install --verbose=1 "$PRODUCT" 2>&1 | tail -n 1   # expect "accepted  source=Notarized Developer ID"
fi
shasum -a 256 "$DMG" | tee "$DMG.sha256"
echo "installer: $DMG"
