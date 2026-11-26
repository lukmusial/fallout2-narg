#!/usr/bin/env python3
"""Build an English 800-pixel interface bar (HR_IFACE_800) from the original one.

Reference for the engine, which builds this bar at run time from the player's
own iface.frm (src/iface_bar_art.{h,cc}); nothing made with this script is
shipped. The HR_IFACE_800.FRM bundled in files/f2_res.dat is a Russian redraw
of the bar (labels such as "ИНВ", "КРТ", "Навыки"). The engine's 800-wide
layout is the 640-wide bar with a message box 160 pixels wider, so the English
version is the original iface.frm with the middle of its message box repeated;
every other pixel keeps its original palette index.

  iface_bar_art.py --iface iface.png --out hr_iface_800.png

iface.png: art/intrface/iface.frm from master.dat as a palette-indexed PNG
(`ce-frm2png iface.frm iface.png --palette color.pal --opaque`).
"""Build an English 800-pixel interface bar (HR_IFACE_800) from the original one.

The HR_IFACE_800.FRM bundled in files/f2_res.dat is a Russian redraw of the bar
(labels such as "ИНВ", "КРТ", "Навыки"). The engine's 800-wide layout is the
640-wide bar with a message box 160 pixels wider, so the English version is the
original iface.frm with the middle of its message box repeated; every other
pixel keeps its original palette index.

  iface_bar_art.py --iface iface.png --out hr_iface_800.png
  ce-frm2png hr_iface_800.png files/ce.dat/art/intrface/HR_IFACE_800.FRM --palette color.pal

iface.png: art/intrface/iface.frm from master.dat as a palette-indexed PNG
(`ce-frm2png iface.frm iface.png --palette color.pal --opaque`).
"""

import argparse

import numpy as np
from PIL import Image

EXTRA_WIDTH = 160
# Columns 100..119 lie inside the message box, between its rounded ends.
SEGMENT = (100, 120)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--iface", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    image = Image.open(args.iface)
    if image.mode != "P" or image.size != (640, 99):
        raise SystemExit("expected the 640x99 palette-indexed iface.frm")
    pixels = np.asarray(image)

    start, end = SEGMENT
    segment = pixels[:, start:end]
    repeats = EXTRA_WIDTH // (end - start)
    wide = np.hstack([pixels[:, :start]] + [segment] * repeats + [pixels[:, start:]])

    out = Image.fromarray(wide, mode="P")
    out.putpalette(image.getpalette())
    out.save(args.out)


if __name__ == "__main__":
    main()
