#!/usr/bin/env python3
"""Phase 2.11 proposal: HD lights (coloured light, glow, shadows).

Runs the game unattended (hidden window, no audio) at 3440x1440 (scale 2,
logical 1720x720) with the `mixed` pack and `[graphics] hd_lights=1`, using
tests/scripts/hd_lights.txt, which captures one view with the lights off
(current HD), coloured light only, + glow and + shadows, plus eight frames
for the fire flicker. Writes to docs/screens/phase2.11_lights/:

  den_0_current.png ... den_3_shadows.png   Den at night (slot 8), full
                                            3440x1440 frames per variant
  arroyo_0_current.png, arroyo_3_shadows.png
                                            Arroyo by day (slot 3): daytime
                                            is not degraded
  crop_fire.png, crop_neon.png              1:1 output-pixel crops, all
                                            variants side by side
  crop_neon_spill.png                       with --previous FRAME (an earlier
                                            den_3_shadows.png): current HD |
                                            that frame | now, at the sign
  fire_flicker.png                          animated PNG (APNG) of the fire
  fire_flicker_strip.png                    the same frames side by side
  den_field.png                             the light field the presenter
                                            applies (extra light, glow,
                                            shadows), upscaled to the screen

  lights_sheet.py [--out DIR] [--reuse]   # --reuse: keep earlier game runs

Needs a build (HD_BUILD_DIR, default out/build/macos; a Release build is
faster), the run directory (tools/dev/setup-run-dir.sh) and the packs.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

import hdlib

RUN_DIR = hdlib.BUILD_DIR
EXE = RUN_DIR / "Fallout II Community Edition.app/Contents/MacOS/Fallout II Community Edition"
SCRIPT = hdlib.REPO_ROOT / "tests/scripts/hd_lights.txt"
WORK = RUN_DIR / "devout/phase2.11_lights"
ARGS = ["[screen]resolution_x=3440", "[screen]resolution_y=1440", "[screen]scale=2",
        "[graphics]hd=1", "[graphics]hd_pack=mixed", "[graphics]hd_text=1", "[graphics]hd_lights=1"]
VARIANTS = [
    ("off", "0_current", "current HD (hd_lights=0)"),
    ("colour", "1_colour", "(a) coloured light"),
    ("glow", "2_glow", "(b) coloured light + glow"),
    ("all", "3_shadows", "(c) coloured light + glow + shadows (hd_lights=1 default)"),
]
SAVES = {"den": 8, "arroyo": 3}
# 1:1 crops in output pixels (x, y, w, h) of the Den view.
NEON_CROP = (2680, 620, 560, 420)
FIRE_SIZE = (720, 520)
BG = (34, 34, 40)
PAD = 8
MAX_BYTES = 8 * 1024 * 1024


def font(size: int = 18):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # Pillow < 10.1
        return ImageFont.load_default()


def run(name: str, slot: int, reuse: bool) -> Path:
    out = WORK / name
    if reuse and (out / "anim08.png").exists():
        return out
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    subprocess.run([str(hdlib.REPO_ROOT / "tools/dev/reset-saves.sh")], check=True, stdout=subprocess.DEVNULL)
    subprocess.run([str(EXE), f"--dev-script={SCRIPT}", f"--dev-out={out}", f"--dev-load-game={slot}", *ARGS],
                   cwd=RUN_DIR, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=180)
    if not (out / "anim08.png").exists():
        raise SystemExit(f"{name}: the game wrote no screenshots")
    print(f"ran {name}")
    return out


def save(image: Image.Image, path: Path, **kwargs) -> None:
    image.save(path, optimize=True, **kwargs)
    size = path.stat().st_size
    if size > MAX_BYTES:
        raise SystemExit(f"{path} is {size / 1e6:.1f} MB (limit 8 MB)")
    print(f"wrote {path} ({size / 1e6:.2f} MB)")


def labelled(path: Path, label: str) -> Image.Image:
    """The full frame with its label in the (black) top-left corner."""
    image = Image.open(path).convert("RGB")
    draw = ImageDraw.Draw(image)
    f = font(28)
    box = draw.textbbox((24, 12), label, font=f)
    draw.rectangle((box[0] - 10, box[1] - 6, box[2] + 10, box[3] + 6), fill=(0, 0, 0))
    draw.text((24, 12), label, fill=(235, 235, 200), font=f)
    return image


def side_by_side(title: str, cells: list[Image.Image], labels: list[str]) -> Image.Image:
    f = font(18)
    width = PAD + sum(c.width + PAD for c in cells)
    height = 34 + 26 + max(c.height for c in cells) + PAD
    canvas = Image.new("RGB", (width, height), BG)
    draw = ImageDraw.Draw(canvas)
    draw.text((PAD, 8), title, fill=(230, 230, 230), font=f)
    x = PAD
    for cell, label in zip(cells, labels):
        draw.text((x, 36), label, fill=(200, 200, 160), font=f)
        canvas.paste(cell, (x, 60))
        x += cell.width + PAD
    return canvas


def fire_crop(out: Path) -> tuple[int, int, int, int]:
    """Output-pixel crop around the fire light closest to the screen centre."""
    lights = json.loads((out / "lights.json").read_text())
    fires = [l for l in lights["lights"] if l["kind"] == "fire"]
    if not fires:
        raise SystemExit("no fire light in the Den view")
    fire = min(fires, key=lambda l: (l["x"] - 860) ** 2 + (l["y"] - 300) ** 2)
    w, h = FIRE_SIZE
    x = int(fire["x"] * 2 - w * 0.55)
    y = int(fire["y"] * 2 - h * 0.45)
    return max(x, 0), max(y, 0), w, h


def field_image(out: Path, size: tuple[int, int]) -> Image.Image:
    """The light field as three panels: extra light (mul, 1.0 = white),
    glow (add, x4) and shadows (darkness, x2), each upscaled to a third of
    the screen width."""
    panels = []
    for name, gain, label in (("mul", 1.0, "coloured light (extra factor)"), ("add", 4.0, "glow (additive, x4)"),
                              ("shade", 2.0, "shadows (darkness, x2)")):
        grid = np.asarray(Image.open(out / f"field_{name}.png").convert("RGB")).astype(float)
        image = Image.fromarray(np.clip(grid * gain, 0, 255).astype(np.uint8))
        panels.append((image.resize((size[0] // 3, size[1] // 3), Image.BILINEAR), label))
    return side_by_side("Den: the light field the presenter applies (screen space, before upscaling: 4 px light "
                        "cells, 2 px shadow cells)", [p for p, _ in panels], [l for _, l in panels])


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=hdlib.REPO_ROOT / "docs/screens/phase2.11_lights")
    ap.add_argument("--reuse", action="store_true", help="reuse game runs from an earlier invocation")
    ap.add_argument("--previous", type=Path, help="an earlier den_3_shadows.png to compare the neon sign with")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    runs = {name: run(name, slot, args.reuse) for name, slot in SAVES.items()}

    for name, out in runs.items():
        title = "Den, night (slot 8)" if name == "den" else "Arroyo, day (slot 3)"
        variants = VARIANTS if name == "den" else [VARIANTS[0], VARIANTS[-1]]
        for shot, suffix, label in variants:
            save(labelled(out / f"{shot}.png", f"{title}: {label}"), args.out / f"{name}_{suffix}.png")

    den = runs["den"]
    labels = [label for _, _, label in VARIANTS]
    x, y, w, h = fire_crop(den)
    cells = [Image.open(den / f"{shot}.png").convert("RGB").crop((x, y, x + w, y + h)) for shot, _, _ in VARIANTS]
    save(side_by_side("Den: burning barrel, 1:1 output pixels (3440x1440)", cells, labels), args.out / "crop_fire.png")

    nx, ny, nw, nh = NEON_CROP
    cells = [Image.open(den / f"{shot}.png").convert("RGB").crop((nx, ny, nx + nw, ny + nh)) for shot, _, _ in VARIANTS]
    save(side_by_side("Den: Becky's neon sign, 1:1 output pixels (3440x1440)", cells, labels), args.out / "crop_neon.png")

    if args.previous is not None:
        crop = (nx, ny, nx + nw, ny + nh)
        cells = [Image.open(den / "off.png").convert("RGB").crop(crop), Image.open(args.previous).convert("RGB").crop(crop),
                 Image.open(den / "all.png").convert("RGB").crop(crop)]
        save(side_by_side("Den: Becky's neon sign, 1:1 output pixels: the sign now also lights the wall around it",
                          cells, ["current HD (hd_lights=0)", "before: glow only (first proposal)", "now: glow + neon light spill"]),
             args.out / "crop_neon_spill.png")

    frames = [Image.open(den / f"anim{i:02d}.png").convert("RGB").crop((x, y, x + w, y + h)) for i in range(1, 9)]
    save(frames[0], args.out / "fire_flicker.png", save_all=True, append_images=frames[1:], duration=120, loop=0)
    strip = side_by_side("Den: fire flicker (consecutive frames, ~60-120 ms apart; light follows the fire palette cycle)",
                         [f.resize((w // 2, h // 2), Image.LANCZOS) for f in frames], [f"frame {i}" for i in range(1, 9)])
    save(strip, args.out / "fire_flicker_strip.png")

    save(field_image(den, (3440, 1440)), args.out / "den_field.png")


if __name__ == "__main__":
    main()
