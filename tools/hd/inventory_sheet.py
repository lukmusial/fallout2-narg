#!/usr/bin/env python3
"""Phase 2.10 proposal sheets: the inventory, loot and trade windows and the
interface bar's item button, legacy | scripted | ai | mixed (with the
proposed manifest additions), at 1:1 output pixels (legacy pixel-doubled),
plus one full-resolution 3440x1440 inventory frame per pack.

Runs the game hidden (tests/scripts/hd_inventory.txt on slot 1) and writes
docs/screens/phase2.10_inventory/. The "mixed (proposed)" column uses a
temporary pack in the run directory (data/hd/mixedprop: the shipped mixed
manifest plus PROPOSED below), so nothing is applied to the real one.

  inventory_sheet.py [--out DIR] [--reuse]
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image

import hdlib
from screens_sheet import load, sheet

RUN_DIR = hdlib.BUILD_DIR
EXE = RUN_DIR / "Fallout II Community Edition.app/Contents/MacOS/Fallout II Community Edition"
SCRIPT = hdlib.REPO_ROOT / "tests/scripts/hd_inventory.txt"
WORK = RUN_DIR / "devout/phase2.10"
WIDE = ["[screen]resolution_x=3440", "[screen]resolution_y=1440", "[screen]scale=2"]
MAX_BYTES = 8 * 1024 * 1024

# Proposed additions to files/ce.dat/hd/mixed/manifest.txt (not applied):
# see README "Inventory" and docs/screens/phase2.10_inventory/mixed_candidates.png.
PROPOSED = [
    # Scroll arrows: the AI paints blue streaks into the down arrows and
    # smears the up arrows (same failure as the 11x14 load-screen arrows).
    "art/intrface/invupout.frm", "art/intrface/invupin.frm", "art/intrface/invupds.frm",
    "art/intrface/invdnout.frm", "art/intrface/invdnin.frm", "art/intrface/invdnds.frm",
    "art/intrface/di_up1.frm", "art/intrface/di_up2.frm", "art/intrface/di_down1.frm", "art/intrface/di_down2.frm",
    "art/intrface/di_upds.png", "art/intrface/di_dnds.png",
    # Super tool kit: the AI turns the speckled red paint into wood grain.
    "art/inven/suprtool.frm",
]

PACKS = ("scripted", "ai", "mixedprop")
LABELS = ["legacy (pixel-doubled)", "scripted", "ai", "mixed (+ proposed)"]
# Logical 640x480 rectangles per screen.
CROPS = {
    "inventory": (80, 20, 510, 350),
    "loot": (70, 10, 510, 360),
    "barter": (0, 295, 640, 185),
    "bar": (190, 385, 280, 95),
}
TITLES = {
    "inventory": "Inventory (slot 1): window, buttons and items in HD; item art resampled into the slots",
    "loot": "Loot window on Sulik (slot 1)",
    "barter": "Trade window (dialogue with Sulik, slot 1)",
    "bar": "Interface bar: the item button (button art + item, the action text and AP stay legacy)",
}


def write_mixedprop() -> None:
    base = (hdlib.REPO_ROOT / "files/ce.dat/hd/mixed/manifest.txt").read_text()
    pack = RUN_DIR / "data/hd/mixedprop"
    pack.mkdir(parents=True, exist_ok=True)
    lines = [base.rstrip(), "", "# Phase 2.10 proposals (inventory_sheet.py)"] + [f"{p} = scripted" for p in PROPOSED]
    (pack / "manifest.txt").write_text("\n".join(lines) + "\n")


def run(name: str, args: list[str], reuse: bool) -> Path:
    out = WORK / name
    if reuse and (out / "barter.png").exists():
        return out
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    subprocess.run([str(hdlib.REPO_ROOT / "tools/dev/reset-saves.sh")], check=True, stdout=subprocess.DEVNULL)
    subprocess.run([str(EXE), f"--dev-script={SCRIPT}", f"--dev-out={out}", "--dev-load-game=1", "--dev-fast", *args],
                   cwd=RUN_DIR, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=180)
    print(f"ran {name}")
    return out


def save(image: Image.Image, path: Path) -> None:
    image.save(path, optimize=True)
    size = path.stat().st_size
    if size > MAX_BYTES:
        raise SystemExit(f"{path} is {size / 1e6:.1f} MB (limit 8 MB)")
    print(f"wrote {path} ({size / 1e6:.2f} MB)")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=hdlib.REPO_ROOT / "docs/screens/phase2.10_inventory")
    ap.add_argument("--reuse", action="store_true", help="reuse game runs from an earlier invocation")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    write_mixedprop()

    hd = ["[graphics]hd=1"]
    runs = {"legacy": run("legacy", [], args.reuse)}
    for pack in PACKS:
        runs[pack] = run(pack, [*hd, f"[graphics]hd_pack={pack}"], args.reuse)

    for screen, (x, y, w, h) in CROPS.items():
        shot = "game" if screen == "bar" else screen
        frames = [load(runs["legacy"] / f"{shot}.png", double=True)]
        frames += [load(runs[pack] / f"{shot}.png", double=False) for pack in PACKS]
        row = [f[y * 2:(y + h) * 2, x * 2:(x + w) * 2] for f in frames]
        save(sheet(TITLES[screen], [row], LABELS), args.out / f"{screen}.png")

    # Full resolution (3440x1440, logical 1720x720: the two-column inventory).
    for pack in PACKS:
        out = run(f"{pack}_wide", [*hd, f"[graphics]hd_pack={pack}", *WIDE], args.reuse)
        image = Image.open(out / "inventory.png").convert("RGB")
        if image.size != (3440, 1440):
            raise SystemExit(f"{pack}: full frame is {image.size}")
        save(image, args.out / f"fullres_{'mixed' if pack == 'mixedprop' else pack}_inventory.png")


if __name__ == "__main__":
    main()
