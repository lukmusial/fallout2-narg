#!/usr/bin/env python3
"""Play timings of the runtime upscale with no HD packs (Phase 2.23): per
build, game install and save, the map load (with the scene preparation and
its loading screen), frame times during the first ~30 s of walking, running
and scrolling, entering combat, zoomed out to 0.5x, a map transition (and
its loading screen), the world map; disk cache cold and warm; peak memory.
Hidden runs (tools/dev/hd_run.py, tests/scripts/hd_perf_play.txt).

  runtime_perf_play.py --build after=out/build/macos-release [--build before=<dir>]
                       [--game vanilla] [--game rpu=<installed game>] [--warm] [--only <run>]
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUN = ROOT / "out/build/macos/hdrun"

# Settings pinned for comparable runs: no pack installed (hd_pack names the
# scripted pack, which is absent: the runtime upscale in both builds), the
# HD effects off, a 1280x960 window at scale 2, the map view (zoom).
OVERRIDES = ["[graphics]hd=1", "[graphics]hd_pack=scripted", "[graphics]hd_text=1", "[graphics]zoom=1",
             "[graphics]hd_lights=0", "[graphics]hd_wind=0", "[graphics]hd_shot_light=0",
             "[graphics]hd_ground_variants=0", "[graphics]hd_cache_mb=512", "[graphics]hd_upscale_cache=1",
             "[screen]resolution_x=1280", "[screen]resolution_y=960", "[screen]scale=2", "[screen]windowed=1",
             "[ui]skip_opening_movies=2"]

# (save slot, town, next map) per game
VANILLA = [(3, "arroyo", "KLADWTWN"), (1, "klamath", "DENBUS1"), (8, "den", "ARVILLAG")]
RPU = [(3, "arroyo", "KLADWTWN"), (1, "klamath", "DENBUS1"), (8, "den", "ARVILLAG"), (1, "klamath>abbey", "ABBEY")]


def run(name, build, game, slot, next_map, keep):
    cmd = [sys.executable, str(ROOT / "tools/dev/hd_run.py"), name, "hd_perf_play.txt", "--slot", str(slot),
           "--packs", "", "--app", str(build), "--set", f"NEXT_MAP={next_map}", "--timeout", "400", "--rss"]
    if game:
        cmd += ["--game", str(game)]
    if keep:
        cmd.append("--keep-cache")
    subprocess.run(cmd + OVERRIDES, check=True, stdout=subprocess.DEVNULL)
    return RUN / name / "devout"


def load(out, name):
    path = out / f"{name}.json"
    return json.loads(path.read_text()) if path.exists() else {}


def summary(out):
    log = (out / "debug.log").read_text(errors="replace")
    loads = [int(m) for m in re.findall(r"CE: map \S+ loaded in (\d+) ms", log)]
    rss = (out / "rss.txt").read_text().strip() if (out / "rss.txt").exists() else ""
    row = {"map_load_ms": loads[0] if loads else None, "next_map_load_ms": loads[-1] if len(loads) > 1 else None,
           "rss_mb": round(int(rss) / 1048576) if rss else None}
    for phase in ("play", "combat", "zoom", "newtown", "worldmap"):
        times = load(out, phase)
        row[f"{phase}_p99"] = round(times.get("p99_ms", 0), 1)
        row[f"{phase}_max"] = round(times.get("max_ms", 0), 1)
    first = load(out, "load")
    end = load(out, "end_stats")
    row["scene_ms"] = round(first.get("scene_collect_ms", 0) + first.get("scene_wait_ms", 0)) if "scene_wait_ms" in first else None
    row["scene_frames"] = first.get("scene_frames")
    row["drawn_upscales_play"] = end.get("draw_upscales_since_scene", end.get("runtime_inline"))
    row["hd_cache_mb"] = round(end.get("bytes_cached", 0) / 1048576)
    row["disk_hits"] = end.get("upscale_disk_hits")
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", action="append", required=True, help="label=build dir (with the .app)")
    ap.add_argument("--game", action="append", default=[], help="vanilla, or label=installed game dir")
    ap.add_argument("--warm", action="store_true", help="also a second run with the disk cache of the first")
    ap.add_argument("--only", help="only runs whose name contains this")
    args = ap.parse_args()
    games = []
    for entry in args.game or ["vanilla"]:
        if entry == "vanilla":
            games.append(("vanilla", None, VANILLA))
        else:
            label, path = entry.split("=", 1)
            games.append((label, Path(path), RPU))
    rows = []
    for build in args.build:
        label, path = build.split("=", 1)
        for game_label, game, saves in games:
            for slot, town, next_map in saves:
                name = f"play_{label}_{game_label}_{town.replace('>', '_')}"
                if args.only and args.only not in name:
                    continue
                passes = ["cold", "warm"] if args.warm and label != "before" else ["cold"]
                for index, cache in enumerate(passes):
                    out = run(name, Path(path).resolve(), game, slot, next_map, keep=index > 0)
                    row = {"build": label, "game": game_label, "town": town, "cache": cache, **summary(out)}
                    rows.append(row)
                    print(json.dumps(row), flush=True)
    keys = list(rows[0].keys())
    print("| " + " | ".join(keys) + " |")
    print("|" + "---|" * len(keys))
    for row in rows:
        print("| " + " | ".join(str(row[k]) for k in keys) + " |")


if __name__ == "__main__":
    main()
