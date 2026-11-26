#!/usr/bin/env python3
"""Review sheets for the no-pack runtime upscale (Phase 2.23), local only:
docs/screens/phase2.23_runtime_perf/. Made from hidden runs:

- splash.png: the start-up splash, upscaled at runtime (palette boost);
- loading.png: the loading screen of a map transition;
- worldmap.png, townmap.png: the world map and a town map, no pack;
- seams.png: floor tiles upscaled as copies of themselves (lattice
  self-wrap) next to tiles upscaled with their real neighbours on the map,
  enlarged crops of the same spots;
- rpu_<town>.png: the RPU install, no pack.

  runtime_perf_screens.py [--app <build dir>] [--rpu <installed game>]
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
RUN = ROOT / "out/build/macos/hdrun"
OUT = ROOT / "docs/screens/phase2.23_runtime_perf"
BASE = ["[graphics]hd=1", "[graphics]hd_pack=", "[graphics]hd_text=1", "[graphics]hd_ground_variants=0",
        "[graphics]hd_loading_screen_min_ms=0"]


def hd_run(name, script, *extra, app=None, game=None, slot=None, sets=()):
    cmd = [sys.executable, str(ROOT / "tools/dev/hd_run.py"), name, script, "--packs", ""]
    if app:
        cmd += ["--app", str(app)]
    if game:
        cmd += ["--game", str(game)]
    if slot is not None:
        cmd += ["--slot", str(slot)]
    for item in sets:
        cmd += ["--set", item]
    subprocess.run(cmd + BASE + list(extra), check=True, stdout=subprocess.DEVNULL)
    return RUN / name / "devout"


def labelled(images, labels, scale=1):
    images = [im.resize((im.width * scale, im.height * scale), Image.NEAREST) if scale != 1 else im for im in images]
    width = sum(im.width for im in images) + 6 * (len(images) - 1)
    height = max(im.height for im in images) + 22
    sheet = Image.new("RGB", (width, height), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    x = 0
    for image, label in zip(images, labels):
        draw.text((x + 4, 4), label, fill=(230, 230, 230))
        sheet.paste(image, (x, 22))
        x += image.width + 6
    return sheet


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--app", help="build folder with the .app (default: this tree's Debug build)")
    ap.add_argument("--rpu", help="installed game with the Restoration Project")
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)

    splash = hd_run("screens_splash", "smoke_menu.txt", "[ui]skip_opening_movies=1", app=args.app)
    shutil.copyfile(splash / "splash.png", OUT / "splash.png")

    # Klamath: loading screen of the transition to the Den, world map, town map.
    play = hd_run("screens_play", "hd_perf_play.txt", "[graphics]zoom=1", app=args.app, slot=1, sets=["NEXT_MAP=DENBUS1", "LOADING_CMD=loadingshot"])
    for name in ("transition_loading", "worldmap", "townmap", "newtown", "play"):
        if (play / f"{name}.png").exists():
            shutil.copyfile(play / f"{name}.png", OUT / f"{name.replace('transition_', '')}.png")

    # Tile seams: the same spot with and without the neighbours.
    crops = [(380, 520, 620, 700), (700, 560, 940, 740)]
    shots = {}
    for neighbours in (0, 1):
        out = hd_run(f"screens_seams{neighbours}", "hd_upscale_map.txt", f"[graphics]hd_upscale_tile_neighbours={neighbours}",
                     app=args.app, slot=3)
        shots[neighbours] = Image.open(out / "game.png").convert("RGB")
    rows = []
    for crop in crops:
        rows.append(labelled([shots[0].crop(crop), shots[1].crop(crop)],
                             ["tiles: copies of themselves (lattice self-wrap)", "tiles: real neighbours on the map"], scale=2))
    sheet = Image.new("RGB", (max(r.width for r in rows), sum(r.height for r in rows) + 6), (24, 24, 24))
    y = 0
    for row in rows:
        sheet.paste(row, (0, y))
        y += row.height + 6
    sheet.save(OUT / "seams.png", optimize=True)

    if args.rpu:
        for slot, town, next_map in ((3, "arroyo", "ABBEY"), (8, "den", "ARVILLAG")):
            out = hd_run(f"screens_rpu_{town}", "hd_perf_play.txt", "[graphics]zoom=1", app=args.app, game=args.rpu, slot=slot,
                         sets=[f"NEXT_MAP={next_map}", "LOADING_CMD=loadingshot"])
            for name in ("play", "transition_loading", "newtown", "worldmap"):
                if (out / f"{name}.png").exists():
                    shutil.copyfile(out / f"{name}.png", OUT / f"rpu_{town}_{name.replace('transition_', '')}.png")
    for path in sorted(OUT.glob("*.png")):
        print(path)


if __name__ == "__main__":
    main()
