#!/bin/bash
# Restore the reference save slots (testdata/SAVEGAME) into the run directory,
# discarding any slots and working map/proto files written by earlier runs.

set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
RUN_DIR="${RUN_DIR:-$REPO_DIR/out/build/macos}"
SRC="$REPO_DIR/testdata/SAVEGAME"
DST="$RUN_DIR/data/SAVEGAME"

if [[ ! -d "$SRC" ]]; then
    echo "error: $SRC missing" >&2
    exit 1
fi

rm -rf "$DST" "$RUN_DIR/data/maps" "$RUN_DIR/data/MAPS" "$RUN_DIR/data/proto" "$RUN_DIR/data/worldmap.dat"
mkdir -p "$RUN_DIR/data"
cp -R "$SRC" "$DST"
echo "restored $(ls "$DST" | wc -l | tr -d ' ') save slots into $DST"
