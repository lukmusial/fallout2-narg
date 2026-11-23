#!/usr/bin/env python3
"""Tile screenshots into one labelled sheet for phase reviews.

  contact_sheet.py OUT.png --cols 5 --scale 0.5 LABEL=IMAGE.png [LABEL=IMAGE.png ...]
"""

import argparse

from PIL import Image, ImageDraw


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output")
    parser.add_argument("images", nargs="+", help="LABEL=PATH")
    parser.add_argument("--cols", type=int, default=5)
    parser.add_argument("--scale", type=float, default=0.5)
    args = parser.parse_args()

    entries = []
    for item in args.images:
        label, path = item.split("=", 1)
        image = Image.open(path).convert("RGB")
        size = (round(image.width * args.scale), round(image.height * args.scale))
        entries.append((label, image.resize(size, Image.LANCZOS)))

    cell_w = max(img.width for _, img in entries)
    cell_h = max(img.height for _, img in entries)
    label_h = 18
    pad = 6
    cols = min(args.cols, len(entries))
    rows = (len(entries) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (cell_w + pad) + pad, rows * (cell_h + label_h + pad) + pad), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    for index, (label, image) in enumerate(entries):
        x = pad + (index % cols) * (cell_w + pad)
        y = pad + (index // cols) * (cell_h + label_h + pad)
        draw.text((x + 2, y + 2), label, fill=(220, 220, 160))
        sheet.paste(image, (x, y + label_h))
    sheet.save(args.output)


if __name__ == "__main__":
    main()
