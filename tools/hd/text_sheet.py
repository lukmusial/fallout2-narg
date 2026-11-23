#!/usr/bin/env python3
"""Phase 2.9 proposal sheets: text at 2x (HD text) as the engine shows it.

Runs the game unattended (hidden window, no audio) at 3440x1440 (scale 2,
logical 1720x720) with the `mixed` pack and HD text off, `sharp` and
`smooth` (tests/scripts/hd_text_screens.txt: main menu, load screen,
Stimpack hub and character editor; tests/scripts/hd_text_game.txt: a game
view with interface bar messages and floating text), then writes to
docs/screens/phase2.9_text/:

  <screen>.png            crops at 1:1 output pixels: current (text
                          pixel-doubled) | (a) sharp | (b) smooth
  zoom_<screen>.png       one crop of each at 3x, to judge glyph shapes

  text_sheet.py [--out DIR] [--reuse]   # --reuse: keep earlier game runs

Needs a build (cmake --build out/build/macos), the run directory
(tools/dev/setup-run-dir.sh) and the packs installed (install.py).
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
SCRIPTS = hdlib.REPO_ROOT / "tests/scripts"
WORK = RUN_DIR / "devout/phase2.9_text"
WIDE = ["[screen]resolution_x=3440", "[screen]resolution_y=1440", "[screen]scale=2"]
MODES = {
    "current": ["[graphics]hd_text=0"],
    "sharp": ["[graphics]hd_text=1", "[graphics]hd_text_style=sharp"],
    "smooth": ["[graphics]hd_text=1", "[graphics]hd_text_style=smooth"],
}
LABELS = ["current (text pixel-doubled)", "(a) sharp: EPX of the original glyphs", "(b) smooth: re-rasterised, anti-aliased"]
BG = (34, 34, 40)
PAD = 8
MAX_BYTES = 3 * 1024 * 1024

# Logical 1720x720 crops (x, y, w, h), shown at 1:1 output pixels (2x). The
# 640x480 screens are centred at (540, 120).
CROPS = {
    "menu": [(290, 25, 200, 290), (225, 694, 400, 16)],
    "load": [(580, 140, 260, 160), (925, 360, 200, 140)],
    "hub": [(780, 210, 160, 300)],
    "editor": [(550, 120, 320, 140), (720, 290, 150, 140), (885, 120, 290, 120), (880, 385, 290, 110)],
    "game": [(475, 655, 340, 50), (875, 640, 380, 80)],
}
TITLES = {
    "menu": "Main menu: button labels (font 104 over the HD background), copyright line; 3440x1440",
    "load": "Load screen: title, slot list (font 101), save details, button labels",
    "hub": "Stimpack hub: name, button labels (drawn into the button images)",
    "editor": "Stimpack character editor: name/age/sex, SPECIAL, derived stats, skills, info card",
    "game": "Game view: interface bar messages (font 101), AP / HP / AC labels",
}
# Zoomed crops (logical x, y, w, h), output pixels shown at 3x.
ZOOM = {
    "editor": (730, 160, 110, 30),
    "load": (590, 205, 120, 30),
    "menu": (300, 30, 190, 40),
    "game": (480, 660, 170, 30),
}


def font(size: int = 16):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # Pillow < 10.1
        return ImageFont.load_default()


def run(name: str, script: str, args: list[str], reuse: bool, expect: str) -> Path:
    out = WORK / name
    if reuse and (out / expect).exists():
        return out
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    subprocess.run([str(hdlib.REPO_ROOT / "tools/dev/reset-saves.sh")], check=True, stdout=subprocess.DEVNULL)
    for attempt in range(2):
        subprocess.run([str(EXE), f"--dev-script={SCRIPTS / script}", f"--dev-out={out}", *args],
                       cwd=RUN_DIR, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=180)
        if (out / expect).exists():
            break
        print(f"{name}: no output, retrying")
    else:
        raise SystemExit(f"{name}: the game wrote no screenshots")
    print(f"ran {name}")
    return out


def sheet(title: str, rows: list[list[np.ndarray]], labels: list[str]) -> Image.Image:
    f = font(16)
    col_w = [max(max(row[i].shape[1] for row in rows), 300) for i in range(len(labels))]
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
    ap.add_argument("--out", type=Path, default=hdlib.REPO_ROOT / "docs/screens/phase2.9_text")
    ap.add_argument("--reuse", action="store_true", help="reuse game runs from an earlier invocation")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    runs = {}
    for mode, extra in MODES.items():
        base = ["[graphics]hd=1", "[graphics]hd_pack=mixed", *WIDE, *extra]
        runs[mode] = run(mode, "hd_text_screens.txt", base, args.reuse, "editor.png")
        runs[mode + "_game"] = run(mode + "_game", "hd_text_game.txt", ["--dev-load-game=3", *base], args.reuse, "game.png")

    def frame(mode: str, screen: str) -> np.ndarray:
        folder = runs[mode + "_game"] if screen == "game" else runs[mode]
        return np.asarray(Image.open(folder / f"{screen}.png").convert("RGB"))

    for screen, crops in CROPS.items():
        frames = [frame(mode, screen) for mode in MODES]
        rows = [[f[y * 2:(y + h) * 2, x * 2:(x + w) * 2] for f in frames] for x, y, w, h in crops]
        save(sheet(TITLES[screen], rows, LABELS), args.out / f"{screen}.png")

    for screen, (x, y, w, h) in ZOOM.items():
        frames = [frame(mode, screen) for mode in MODES]
        row = [np.asarray(Image.fromarray(f[y * 2:(y + h) * 2, x * 2:(x + w) * 2]).resize((w * 6, h * 6), Image.NEAREST))
               for f in frames]
        save(stack_zoom(TITLES[screen] + " (output pixels at 3x)", row), args.out / f"zoom_{screen}.png")

    # Floating text over the map (found from the legacy white pixels).
    frames = [frame(mode, "game") for mode in MODES]
    current = frames[0][:1120]
    white = np.all(current >= 250, axis=2)
    ys, xs = np.nonzero(white)
    if len(ys):
        cx, cy = int(np.median(xs)), int(np.median(ys))
        x0, y0 = max(cx - 260, 0), max(cy - 60, 0)
        rows = [[f[y0:y0 + 120, x0:x0 + 520] for f in frames]]
        save(sheet("Game view: floating text over the player (font 101, white with black outline)", rows, LABELS),
             args.out / "game_float.png")


def stack_zoom(title: str, cells: list[np.ndarray]) -> Image.Image:
    f = font(16)
    width = max(c.shape[1] for c in cells) + 2 * PAD
    height = 28 + sum(c.shape[0] + 22 + PAD for c in cells) + PAD
    canvas = Image.new("RGB", (width, height), BG)
    draw = ImageDraw.Draw(canvas)
    draw.text((PAD, 6), title, fill=(230, 230, 230), font=f)
    y = 28
    for label, cell in zip(LABELS, cells):
        draw.text((PAD, y), label, fill=(200, 200, 160), font=f)
        y += 22
        canvas.paste(Image.fromarray(cell), (PAD, y))
        y += cell.shape[0] + PAD
    return canvas


if __name__ == "__main__":
    main()
