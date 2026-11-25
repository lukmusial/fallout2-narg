#!/bin/bash
# Packs a Fallout 2: Narg Edition release archive for one platform.
#
#   extra/package.sh <platform> <engine build dir> <hd packs dir> [<ce-dat-tool>]
#
#   <platform>          macos | linux | windows
#   <engine build dir>  the CMake build of engine/: "Fallout II Community
#                       Edition.app" (macos), fallout2-ce (linux) or
#                       fallout2-ce.exe (windows), and ce.dat
#   <hd packs dir>      the installed HD packs: <dir>/ai and <dir>/scripted, each
#                       with legacy.txt (engine/tools/hd: install.py makes
#                       both), and optionally <dir>/rpu (mod_pack.py install).
#                       Empty ("") packs no HD art: an engine-only archive.
#   <ce-dat-tool>       default: <engine build dir>/ce-dat-tool
#
# Writes narg_v<VERSION>_<platform>.zip (and, with an rpu pack,
# narg_v<VERSION>_rpu.zip) in the repository root, following
# the Restoration Project's layout: everything extracts into the game folder,
# mods/*.dat are the data, mods_order.txt lists the ones to load and
# narg-install.* sets the game up (see docs/).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PLATFORM="$1"
BUILD="$(cd "$2" && pwd)"
PACKS="${3:-}"
DAT_TOOL="${4:-$BUILD/ce-dat-tool}"
VERSION="$(cat "$ROOT/VERSION")"
NAME="narg_v${VERSION}_${PLATFORM}"
STAGE="$ROOT/out/stage/$PLATFORM"
APP="Fallout II Community Edition.app"

rm -rf "$STAGE"
mkdir -p "$STAGE/mods" "$STAGE/narg/docs" "$STAGE/narg/settings"

# installer, load order, settings, docs
cp "$ROOT/release/mods_order.txt" "$STAGE/"
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

# HD art: mods/narg_hd.dat holds the AI pack (hd/ai) and the images of the
# scripted pack that the "mixed" manifest (ce.dat hd/mixed/manifest.txt)
# takes from it, so hd_pack=mixed and hd_pack=ai work. The whole scripted
# pack is the optional component mods/narg_hd_scripted.dat.
#
# The Restoration Project's own art (hd/rpu, engine/tools/hd/mod_pack.py),
# when <hd packs dir>/rpu exists, goes into a separate archive
# narg_v<VERSION>_rpu.zip holding mods/narg_hd_rpu.dat (same for every
# platform); the installer loads it only on RPU, where the mixed manifest
# tries it after ai.
if [[ -n "$PACKS" ]]; then
    for pack in ai scripted; do
        if [[ ! -f "$PACKS/$pack/legacy.txt" ]]; then
            echo "$PACKS/$pack/legacy.txt missing: run engine/tools/hd/legacy_art.py hashes" >&2
            exit 1
        fi
    done
    tree="$ROOT/out/stage/hd"
    rm -rf "$tree"
    mkdir -p "$tree/main/hd" "$tree/scripted/hd"
    # APFS/btrfs clones where possible: no copy of ~600 MB.
    cp -cR "$PACKS/ai" "$tree/main/hd/ai" 2>/dev/null || cp -R "$PACKS/ai" "$tree/main/hd/ai"
    mkdir -p "$tree/main/hd/scripted"
    cp "$PACKS/scripted/legacy.txt" "$tree/main/hd/scripted/"
    manifest="$ROOT/engine/files/ce.dat/hd/mixed/manifest.txt"
    awk -F= '!/^[[:space:]]*#/ && NF == 2 {
            k = $1; v = $2; gsub(/[[:space:]]|#.*/, "", k); sub(/#.*/, "", v)
            n = split(v, packs, /[[:space:],]+/)
            for (i = 1; i <= n; i++) {
                if (k != "*" && packs[i] == "scripted") { sub(/\.[^.\/]*$/, "", k); print k; break }
            }
        }' "$manifest" | while read -r stem; do
        for ext in png fhd; do
            if [[ -f "$PACKS/scripted/$stem.$ext" ]]; then
                mkdir -p "$tree/main/hd/scripted/$(dirname "$stem")"
                cp "$PACKS/scripted/$stem.$ext" "$tree/main/hd/scripted/$stem.$ext"
            fi
        done
    done
    cp -cR "$PACKS/scripted" "$tree/scripted/hd/scripted" 2>/dev/null || cp -R "$PACKS/scripted" "$tree/scripted/hd/scripted"
    "$DAT_TOOL" create "$tree/main" "$STAGE/mods/narg_hd.dat"
    "$DAT_TOOL" create "$tree/scripted" "$STAGE/mods/narg_hd_scripted.dat"
    if [[ -d "$PACKS/rpu" ]]; then
        if [[ ! -f "$PACKS/rpu/legacy.txt" ]]; then
            echo "$PACKS/rpu/legacy.txt missing: run engine/tools/hd/mod_pack.py install" >&2
            exit 1
        fi
        rpu_stage="$ROOT/out/stage/rpu"
        rm -rf "$rpu_stage"
        mkdir -p "$tree/rpu/hd" "$rpu_stage/mods"
        cp -cR "$PACKS/rpu" "$tree/rpu/hd/rpu" 2>/dev/null || cp -R "$PACKS/rpu" "$tree/rpu/hd/rpu"
        "$DAT_TOOL" create "$tree/rpu" "$rpu_stage/mods/narg_hd_rpu.dat"
        rpu_zip="$ROOT/narg_v${VERSION}_rpu.zip"
        rm -f "$rpu_zip"
        (cd "$rpu_stage" && zip -qry "$rpu_zip" -- .)
        echo "packed $rpu_zip"
    fi
    rm -rf "$tree"
else
    echo "warning: no HD packs given; the archive has no HD art" >&2
fi

rm -f "$ROOT/$NAME.zip"
(cd "$STAGE" && zip -qry "$ROOT/$NAME.zip" -- .)
echo "packed $ROOT/$NAME.zip"
