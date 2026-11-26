#!/usr/bin/env python3
"""Review sheets for the runtime scripted upscale (Phase 2.22): the same
spots drawn (a) pixel-doubled, (b) upscaled at runtime with the palette boost
(precision 16) and (b8) without it (precision 8, the offline method), (c) the
offline scripted pack, (d) the AI pack; crops at 1:1 HD pixels or enlarged.
Hidden runs (tools/dev/hd_run.py); sheets go to
docs/screens/phase2.22_runtime_scripted/ (local review material).

  runtime_upscale_screens.py [--skip-runs]
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
RUN = ROOT / "out/build/macos/hdrun"
OUT = ROOT / "docs/screens/phase2.22_runtime_scripted"
HD = ["[graphics]hd=1", "[graphics]hd_text=1", "[graphics]hd_ground_variants=0"]

# column: (label, packs linked, overrides). Critters: only the runtime
# columns upscale them (no pack has critter images); elsewhere they are
# doubled.
CONFIGS = {
    "double": ("(a) legacy doubled", "", ["[graphics]hd_pack=none", "[graphics]hd_upscale=double"]),
    "runtime16": ("(b) runtime, palette boost", "ai,rpu", ["[graphics]hd_pack=scripted"]),
    "runtime8": ("(b8) runtime, 8-bit", "ai,rpu", ["[graphics]hd_pack=scripted", "[graphics]hd_upscale_precision=8"]),
    "offline": ("(c) offline scripted pack", "scripted", ["[graphics]hd_pack=scripted", "[graphics]hd_upscale=double"]),
    "ai": ("(d) AI pack", "ai", ["[graphics]hd_pack=ai", "[graphics]hd_upscale=double"]),
}
COLUMNS = ["double", "runtime16", "runtime8", "offline", "ai"]

SLOTS = {3: "arroyo", 1: "klamath", 8: "den"}

# sheet: [(source capture, crop box in HD pixels (1280x960), enlargement)]
SHEETS = {
    "arroyo": [("slot3/game", (400, 200, 880, 560), 1), ("slot3/game", (580, 330, 740, 490), 2)],
    "klamath": [("slot1/game", (400, 200, 880, 560), 1), ("slot1/game", (40, 40, 520, 400), 1)],
    "den": [("slot8/game", (400, 200, 880, 560), 1), ("slot8/game", (760, 60, 1240, 420), 1)],
    "main_menu": [("menu/menu", (600, 60, 1240, 540), 1), ("menu/load", (660, 60, 1240, 420), 1)],
    "inventory_character": [("slot1/inventory", (160, 40, 1120, 560), 1), ("slot1/character", (0, 40, 560, 600), 1)],
    "critters": [("slot3/game", (360, 440, 480, 620), 2), ("slot3/game", (100, 220, 220, 400), 2),
                 ("slot1/game", (580, 600, 700, 780), 2), ("slot8/game", (580, 220, 700, 400), 2)],
    # The palette boost: smooth shading of the menu's power armour and the
    # load screen's frame, enlarged; (b8)/(c) keep the 256-colour bands.
    "banding": [("menu/menu", (720, 140, 900, 290), 3), ("menu/menu", (1000, 380, 1180, 530), 3)],
}


def capture(column: str, name: str, script: str, slot: int | None) -> Path:
    label, packs, overrides = CONFIGS[column]
    run_name = f"review_{column}_{name}"
    cmd = [sys.executable, str(ROOT / "tools/dev/hd_run.py"), run_name, script, "--packs", packs, *HD, *overrides]
    if slot is not None:
        cmd += ["--slot", str(slot)]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    return RUN / run_name / "devout"


def source(column: str, ref: str) -> Path:
    where, shot = ref.split("/")
    return RUN / f"review_{column}_{where}" / "devout" / f"{shot}.png"


def sheet(name: str, crops: list) -> Path:
    tiles = []
    for ref, (left, top, right, bottom), scale in crops:
        row = []
        for column in COLUMNS:
            image = Image.open(source(column, ref)).convert("RGB").crop((left, top, right, bottom))
            if scale != 1:
                image = image.resize((image.width * scale, image.height * scale), Image.NEAREST)
            row.append(image)
        tiles.append(row)
    label_h = 22
    gap = 6
    width = sum(t.width for t in tiles[0]) + gap * (len(COLUMNS) - 1)
    height = label_h + sum(row[0].height for row in tiles) + gap * (len(tiles) - 1)
    out = Image.new("RGB", (width, height), (24, 24, 24))
    draw = ImageDraw.Draw(out)
    x = 0
    for index, column in enumerate(COLUMNS):
        draw.text((x + 4, 4), CONFIGS[column][0], fill=(230, 230, 230))
        x += tiles[0][index].width + gap
    y = label_h
    for row in tiles:
        x = 0
        for image in row:
            out.paste(image, (x, y))
            x += image.width + gap
        y += row[0].height + gap
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / f"{name}.png"
    out.save(path, optimize=True)
    return path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--skip-runs", action="store_true", help="only rebuild the sheets from earlier captures")
    ap.add_argument("--columns", default=",".join(COLUMNS), help="columns to capture again")
    args = ap.parse_args()
    if not args.skip_runs:
        for column in args.columns.split(","):
            capture(column, "menu", "hd_upscale_review_menu.txt", None)
            for slot in SLOTS:
                capture(column, f"slot{slot}", "hd_upscale_review.txt", slot)
            print(f"captured {column}", flush=True)
    for name, crops in SHEETS.items():
        print(sheet(name, crops))
    return 0


if __name__ == "__main__":
    sys.exit(main())
