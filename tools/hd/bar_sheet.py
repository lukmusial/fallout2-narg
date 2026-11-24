#!/usr/bin/env python3
"""Phase 2.16 review sheets: the interface bar before and after.

Runs the game unattended (hidden window, no audio; tests/scripts/hd_bar.txt,
slot 1: the bar, then combat mode) with hd=0 and with hd=1 and the mixed
pack, at 1280x960 and at 3440x1440 (scale 2: HR_IFACE_800 and the side
panels), then writes to docs/screens/phase2.16_bar/:

  game.png, combat.png        legacy (hd=0, pixel-doubled) | HD mixed, 1280x960 each
  bar_crops.png               the bar at 1:1 output pixels, legacy above HD
                              (normal and combat)
  wide_combat.png             3440x1440: legacy above HD
  wide_bar_crops.png          the 800-px bar and side panels at 1:1, legacy above HD

  bar_sheet.py [--out DIR]
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
BG = (34, 34, 40)


def label(image: Image.Image, text: str) -> Image.Image:
    out = Image.new("RGB", (image.width, image.height + 18), BG)
    out.paste(image, (0, 18))
    ImageDraw.Draw(out).text((4, 3), text, fill=(255, 230, 120))
    return out


def stack(images: list[Image.Image], horizontal: bool, pad: int = 8) -> Image.Image:
    if horizontal:
        out = Image.new("RGB", (sum(i.width for i in images) + pad * (len(images) - 1), max(i.height for i in images)), BG)
    else:
        out = Image.new("RGB", (max(i.width for i in images), sum(i.height for i in images) + pad * (len(images) - 1)), BG)
    offset = 0
    for image in images:
        out.paste(image, (offset, 0) if horizontal else (0, offset))
        offset += (image.width if horizontal else image.height) + pad
    return out


def run(name: str, extra: list[str]) -> Path:
    rt.reset_saves()
    out, _ = rt.run_game(name, "hd_bar.txt", ["--dev-load-game=1", *extra])
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=hdlib.REPO_ROOT / "docs/screens/phase2.16_bar")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    legacy, hd = run("sheet216_legacy", []), run("sheet216_hd", HD)
    crops = []
    for screen in ("game", "combat"):
        before = Image.open(legacy / f"{screen}.png").convert("RGB").resize((1280, 960), Image.NEAREST)
        after = Image.open(hd / f"{screen}.png").convert("RGB")
        stack([label(before, f"{screen}: legacy (hd=0, 2x)"), label(after, f"{screen}: HD, mixed pack")], True) \
            .save(args.out / f"{screen}.png", optimize=True)
        crops += [label(before.crop((0, 760, 1280, 960)), f"{screen}: legacy"),
                  label(after.crop((0, 760, 1280, 960)), f"{screen}: HD")]
    stack(crops, False).save(args.out / "bar_crops.png", optimize=True)

    legacy, hd = run("sheet216_wide_legacy", WIDE), run("sheet216_wide_hd", [*HD, *WIDE])
    before = Image.open(legacy / "combat.png").convert("RGB").resize((3440, 1440), Image.NEAREST)
    after = Image.open(hd / "combat.png").convert("RGB")
    stack([label(before, "3440x1440 combat: legacy (hd=0)"), label(after, "3440x1440 combat: HD, mixed pack")], False) \
        .save(args.out / "wide_combat.png", optimize=True)
    rows = []
    for x0 in (0, 1220, 2240):
        rows += [label(before.crop((x0, 1240, x0 + 1200, 1440)), f"legacy x {x0}"),
                 label(after.crop((x0, 1240, x0 + 1200, 1440)), f"HD x {x0}")]
    stack(rows, False).save(args.out / "wide_bar_crops.png", optimize=True)
    for path in sorted(args.out.glob("*.png")):
        print(path)
    rt.reset_saves()


if __name__ == "__main__":
    main()
