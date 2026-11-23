#!/usr/bin/env python3
"""Phase 2.7 proposal sheets: floors and roofs of the ten reference saves as
the engine shows them.

Runs the game unattended (tests/scripts/hd_floors_sheet.txt: hidden window,
no audio) with hd=1 on every save slot. In one tick the script captures the
frame without HD tiles, with the scripted pack and with the AI pack, so
critters and lights are identical in all three. Writes to
docs/screens/phase2.7/:

  slotNN.png                  crops at 1:1 output pixels (1280x960 game,
                              2x): legacy (pixel-doubled) | scripted | ai
  fullframe_<pack>_slotNN.png 3440x1440 frames (scale 2) for FULL_SLOTS,
                              half size

  floors_sheet.py [--out DIR] [--reuse]   # --reuse: keep earlier game runs

Needs a build (cmake --build out/build/macos), the run directory
(tools/dev/setup-run-dir.sh) and both packs with the area tiles installed
(extract.py --areas, enhance_scripted.py, enhance_ai.py, install.py).
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
from pathlib import Path

from PIL import Image

import hdlib
from screens_sheet import load, save, sheet

RUN_DIR = hdlib.BUILD_DIR
EXE = RUN_DIR / "Fallout II Community Edition.app/Contents/MacOS/Fallout II Community Edition"
SCRIPT = hdlib.REPO_ROOT / "tests/scripts/hd_floors_sheet.txt"
WORK = RUN_DIR / "devout/phase2.7"
PACKS = ("scripted", "ai")
WIDE = ["[screen]resolution_x=3440", "[screen]resolution_y=1440", "[screen]scale=2"]
SLOTS = range(1, 11)
FULL_SLOTS = (1, 3, 8)

# Crops per slot in logical pixels (x, y, w, h) of the 640x480 view, shown
# at 1:1 output pixels (2x). Chosen to show floors, roofs and the objects
# standing on them.
CROPS = {
    1: [(100, 40, 300, 150), (280, 180, 300, 150)],   # roof, wall; sidewalk, road
    2: [(150, 50, 300, 150), (300, 190, 300, 150)],   # toxic caves
    3: [(330, 0, 300, 150), (100, 170, 300, 150)],    # tent, village ground
    4: [(60, 10, 300, 150), (300, 150, 300, 150)],    # garden on concrete; roofs
    5: [(20, 150, 300, 150), (300, 30, 300, 150)],    # shed roof; ground, trees
    6: [(100, 10, 300, 150), (250, 200, 300, 150)],   # rat caves
    7: [(0, 170, 300, 150), (180, 10, 300, 150)],     # roof; sheds, ground
    8: [(130, 90, 300, 150), (300, 220, 300, 150)],   # Den: lamp-lit street
    9: [(0, 170, 300, 150), (250, 0, 300, 150)],      # bridge ground; rocks
    10: [(200, 60, 300, 150), (200, 200, 300, 150)],  # toxic caves
}


def run(name: str, slot: int, args: list[str], reuse: bool) -> Path:
    out = WORK / name
    if reuse and (out / "ai.png").exists():
        return out
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    subprocess.run([str(hdlib.REPO_ROOT / "tools/dev/reset-saves.sh")], check=True, stdout=subprocess.DEVNULL)
    subprocess.run([str(EXE), f"--dev-script={SCRIPT}", f"--dev-out={out}", f"--dev-load-game={slot}",
                    "[graphics]hd=1", "[graphics]hd_pack=scripted", *args],
                   cwd=RUN_DIR, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=180)
    if not (out / "ai.png").exists():
        raise SystemExit(f"{name}: the game wrote no screenshots")
    print(f"ran {name}")
    return out


def map_name(slot: int) -> str:
    state = json.loads((hdlib.REPO_ROOT / f"testdata/golden/slot{slot:02d}_state.json").read_text())
    return state["world"]["map_name"].removesuffix(".SAV").lower()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=hdlib.REPO_ROOT / "docs/screens/phase2.7")
    ap.add_argument("--reuse", action="store_true", help="reuse game runs from an earlier invocation")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(hdlib.REPO_ROOT / "tools/dev/reset-saves.sh")], check=True, stdout=subprocess.DEVNULL)

    labels = ["legacy (pixel-doubled)", "scripted", "ai"]
    for slot in SLOTS:
        out = run(f"slot{slot:02d}", slot, [], args.reuse)
        # The legacy frame is the hd=1 frame without HD tiles: pixel-doubled already.
        frames = [load(out / f"{name}.png", double=False) for name in ("legacy", *PACKS)]
        rows = [[f[y * 2:(y + h) * 2, x * 2:(x + w) * 2] for f in frames] for x, y, w, h in CROPS[slot]]
        title = f"Slot {slot}: {map_name(slot)}, floors and roofs (objects stay legacy this phase), 1280x960"
        save(sheet(title, rows, labels), args.out / f"slot{slot:02d}.png")

    for slot in FULL_SLOTS:
        out = run(f"slot{slot:02d}_wide", slot, WIDE, args.reuse)
        for pack in PACKS:
            image = Image.open(out / f"{pack}.png").convert("RGB")
            image = image.resize((image.width // 2, image.height // 2), Image.LANCZOS)
            save(image, args.out / f"fullframe_{pack}_slot{slot:02d}.png")
    subprocess.run([str(hdlib.REPO_ROOT / "tools/dev/reset-saves.sh")], check=True, stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
