#!/usr/bin/env python3
"""Phase 2.6 proposal sheets: the interface screens as the engine shows them.

Runs the game unattended (tests/scripts/hd_screens.txt, hidden window, no
audio) with hd=0, and with hd=1 for each installed pack, at 1280x960 and at
3440x1440 (scale 2), then writes to docs/screens/phase2.6/:

  <screen>.png            crops at 1:1 output pixels: legacy (pixel-doubled)
                          | scripted | ai, one row per crop
  fullframe_ai_<screen>.png  3440x1440 frames with the AI pack, half size

  screens_sheet.py [--out DIR] [--reuse]   # --reuse: keep earlier game runs

Needs a build (cmake --build out/build/macos), the run directory
(tools/dev/setup-run-dir.sh) and both packs installed (install.py).
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

import hdlib

RUN_DIR = hdlib.BUILD_DIR
EXE = RUN_DIR / "Fallout II Community Edition.app/Contents/MacOS/Fallout II Community Edition"
SCRIPT = hdlib.REPO_ROOT / "tests/scripts/hd_screens.txt"
WORK = RUN_DIR / "devout/phase2.6"
PACKS = ("scripted", "ai")
WIDE = ["[screen]resolution_x=3440", "[screen]resolution_y=1440", "[screen]scale=2"]
BG = (34, 34, 40)
PAD = 8
MAX_BYTES = 3 * 1024 * 1024

# Crops in logical pixels (x, y, w, h) of each screen; the 3440x1440 menu uses
# 1720x720 logical coordinates. Shown at 1:1 output pixels (2x).
CROPS = {
    "menu": [(10, 200, 250, 130), (300, 110, 250, 130)],
    "menu_wide": [(220, 10, 250, 150), (760, 130, 250, 150)],
    "load": [(20, 15, 250, 130), (330, 225, 250, 150)],
    "hub": [(238, 76, 164, 328)],
    "editor": [(0, 0, 250, 140), (0, 250, 250, 90), (340, 225, 250, 90)],
    "perks": [(33, 125, 270, 230)],
    "creation": [(0, 250, 250, 140)],
}
TITLES = {
    "menu": "Main menu (mainmenu_sp.png, menuup/menudown), 1280x960",
    "menu_wide": "Main menu hi-res (HR_MAINMENU.FRM + hr_menu_bg_sp.png), 3440x1440",
    "load": "Load/save screen (lsgame, lscover, lilred*, arrows)",
    "hub": "Stimpak hub (opbase_sp.png, opbtnoff/opbtnon)",
    "editor": "Stimpak character editor (edtrcrte + edtredt level box, buttons, slider)",
    "perks": "Perk dialog (perkwin + editor buttons)",
    "creation": "Character creation (edtrcrte, trait buttons)",
}
FULL_FRAMES = ("menu", "hub", "editor", "load")


def font(size: int = 16):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # Pillow < 10.1
        return ImageFont.load_default()


def run(name: str, args: list[str], reuse: bool) -> Path:
    out = WORK / name
    if reuse and (out / "creation.png").exists():
        return out
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    subprocess.run([str(hdlib.REPO_ROOT / "tools/dev/reset-saves.sh")], check=True, stdout=subprocess.DEVNULL)
    before = set(RUN_DIR.glob("scr*.*"))
    for attempt in range(2):
        subprocess.run([str(EXE), f"--dev-script={SCRIPT}", f"--dev-out={out}", *args],
                       cwd=RUN_DIR, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=180)
        if (out / "creation.png").exists():
            break
        print(f"{name}: no output, retrying")  # seen once: the game exited 0 without running the script
    else:
        raise SystemExit(f"{name}: the game wrote no screenshots")
    for path in set(RUN_DIR.glob("scr*.*")) - before:  # the script's F12 test shot
        path.unlink()
    print(f"ran {name}")
    return out


def load(path: Path, double: bool) -> np.ndarray:
    image = np.asarray(Image.open(path).convert("RGB"))
    if double:
        image = image.repeat(2, axis=0).repeat(2, axis=1)
    return image


def sheet(title: str, rows: list[list[np.ndarray]], labels: list[str]) -> Image.Image:
    f = font(16)
    col_w = [max(row[i].shape[1] for row in rows) for i in range(len(labels))]
    width = PAD + sum(w + PAD for w in col_w)
    height = 28 + 22 + sum(max(c.shape[0] for c in row) + PAD for row in rows) + PAD
    canvas = Image.new("RGB", (width, height), BG)
    draw = ImageDraw.Draw(canvas)
    draw.text((PAD, 6), title, fill=(230, 230, 230), font=f)
    x = PAD
    for label, w in zip(labels, col_w):
        draw.text((x, 30), label, fill=(200, 200, 160), font=f)
        x += w + PAD
    y = 52
    for row in rows:
        x = PAD
        for cell, w in zip(row, col_w):
            canvas.paste(Image.fromarray(cell), (x, y))
            x += w + PAD
        y += max(c.shape[0] for c in row) + PAD
    return canvas


def save(image: Image.Image, path: Path) -> None:
    image.save(path, optimize=True)
    size = path.stat().st_size
    if size > MAX_BYTES:
        raise SystemExit(f"{path} is {size / 1e6:.1f} MB (limit 3 MB)")
    print(f"wrote {path} ({size / 1e6:.2f} MB)")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=hdlib.REPO_ROOT / "docs/screens/phase2.6")
    ap.add_argument("--reuse", action="store_true", help="reuse game runs from an earlier invocation")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    runs = {"legacy": run("legacy", [], args.reuse), "legacy_wide": run("legacy_wide", WIDE, args.reuse)}
    for pack in PACKS:
        hd = ["[graphics]hd=1", f"[graphics]hd_pack={pack}"]
        runs[pack] = run(pack, hd, args.reuse)
        runs[pack + "_wide"] = run(pack + "_wide", [*hd, *WIDE], args.reuse)

    labels = ["legacy (pixel-doubled)", "scripted", "ai"]
    for screen, crops in CROPS.items():
        wide = screen.endswith("_wide")
        name = screen.removesuffix("_wide")
        suffix = "_wide" if wide else ""
        frames = [load(runs["legacy" + suffix] / f"{name}.png", double=True)]
        frames += [load(runs[pack + suffix] / f"{name}.png", double=False) for pack in PACKS]
        rows = []
        for x, y, w, h in crops:
            rows.append([f[y * 2:(y + h) * 2, x * 2:(x + w) * 2] for f in frames])
        save(sheet(TITLES[screen], rows, labels), args.out / f"{screen}.png")

    for screen in FULL_FRAMES:
        image = Image.open(runs["ai_wide"] / f"{screen}.png").convert("RGB")
        image = image.resize((image.width // 2, image.height // 2), Image.LANCZOS)
        save(image, args.out / f"fullframe_ai_{screen}.png")


if __name__ == "__main__":
    main()
