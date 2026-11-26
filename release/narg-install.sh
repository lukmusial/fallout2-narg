#!/bin/bash
# Fallout 2: Narg Edition installer (Linux / macOS).
#
# Extract the release archive into the Fallout 2 folder (vanilla, or with the
# Restoration Project installed), then run this script there:
#
#   ./narg-install.sh [--with <component>] [--resolution WxH] ...
#
#   --with <component>  apply narg/settings/<component>.cfg after the defaults
#                       (no_wind, no_shot_light, no_lights, classic, windowed,
#                       pixel_doubled)
#
# Whatever the installer replaces or changes is saved to backup/narg first
# (only the first time: a re-install keeps the original backup).
#
# Uses only POSIX tools (sed, awk, grep): BSD grep on macOS mishandles some
# GNU-style patterns, so none are used.

set -eu

cd -- "$(dirname "$0")"

COMPONENTS=()
RESOLUTION=""
while [[ $# -gt 0 ]]; do
    case "$1" in
    --with)
        COMPONENTS+=("$2")
        shift 2
        ;;
    --resolution)
        RESOLUTION="$2"
        shift 2
        ;;
    *)
        echo "unknown option: $1 (see the top of $0)"
        exit 1
        ;;
    esac
done

if [[ ! -f master.dat || ! -f critter.dat ]]; then
    echo "master.dat / critter.dat not found. This is not a Fallout 2 directory (or its file names are not lower case). Aborting."
    exit 1
fi
if [[ ! -d narg || ! -f narg/narg.cfg ]]; then
    echo "narg/ not found: extract the whole Narg Edition archive into this directory first. Aborting."
    exit 1
fi
for c in ${COMPONENTS[@]+"${COMPONENTS[@]}"}; do
    if [[ ! -f "narg/settings/$c.cfg" ]]; then
        echo "unknown component $c (see narg/settings/). Aborting."
        exit 1
    fi
done

# Case-sensitive file systems: the engine looks files up in lower case.
touch fs_testx fs_testX
if [[ "$(find . -maxdepth 1 -name 'fs_test*' | wc -l | tr -d ' ')" == "2" ]]; then
    echo "note: the file system is case sensitive; Fallout 2's own files must have lower-case names."
    rm -f fs_testx fs_testX
else
    rm -f fs_testx
fi

game="vanilla Fallout 2"
rpu=0
if [[ -f mods/rpu.dat ]]; then
    game="Fallout 2 with the Restoration Project"
    rpu=1
elif [[ -f mods/upu.dat ]]; then
    game="Fallout 2 with the Unofficial Patch"
fi
echo "Installing Fallout 2: Narg Edition $(cat narg/VERSION 2>/dev/null || echo '') on $game."

bdir="backup/narg"
mkdir -p "$bdir"

# backup <path>: copy a file or folder into the backup once.
backup() {
    if [[ -e "$1" && ! -e "$bdir/$1" ]]; then
        mkdir -p "$bdir/$(dirname "$1")"
        cp -R "$1" "$bdir/$1"
    fi
}

# --- the engine ------------------------------------------------------------
case "$(uname -s)" in
Darwin)
    # "Fallout 2 Narg Edition.app" was extracted next to the game files; it
    # reads the game from the folder it is in (ce.dat is inside the bundle).
    if [[ ! -d "Fallout 2 Narg Edition.app" ]]; then
        echo "Fallout 2 Narg Edition.app not found: this archive is not the macOS one. Aborting."
        exit 1
    fi
    xattr -dr com.apple.quarantine "Fallout 2 Narg Edition.app" 2>/dev/null || true
    launcher="Fallout 2 Narg Edition.app"
    ;;
*)
    if [[ ! -f narg/linux/fallout2-ce ]]; then
        echo "narg/linux/fallout2-ce not found: this archive is not the Linux one. Aborting."
        exit 1
    fi
    backup fallout2-ce
    backup ce.dat
    cp narg/linux/fallout2-ce fallout2-ce
    chmod +x fallout2-ce
    cp narg/ce.dat ce.dat
    launcher="./fallout2-ce"
    ;;
esac

