#!/usr/bin/env python3
"""Build an English 800-pixel interface bar (HR_IFACE_800) from the original one.

Reference for the engine, which builds this bar at run time from the player's
own iface.frm (src/iface_bar_art.{h,cc}); nothing made with this script is
shipped. The HR_IFACE_800.FRM bundled in files/f2_res.dat is a Russian redraw
of the bar (labels such as "ИНВ", "КРТ", "Навыки"). The engine's 800-wide
layout is the 640-wide bar with a message box 160 pixels wider, so the English
version is the original iface.frm with the straight part of its message box
(columns 30..184) stretched: each column appears 1 to 3 times, irregularly, so
nothing repeats; every pixel keeps its original palette index.

  iface_bar_art.py --iface iface.png --out hr_iface_800.png

iface.png: art/intrface/iface.frm from master.dat as a palette-indexed PNG
(`ce-frm2png iface.frm iface.png --palette color.pal --opaque`).
"""

import argparse

import numpy as np
from PIL import Image

WIDTH = 800
# The straight part of the message box, between its rounded ends.
SPAN = (30, 185)


def column_counts(width):
    """As interfaceBarArtColumnCounts: how often each span column appears."""
    start, end = SPAN
    span = end - start
    length = span + width - 640
    state = 0x2545F491

    def rand():
        nonlocal state
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        return state >> 8

    base = length // span
    counts = [base] * span
    extra = length - base * span
    while extra > 0:
        index = rand() % span
        if counts[index] == base:
            counts[index] += 1
            extra -= 1
    for _ in range(2 * span):
        to, source = rand() % span, rand() % span
        if to != source and counts[to] < base + 1 and counts[source] > base - 1 and counts[source] > 1:
            counts[to] += 1
            counts[source] -= 1
    return counts


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--iface", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    image = Image.open(args.iface)
    if image.mode != "P" or image.size != (640, 99):
        raise SystemExit("expected the 640x99 palette-indexed iface.frm")
    pixels = np.asarray(image)

    start, end = SPAN
    columns = list(range(start))
    for index, count in enumerate(column_counts(WIDTH)):
        columns += [start + index] * count
    columns += list(range(end, 640))
    wide = pixels[:, columns]

    out = Image.fromarray(wide, mode="P")
    out.putpalette(image.getpalette())
    out.save(args.out)


if __name__ == "__main__":
    main()
