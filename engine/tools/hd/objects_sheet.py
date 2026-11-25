#!/usr/bin/env python3
"""Phase 2.8 proposal sheets: walls, scenery and items (on the HD floors and
roofs of Phase 2.7) of the ten reference saves as the engine shows them.

Runs the game unattended (tests/scripts/hd_objects_sheet.txt: hidden
window, no audio) with hd=1 on every save slot. In one tick the script
captures the frame without HD art, with the scripted, the AI and the mixed
pack, so critters and lights are identical in all four. Writes to
docs/screens/phase2.8/:

  slotNN.png                  crops at 1:1 output pixels (1280x960 game,
                              2x): legacy (pixel-doubled) | scripted | ai | mixed
  fullframe_mixed_slotNN.png  3440x1440 frames (scale 2) for FULL_SLOTS with
                              the mixed pack, half size

  objects_sheet.py [--out DIR] [--reuse]   # --reuse: keep earlier game runs

Needs a build (cmake --build out/build/macos), the run directory
(tools/dev/setup-run-dir.sh) and both packs with the area art installed
(extract.py --areas --objects, enhance_scripted.py, enhance_ai.py,
install.py).
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path

from PIL import Image

import hdlib
from floors_sheet import map_name
from screens_sheet import load, save, sheet

RUN_DIR = hdlib.BUILD_DIR
EXE = RUN_DIR / "Fallout II Community Edition.app/Contents/MacOS/Fallout II Community Edition"
SCRIPT = hdlib.REPO_ROOT / "tests/scripts/hd_objects_sheet.txt"
WORK = RUN_DIR / "devout/phase2.8"
PACKS = ("scripted", "ai", "mixed")
WIDE = ["[screen]resolution_x=3440", "[screen]resolution_y=1440", "[screen]scale=2"]
SLOTS = range(1, 11)
FULL_SLOTS = (1, 3, 6, 8)

# Crops per slot in logical pixels (x, y, w, h) of the 640x480 view, shown
# at 1:1 output pixels (2x). Chosen to show walls, scenery and items.
CROPS = {
    1: [(130, 60, 240, 150), (330, 170, 240, 150)],   # brick wall, windows; hydrant, sign, chest
    2: [(80, 30, 240, 150), (380, 20, 240, 150)],    # caves: walls, critters; barrels
    3: [(330, 0, 240, 150), (0, 130, 240, 150)],     # tent; stone head, totem
    4: [(60, 10, 240, 150), (0, 150, 240, 150)],     # garden; walls
    5: [(20, 150, 240, 150), (330, 30, 240, 150)],    # shed; trees, rocks
    6: [(100, 10, 240, 150), (250, 200, 240, 150)],   # rat caves
    7: [(0, 170, 240, 150), (200, 10, 240, 150)],     # sheds; fences
    8: [(360, 90, 240, 150), (0, 120, 240, 150)],    # Den at night: barrel, sign; fence
    9: [(0, 170, 240, 150), (250, 0, 240, 150)],      # bridge; rocks
    10: [(200, 60, 240, 150), (200, 200, 240, 150)],  # toxic caves
}


def run(name: str, slot: int, args: list[str], reuse: bool) -> Path:
    out = WORK / name
    if reuse and (out / "mixed.png").exists():
        return out
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    subprocess.run([str(hdlib.REPO_ROOT / "tools/dev/reset-saves.sh")], check=True, stdout=subprocess.DEVNULL)
    subprocess.run([str(EXE), f"--dev-script={SCRIPT}", f"--dev-out={out}", f"--dev-load-game={slot}",
                    "--dev-fast", "[graphics]hd=1", "[graphics]hd_pack=mixed", *args],
                   cwd=RUN_DIR, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=180)
    if not (out / "mixed.png").exists():
        raise SystemExit(f"{name}: the game wrote no screenshots")
    print(f"ran {name}")
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=hdlib.REPO_ROOT / "docs/screens/phase2.8")
    ap.add_argument("--reuse", action="store_true", help="reuse game runs from an earlier invocation")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    labels = ["legacy (pixel-doubled)", "scripted", "ai", "mixed"]
    for slot in SLOTS:
        out = run(f"slot{slot:02d}", slot, [], args.reuse)
        # The legacy frame is the hd=1 frame without HD art: pixel-doubled already.
        frames = [load(out / f"{name}.png", double=False) for name in ("legacy", *PACKS)]
        rows = [[f[y * 2:(y + h) * 2, x * 2:(x + w) * 2] for f in frames] for x, y, w, h in CROPS[slot]]
        title = (f"Slot {slot}: {map_name(slot)}, walls, scenery and items in HD (critters stay legacy), "
                 "1280x960, 1:1 output pixels")
        save(sheet(title, rows, labels), args.out / f"slot{slot:02d}.png")

    for slot in FULL_SLOTS:
        out = run(f"slot{slot:02d}_wide", slot, WIDE, args.reuse)
        image = Image.open(out / "mixed.png").convert("RGB")
        image = image.resize((image.width // 2, image.height // 2), Image.LANCZOS)
        save(image, args.out / f"fullframe_mixed_slot{slot:02d}.png")
    subprocess.run([str(hdlib.REPO_ROOT / "tools/dev/reset-saves.sh")], check=True, stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
