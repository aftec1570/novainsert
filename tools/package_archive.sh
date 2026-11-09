#!/bin/bash
# Release archive for Windows x64 (zip) or Linux x86_64 (tar.gz): NovaInsert.vst3/, LICENSE.txt, BUGS.md, VERSION.md,
# the platform README (+ install.sh on Linux), and <archive>.sha256 next to it.
# Usage: tools/package_archive.sh <build-dir> <out-dir>   (version + NI_PLATFORM_TAG from <build-dir>/CMakeCache.txt)
# Runs under bash on macOS, Linux and Git Bash (windows-latest): zip falls back to python's zipfile, shasum to sha256sum.
set -euo pipefail
[ $# -eq 2 ] || { echo "usage: $0 <build-dir> <out-dir>" >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; BUILD="$1"; OUT="$2"
cache() { sed -n "s/^$1:[A-Z]*=//p" "$BUILD/CMakeCache.txt" | head -1; }
VER="$(cache CMAKE_PROJECT_VERSION)"; TAG="$(cache NI_PLATFORM_TAG)"
[ -n "$VER" ] || { echo "no CMAKE_PROJECT_VERSION in $BUILD/CMakeCache.txt" >&2; exit 1; }
[ -n "$TAG" ] || { echo "no NI_PLATFORM_TAG in $BUILD/CMakeCache.txt" >&2; exit 1; }
BUNDLE="$BUILD/NovaInsert.vst3"
[ -d "$BUNDLE" ] || BUNDLE="$BUILD/src/NovaInsert_artefacts/Release/VST3/NovaInsert.vst3"
[ -d "$BUNDLE" ] || { echo "no NovaInsert.vst3 in $BUILD" >&2; exit 1; }
case "$TAG" in
  Linux-*)   README=README-Linux.txt;   EXT=tar.gz ;;
  Windows-*) README=README-Windows.txt; EXT=zip ;;
  *) echo "unsupported NI_PLATFORM_TAG $TAG (Linux-*/Windows-*; macOS ships as a DMG via tools/make_installer.sh)" >&2; exit 1 ;;
esac
STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
cp -R "$BUNDLE" "$STAGE/NovaInsert.vst3"
cp "$ROOT/LICENSE.md" "$STAGE/LICENSE.txt"; cp "$ROOT/BUGS.md" "$ROOT/VERSION.md" "$STAGE/"
cp "$ROOT/tools/installer/$README" "$STAGE/"
MEMBERS=(NovaInsert.vst3 "$README" LICENSE.txt BUGS.md VERSION.md)
if [ "$EXT" = tar.gz ]; then cp "$ROOT/tools/installer/install.sh" "$STAGE/"; chmod 755 "$STAGE/install.sh"; MEMBERS+=(install.sh); fi
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"; NAME="NovaInsert-$VER-$TAG.$EXT"; rm -f "$OUT/$NAME"
if [ "$EXT" = tar.gz ]; then
  COPYFILE_DISABLE=1 tar -C "$STAGE" -czf "$OUT/$NAME" "${MEMBERS[@]}"   # COPYFILE_DISABLE: no ._ AppleDouble files from macOS tar
elif command -v zip >/dev/null; then
  (cd "$STAGE" && zip -qr "$OUT/$NAME" "${MEMBERS[@]}")
else
  (cd "$STAGE" && python3 -m zipfile -c "$OUT/$NAME" "${MEMBERS[@]}")
fi
if command -v shasum >/dev/null; then SUM="$(cd "$OUT" && shasum -a 256 "$NAME")"; else SUM="$(cd "$OUT" && sha256sum "$NAME")"; fi
echo "$SUM" | sed 's/ \*/  /' > "$OUT/$NAME.sha256"
echo "$OUT/$NAME"
