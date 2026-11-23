#!/usr/bin/env python3
"""Comparison sheets: original (nearest 2x) | scripted | AI.

HD art is previewed the way the engine's HD layer would show it: HD pixels
over the pixel-doubled legacy frame, so alpha-0 colour-cycling pixels show
the legacy colour (first frame of the cycle) and index 0 shows the sheet
background.

  contact_sheet.py                      # all sheets -> docs/screens/phase2.5/
  contact_sheet.py --out /tmp/sheets
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

import hdlib
import tilemap

PACKS = ("scripted", "ai")
BG = (34, 34, 40)
PAD = 8
MAX_BYTES = 3 * 1024 * 1024

# Interface crops (x, y, w, h in source pixels), shown at 4x of the source.
CROPS = {
    "mainmenu": [(20, 20, 110, 70), (240, 120, 120, 76), (420, 380, 110, 70)],
    "lsgame": [(20, 20, 110, 70), (330, 30, 120, 76), (40, 380, 110, 70)],
    "edtredt": [(10, 10, 110, 70), (180, 20, 120, 76), (330, 270, 110, 70)],
    "edtrcrte": [(10, 10, 110, 70), (180, 20, 120, 76), (330, 270, 110, 70)],
}


def font(size: int = 16):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # Pillow < 10.1
        return ImageFont.load_default()


def legacy2x(idx: np.ndarray, pal: np.ndarray) -> np.ndarray:
    rgb = hdlib.preview_palette(pal)[idx]
    rgb[idx == 0] = BG
    return hdlib.nearest2x(rgb)


def preview(pack: str | None, asset: dict) -> np.ndarray:
    """2x RGB preview of an asset for a pack (None = original)."""
    idx, pal = hdlib.load_indexed(hdlib.source_path(asset))
    base = legacy2x(idx, pal)
    if pack is None:
        return base
    hd = np.asarray(Image.open(hdlib.out_path(pack, asset)).convert("RGBA"))
    m = hd[..., 3:4] > 0
    return np.where(m, hd[..., :3], base)


def zoom(a: np.ndarray, f: int) -> np.ndarray:
    return np.repeat(np.repeat(a, f, 0), f, 1) if f > 1 else a


class Sheet:
    def __init__(self, title: str):
        self.title = title
        self.rows: list[tuple[str, list[tuple[str, np.ndarray]]]] = []

    def row(self, label: str, cells: list[tuple[str, np.ndarray]]):
        self.rows.append((label, cells))

    def render(self) -> Image.Image:
        f, fs = font(18), font(14)
        cols = max(len(c) for _, c in self.rows)
        col_w = [0] * cols
        for _, cells in self.rows:
            for i, (cap, a) in enumerate(cells):
                col_w[i] = max(col_w[i], a.shape[1], int(fs.getlength(cap)) + 4)
        heights = [max(a.shape[0] for _, a in cells) for _, cells in self.rows]
        W = PAD + sum(w + PAD for w in col_w)
        H = 34 + sum(h + 22 + 20 + PAD for h in heights)
        img = Image.new("RGB", (W, H), BG)
        d = ImageDraw.Draw(img)
        d.text((PAD, 8), self.title, fill=(235, 220, 160), font=f)
        y = 34
        for (label, cells), h in zip(self.rows, heights):
            d.text((PAD, y), label, fill=(200, 200, 200), font=fs)
            y += 20
            x = PAD
            for i, (cap, a) in enumerate(cells):
                d.text((x, y), cap, fill=(160, 200, 255), font=fs)
                img.paste(Image.fromarray(a.astype(np.uint8)), (x, y + 20))
                x += col_w[i] + PAD
            y += h + 22 + PAD
        return img


def save(img: Image.Image, path: Path) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    img.save(path, optimize=True)
    if path.stat().st_size > MAX_BYTES:  # fall back to high-quality JPEG
        path.unlink()
        path = path.with_suffix(".jpg")
        img.save(path, quality=93, subsampling=0, optimize=True)
    print(f"{path.stat().st_size / 1e6:5.2f} MB  {path}")
    return path


def heads(packs=PACKS):
    return [("original (nearest 2x)", None)] + [(p, p) for p in packs]


def interface_sheet(asset: dict, out: Path):
    s = Sheet(f"{asset['key']} ({asset['width']}x{asset['height']}) - crops at 4x of the original pixel size (HD at 2x zoom)")
    full = {p: preview(p, asset) for _, p in heads()}
    # overview: original at 1x with crop boxes
    idx, pal = hdlib.load_indexed(hdlib.source_path(asset))
    ov = Image.fromarray(hdlib.preview_palette(pal)[idx])
    d = ImageDraw.Draw(ov)
    for k, (x, y, w, h) in enumerate(CROPS[asset["name"]]):
        d.rectangle((x, y, x + w, y + h), outline=(255, 64, 64), width=2)
        d.text((x + 3, y + 2), str(k + 1), fill=(255, 64, 64))
    ov = np.asarray(ov.resize((ov.width // 2, ov.height // 2), Image.LANCZOS))
    for k, (x, y, w, h) in enumerate(CROPS[asset["name"]]):
        cells = [(cap, zoom(full[p][2 * y:2 * (y + h), 2 * x:2 * (x + w)], 2)) for cap, p in heads()]
        if k == 0:
            cells.append(("overview (1x, half size)", ov))
        s.row(f"crop {k + 1}: x={x} y={y} {w}x{h}", cells)
    return save(s.render(), out / f"interface_{asset['name']}.png")


def grid_sheet(title: str, assets: list[dict], out: Path, name: str, z: int, per_row: int = 1):
    s = Sheet(title)
    for i in range(0, len(assets), per_row):
        group = assets[i:i + per_row]
        cells = []
        for a in group:
            for cap, p in heads():
                cells.append((f"{a['name']} {cap}", zoom(preview(p, a), z)))
        s.row(" | ".join(a["key"] for a in group), cells)
    return save(s.render(), out / name)


def map_sheet(region: dict, out: Path):
    idx, pal, _ = tilemap.assemble_indices(region)
    s = Sheet(f"{region['map']}.map elevation {region['elevation']}: rows {region['row']}-"
              f"{region['row'] + region['size'] - 1}, cols {region['col']}-{region['col'] + region['size'] - 1} "
              f"- floor tiles enhanced one by one, then assembled (2x, actual HD size)")
    orig = legacy2x(idx, pal)
    cells = {"original (nearest 2x)": orig}
    for p in PACKS:
        hd = tilemap.assemble_rgba(region, tilemap.load_pack_tiles(region, p))
        cells[p] = np.where(hd[..., 3:4] > 0, hd[..., :3], orig)
    s.row("whole patch", list(cells.items()))
    # 4x detail around the centre, where several seams meet
    h, w = orig.shape[:2]
    cy, cx = h // 2, w // 2
    crop = (slice(cy - 70, cy + 70), slice(cx - 110, cx + 110))
    s.row("centre detail (4x of the original pixel size): seams between neighbouring tiles",
          [(cap, zoom(a[crop], 2)) for cap, a in cells.items()])
    return save(s.render(), out / f"map_{region['map']}.png")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=hdlib.REPO_ROOT / "docs" / "screens" / "phase2.5")
    args = ap.parse_args()
    data = json.loads(hdlib.MANIFEST.read_text())
    assets = data["assets"]
    by = {a["name"]: a for a in assets}
    for n in CROPS:
        interface_sheet(by[n], args.out)
    grid_sheet("Main-menu buttons, 4x zoom of the original pixel size", [by["menuup"], by["menudown"]],
               args.out, "interface_buttons.png", 2, per_row=2)
    samples = [a for a in assets if a["sample"] and a["type"] == "tiles"]
    grid_sheet("Floor / roof tiles (sample set), 4x zoom of the original pixel size; "
               "slime09/shore05 cycling pixels are alpha 0 in HD (legacy colour shown)",
               samples, args.out, "tiles.png", 2, per_row=2)
    grid_sheet("Walls and scenery, 2x (actual HD size)",
               [a for a in assets if a["type"] in ("walls", "scenery")], args.out, "objects.png", 1, per_row=1)
    for region in data["regions"]:
        map_sheet(region, args.out)


if __name__ == "__main__":
    main()
