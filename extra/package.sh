#!/bin/bash
# Packs a Fallout 2: Narg Edition release archive for one platform.
#
#   extra/package.sh <platform> <engine build dir>
#
#   <platform>          macos | linux | windows
#   <engine build dir>  the CMake build of engine/: "Fallout II Community
#                       Edition.app" (macos), fallout2-ce (linux) or
#                       fallout2-ce.exe (windows), and ce.dat
#
# Writes narg_v<VERSION>_<platform>.zip in the repository root: everything
# extracts into the game folder and narg-install.* sets the game up (see docs/).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PLATFORM="$1"
BUILD="$(cd "$2" && pwd)"
VERSION="$(cat "$ROOT/VERSION")"
NAME="narg_v${VERSION}_${PLATFORM}"
STAGE="$ROOT/out/stage/$PLATFORM"
APP="Fallout II Community Edition.app"

rm -rf "$STAGE"
mkdir -p "$STAGE/narg/docs" "$STAGE/narg/settings"

# installer, settings, docs
cp "$ROOT/release/narg.cfg" "$STAGE/narg/"
cp "$ROOT/VERSION" "$STAGE/narg/"
cp "$ROOT"/components/settings/*.cfg "$STAGE/narg/settings/"
cp "$ROOT/README.md" "$STAGE/narg/docs/README.md"
cp "$ROOT"/docs/*.md "$STAGE/narg/docs/"
cp "$ROOT/engine/files/f2_res.dat" "$STAGE/narg/f2_res.dat"
cp "$ROOT/release/narg/rpu-save-convert.py" "$STAGE/narg/"
case "$PLATFORM" in
macos)
    cp "$ROOT/release/narg-install.command" "$ROOT/release/narg-install.sh" "$STAGE/"
    # ce.dat is inside the bundle (Contents/Resources).
    cp -R "$BUILD/$APP" "$STAGE/Fallout 2 Narg Edition.app"
    ;;
linux)
    cp "$ROOT/release/narg-install.sh" "$STAGE/"
    mkdir -p "$STAGE/narg/linux"
    cp "$BUILD/fallout2-ce" "$STAGE/narg/linux/"
    cp "$BUILD/ce.dat" "$STAGE/narg/ce.dat"
    ;;
windows)
    cp "$ROOT/release/narg-install.bat" "$STAGE/"
    cp "$ROOT/release/narg/narg-install.ps1" "$STAGE/narg/"
    mkdir -p "$STAGE/narg/windows"
    cp "$BUILD/fallout2-ce.exe" "$STAGE/narg/windows/"
    find "$BUILD" -maxdepth 1 -name '*.dll' -exec cp {} "$STAGE/narg/windows/" \;
    cp "$BUILD/ce.dat" "$STAGE/narg/ce.dat"
    ;;
*)
    echo "platform must be macos, linux or windows" >&2
    exit 1
    ;;
esac

rm -f "$ROOT/$NAME.zip"
(cd "$STAGE" && zip -qry "$ROOT/$NAME.zip" -- .)
echo "packed $ROOT/$NAME.zip"
