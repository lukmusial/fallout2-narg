#!/usr/bin/env python3
"""Phase 2.14 review sheets: the other static screens before and after.

Runs the game unattended (hidden window, no audio; tests/scripts
hd_static_game.txt, hd_static_menu.txt, hd_static_death.txt,
worldmap_scroll.txt and hd_static_wide.txt) with hd=0 and with hd=1 and the mixed pack, then writes
to docs/screens/phase2.14/:

  <screen>.png         legacy (hd=0, pixel-doubled) | HD mixed pack, 1280x960 each
  wide_<screen>.png    the same screen at 3440x1440 (scale 2) with the mixed pack
  mixed_choice.png     the art the mixed pack takes from scripted: source (2x
                       nearest) | scripted | ai, at 2x output pixels

  static_sheet.py [--out DIR]

Needs a build (cmake --build out/build/macos), the run directory
(tools/dev/setup-run-dir.sh) and both packs installed (install.py).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw

import hdlib

sys.path.insert(0, str(hdlib.REPO_ROOT / "tests"))
import run_tests as rt  # noqa: E402

HD = ["[graphics]hd=1", "[graphics]hd_text=1", "[graphics]hd_pack=mixed"]
WIDE = ["[screen]resolution_x=3440", "[screen]resolution_y=1440", "[screen]scale=2"]
RUNS = {
    # run: (script, extra args, screens)
    "game": ("hd_static_game.txt", ["--dev-load-game=1"],
             ["pipboy", "automap", "skilldex", "options", "preferences", "help", "dialog"]),
    "menu": ("hd_static_menu.txt", [], ["selector"]),
    "death": ("hd_static_death.txt", ["--dev-load-game=1"], ["death"]),
    "worldmap": ("worldmap_scroll.txt", ["--dev-load-game=1"], ["s0", "town"]),
}
NAMES = {"s0": "worldmap", "town": "townmap"}
MIXED_CHOICE = ["numbers", "months", "bignum"]
BG = (34, 34, 40)


def label(image: Image.Image, text: str) -> Image.Image:
    out = Image.new("RGB", (image.width, image.height + 18), BG)
    out.paste(image, (0, 18))
    ImageDraw.Draw(out).text((4, 3), text, fill=(255, 230, 120))
    return out


def side_by_side(images: list[Image.Image], pad: int = 8) -> Image.Image:
    out = Image.new("RGB", (sum(i.width for i in images) + pad * (len(images) - 1), max(i.height for i in images)), BG)
    x = 0
    for image in images:
        out.paste(image, (x, 0))
        x += image.width + pad
    return out


def mixed_choice(out_dir: Path) -> None:
    rows = []
    for name in MIXED_CHOICE:
        key = f"art/intrface/{name}.png"
        source = Image.open(hdlib.SRC_DIR / key).convert("RGB")
        source = source.resize((source.width * 2, source.height * 2), Image.NEAREST)
        packs = []
        for pack in ("scripted", "ai"):
            image = Image.open(hdlib.ASSETS_DIR / pack / key).convert("RGBA")
            flat = Image.new("RGBA", image.size, BG + (255,))
            flat.alpha_composite(image)
            packs.append(flat.convert("RGB"))
        row = [label(i.resize((i.width * 2, i.height * 2), Image.NEAREST), f"{name}: {t}")
               for i, t in zip([source, *packs], ("source 2x nearest", "scripted", "ai"))]
        rows.append(side_by_side(row))
    sheet = Image.new("RGB", (max(r.width for r in rows), sum(r.height + 8 for r in rows)), BG)
    y = 0
    for row in rows:
        sheet.paste(row, (0, y))
        y += row.height + 8
    sheet.save(out_dir / "mixed_choice.png", optimize=True)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=hdlib.REPO_ROOT / "docs/screens/phase2.14")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    for run, (script, extra, screens) in RUNS.items():
        rt.reset_saves()
        legacy, _ = rt.run_game(f"sheet214_{run}_legacy", script, extra)
        rt.reset_saves()
        hd, _ = rt.run_game(f"sheet214_{run}_hd", script, [*extra, *HD])
        for screen in screens:
            name = NAMES.get(screen, screen)
            before = Image.open(legacy / f"{screen}.png").convert("RGB")
            before = before.resize((1280, 960), Image.NEAREST)
            after = Image.open(hd / f"{screen}.png").convert("RGB")
            sheet = side_by_side([label(before, f"{name}: legacy (hd=0, 2x)"), label(after, f"{name}: HD, mixed pack")])
            sheet.save(args.out / f"{name}.png", optimize=True)
            print(args.out / f"{name}.png")
    rt.reset_saves()
    wide, _ = rt.run_game("sheet214_wide", "hd_static_wide.txt", ["--dev-load-game=1", *HD, *WIDE])
    for name in ("pipboy", "worldmap"):
        Image.open(wide / f"{name}.png").convert("RGB").save(args.out / f"wide_{name}.png", optimize=True)
        print(args.out / f"wide_{name}.png")
    mixed_choice(args.out)
    rt.reset_saves()


if __name__ == "__main__":
    main()