# --- hi-res patch data -----------------------------------------------------
# The engine's main menu, dialogue and interface bar art for large screens
# (f2_res.dat). The Restoration Project ships its own (mods/f2_res.dat, set in
# f2_res.ini); keep it. Otherwise install the one the HD art was made from.
cfg_value() { # cfg_value <file> <section> <key>
    [[ -f "$1" ]] || return 0
    NARG_S="$2" NARG_K="$3" awk '
        BEGIN { s = ENVIRON["NARG_S"]; k = ENVIRON["NARG_K"] }
        { line = $0; sub(/[;#].*/, "", line); gsub(/^[ \t]+|[ \t\r]+$/, "", line) }
        line ~ /^\[.*\]$/ { cur = tolower(substr(line, 2, length(line) - 2)); next }
        cur == tolower(s) && index(line, "=") > 0 {
            key = substr(line, 1, index(line, "=") - 1); gsub(/[ \t]+$/, "", key)
            if (tolower(key) == tolower(k)) { v = substr(line, index(line, "=") + 1); gsub(/^[ \t]+/, "", v); print v; exit }
        }' "$1"
}
f2res="$(cfg_value fallout2.cfg system f2_res_dat)"
[[ -n "$f2res" ]] || f2res="$(cfg_value f2_res.ini main f2_res_dat)"
if [[ -n "$f2res" ]]; then
    echo "Keeping the hi-res data the game is set up with: $f2res"
elif [[ ! -f f2_res.dat ]]; then
    cp narg/f2_res.dat f2_res.dat
    f2res="f2_res.dat"
else
    f2res="f2_res.dat"
fi

# --- settings --------------------------------------------------------------
detect_resolution() {
    local r=""
    case "$(uname -s)" in
    Darwin)
        r="$(system_profiler SPDisplaysDataType 2>/dev/null | awk '/Resolution:/ { print $2 "x" $4; exit }')"
        ;;
    *)
        if command -v xrandr >/dev/null 2>&1; then
            r="$(xrandr 2>/dev/null | awk '/\*/ { print $1; exit }')"
        fi
        ;;
    esac
    echo "$r"
}
if [[ -z "$RESOLUTION" ]]; then
    RESOLUTION="$(detect_resolution)"
fi
width="${RESOLUTION%%x*}"
height="${RESOLUTION##*x}"
if ! [[ "$width" =~ ^[0-9]+$ && "$height" =~ ^[0-9]+$ ]]; then
    width=1920
    height=1080
fi
# HD draws at twice the logical size, which must be at least 640x480.
if ((width < 1280)); then width=1280; fi
if ((height < 960)); then height=960; fi

backup fallout2.cfg
touch fallout2.cfg

# set_cfg <section> <key> <value>: set or add a key of fallout2.cfg, keeping
# everything else (comments, other keys, line endings).
set_cfg() {
    # Values go through the environment: awk -v would turn backslashes (as in
    # mods\f2_res.dat) into escapes.
    NARG_S="$1" NARG_K="$2" NARG_V="$3" awk '
        function flush() {
            if (insec && !done) { print k "=" v eol; done = 1 }
            for (i = 0; i < blanks; i++) print ""
            blanks = 0
        }
        BEGIN { s = ENVIRON["NARG_S"]; k = ENVIRON["NARG_K"]; v = ENVIRON["NARG_V"]; eol = ""; insec = 0; done = 0; seen = 0; blanks = 0 }
        {
            if (NR == 1 && $0 ~ /\r$/) eol = "\r"
            line = $0; sub(/\r$/, "", line)
            t = line; gsub(/^[ \t]+|[ \t]+$/, "", t)
            if (t == "") { blanks++; next }
            if (t ~ /^\[.*\]$/) {
                flush()
                insec = (tolower(substr(t, 2, length(t) - 2)) == tolower(s))
                if (insec) seen = 1
                print $0; next
            }
            for (i = 0; i < blanks; i++) print ""
            blanks = 0
            if (insec && !done && index(t, "=") > 0) {
                key = substr(t, 1, index(t, "=") - 1); gsub(/[ \t]+$/, "", key)
                if (tolower(key) == tolower(k)) { print k "=" v eol; done = 1; next }
            }
            print $0
        }
        END {
            flush()
            if (!seen) { print ""; print "[" s "]" eol; print k "=" v eol }
        }' fallout2.cfg >fallout2.cfg.narg
    mv fallout2.cfg.narg fallout2.cfg
}

# apply_cfg <fragment>: every key of a settings fragment
apply_cfg() {
    local section="" line key value
    while IFS= read -r line || [[ -n "$line" ]]; do
        line="${line%$'\r'}"
        line="${line%%;*}"
        line="$(echo "$line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
        [[ -z "$line" ]] && continue
        if [[ "$line" == \[*\] ]]; then
            section="${line:1:${#line}-2}"
            continue
        fi
        [[ "$line" == *=* && -n "$section" ]] || continue
        key="${line%%=*}"
        value="${line#*=}"
        case "$key" in
        resolution_x) [[ "$value" == auto ]] && value="$width" ;;
        resolution_y) [[ "$value" == auto ]] && value="$height" ;;
        esac
        set_cfg "$section" "$key" "$value"
    done <"$1"
}

apply_cfg narg/narg.cfg
# Writing [screen] stops the engine's one-time import of f2_res.ini, so carry
# over the hi-res data path it would have imported.
set_cfg system f2_res_dat "$f2res"
for c in ${COMPONENTS[@]+"${COMPONENTS[@]}"}; do
    apply_cfg "narg/settings/$c.cfg"
    echo "Applied $c."
done

rm -f -- narg-install.bat narg-install.command narg-install.sh narg/narg-install.ps1

echo "Fallout 2: Narg Edition installed (HD upscaled by the engine, screen ${width}x${height}). Backup is in $bdir."
echo "Start the game with: $launcher"
if [[ $rpu -eq 1 ]]; then
    echo "Restoration Project: start a new game, or convert your old saves with narg/rpu-save-convert.py (docs: narg/docs/rpu.md)."
fi
