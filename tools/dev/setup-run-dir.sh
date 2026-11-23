#!/bin/bash
# Prepare the directory the macOS build runs from (the folder that contains the .app;
# see SDL_FILESYSTEM_BASE_DIR_TYPE=parent in os/macos/Info.plist).
#
# Game data is linked read-only from an existing Fallout 2 installation. Nothing in that
# installation is modified: the game writes maps, protos and saves into RUN_DIR/data.
#
# Usage: tools/dev/setup-run-dir.sh [--force-config]
#   FALLOUT2_DATA_DIR  installed game folder (default: ~/Games/Fallout 2)
#   RUN_DIR            build output folder   (default: out/build/macos)

set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
GAME_DIR="${FALLOUT2_DATA_DIR:-$HOME/Games/Fallout 2}"
RUN_DIR="${RUN_DIR:-$REPO_DIR/out/build/macos}"
FORCE_CONFIG=0
[[ "${1:-}" == "--force-config" ]] && FORCE_CONFIG=1

for f in master.dat critter.dat patch000.dat; do
    if [[ ! -f "$GAME_DIR/$f" ]]; then
        echo "error: $GAME_DIR/$f not found (set FALLOUT2_DATA_DIR)" >&2
        exit 1
    fi
done
mkdir -p "$RUN_DIR"

link() { ln -sfn "$1" "$RUN_DIR/$2"; }
link "$GAME_DIR/master.dat" master.dat
link "$GAME_DIR/critter.dat" critter.dat
link "$GAME_DIR/patch000.dat" patch000.dat
link "$GAME_DIR/sound" sound
link "$REPO_DIR/files/f2_res.dat" f2_res.dat

mkdir -p "$RUN_DIR/data"

CFG="$RUN_DIR/fallout2.cfg"
if [[ ! -f "$CFG" || $FORCE_CONFIG -eq 1 ]]; then
    # Start from the repository's example config and pin the values the tests rely on:
    # 1280x960 window rendered at 640x480 logical, no opening movies or splash.
    sed -e 's/^resolution_x=.*/resolution_x=1280/' \
        -e 's/^resolution_y=.*/resolution_y=960/' \
        -e 's/^scale=.*/scale=2/' \
        -e 's/^windowed=.*/windowed=1/' \
        -e 's/^skip_opening_movies=.*/skip_opening_movies=2/' \
        -e 's/^show_script_messages=.*/show_script_messages=0/' \
        -e 's/^show_tile_num=.*/show_tile_num=0/' \
        -e 's/^output_map_data_info=.*/output_map_data_info=0/' \
        -e 's/^hd=.*/hd=0/' \
        -e 's/^hd_pack=.*/hd_pack=/' \
        -e 's/^hd_lights=.*/hd_lights=0/' \
        -e 's/^smooth_scroll=.*/smooth_scroll=0/' \
        -e 's/^zoom=.*/zoom=0/' \
        "$REPO_DIR/files/fallout2.cfg" >"$CFG"
    echo "wrote $CFG"
fi

"$REPO_DIR/tools/dev/reset-saves.sh"
echo "run directory ready: $RUN_DIR"
