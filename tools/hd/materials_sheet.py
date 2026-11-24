#!/usr/bin/env python3
"""Floor and roof review sheets from real map regions (Phase 2.17), drawn
offline from the map grids (tilemap.py layout), so any town can be shown
without running the game.

  materials_sheet.py packs                  # legacy 2x / scripted / ai of
                                            # roofs, metal, carpet, grids
  materials_sheet.py families               # material classification sheet
  materials_sheet.py proposal asphalt asphalt_cracked [--maps ...]
                                            # current HD floor vs each option
  materials_sheet.py exemplars              # the SDXL exemplars

Output: docs/screens/phase2.17/.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

import hdlib
import tilemap

OUT = hdlib.REPO_ROOT / "docs" / "screens" / "phase2.17"
PACKS_DIR = hdlib.BUILD_DIR / "data" / "hd"
BG = (24, 24, 24)

# (label, map, families, layer): roofs and regular floors where the AI
# upscale may blob thin regular patterns; the window where the families cover
# the most squares is shown.
PACK_REGIONS = [
    ("Vault City roofs", "vctycocl", ("vcra", "vcrb", "vcrc", "vcrd", "adb"), "roof"),
    ("New Reno roofs (planks)", "newr1", ("tp", "levl"), "roof"),
    ("Military base roofs", "mbclose", ("mbrf", "yrf"), "roof"),
    ("Navarro roofs", "navarro", ("vrof", "nava", "navb"), "roof"),
    ("Redding roofs", "redment", ("ruf", "wroof", "yrf"), "roof"),
    ("San Francisco roofs", "sfchina", ("grof",), "roof"),
    ("Enclave floor (grid plates)", "encdet", ("encl",), "floor"),
    ("Enclave (carpet)", "encpres", ("carpet",), "floor"),
    ("Tanker (ship floor, grates)", "sftanker", ("shipflr", "grt"), "floor"),
    ("Military base floor", "mbase12", ("mstrflr",), "floor"),
    ("Sierra depot (metal plates)", "depolva", ("plt", "stlflr"), "floor"),
    ("Vault City floor", "vctyvlt", ("rst",), "floor"),
    ("Gecko power plant", "geckpwpl", ("geka", "gekb", "brn"), "floor"),
    ("Redding (carpet)", "reddown", ("carpet",), "floor"),
    ("Enclave dock", "encdock", ("enfl",), "floor"),
]


# Floor families without variants (Phase 2.17 classification): regular or
# patterned floors stay regular; pictures and features must not change.
NO_VARIANTS = {
    **dict.fromkeys(("tp", "crbm"), "regular: wood planks/boards"),
    **dict.fromkeys(("brick", "cabrik"), "regular: brick"),
    **dict.fromkeys(("plt", "shipflr", "stlflr", "grt", "encl", "enfl"), "regular: metal plates/grids"),
    **dict.fromkeys(("rst", "mstrflr", "tepflr"), "regular: floor tiles/slabs"),
    "carpet": "regular: carpet",
    "cav": "cave floor (no variants: little effect)",
    "towr": "regular: New Reno building floors",
    **dict.fromkeys(("geka", "gekb", "tpla", "tplb", "head", "brn"), "picture / plates"),
    **dict.fromkeys(("slime", "shore"), "colour cycling"),
    **dict.fromkeys(("bld", "shut"), "black / unused"),
    "gar": "structure: farm furrows",
    **dict.fromkeys(("mnta", "mntb", "mntc", "mntd"), "rock relief (mountains)"),
    **dict.fromkeys(("brda", "brdb", "hola", "foot", "saus", "moda", "aft", "arfl", "ment", "cavb", "seas", "run_",
                     "yrf"), "ground with features (later?)"),
}


def family(name: str) -> str:
    return re.sub(r"\d+$", "", name)


def square_hash(square: int, elevation: int) -> int:
    """src/tile.cc tileHdFloorImage: the variant a map square shows."""
    h = (square * 2654435761 ^ (elevation + 1) * 40503) & 0xFFFFFFFF
    h ^= h >> 15
    h = (h * 2246822519) & 0xFFFFFFFF
    h ^= h >> 13
    return h


class Source:
    """Tile images (2x RGBA) of one kind: "legacy" (palette, nearest 2x), a
    pack, or a pack with variants from a directory."""

    def __init__(self, kind: str, pack_dir: Path | None = None, variants: Path | None = None):
        self.kind, self.pack_dir, self.variants = kind, pack_dir, variants
        self.cache: dict = {}

    def legacy(self, name: str) -> np.ndarray:
        idx, pal = hdlib.load_indexed(hdlib.SRC_DIR / f"art/tiles/{name}.png")
        rgb = hdlib.preview_palette(pal)[idx]
        return hdlib.nearest2x(np.dstack([rgb, np.where(idx != 0, 255, 0)]).astype(np.uint8))

    def image(self, name: str, square: int, elevation: int) -> np.ndarray | None:
        key = name
        if self.variants is not None and (self.variants / name).is_dir():
            count = len(list((self.variants / name).glob("*.png")))
            key = (name, square_hash(square, elevation) % count)
        if key not in self.cache:
            if isinstance(key, tuple):
                path = self.variants / name / f"{key[1]}.png"
                self.cache[key] = np.asarray(Image.open(path).convert("RGBA"))
            elif self.kind == "legacy":
                self.cache[key] = self.legacy(name)
            else:
                path = self.pack_dir / "art" / "tiles" / f"{name}.png"
                self.cache[key] = (np.asarray(Image.open(path).convert("RGBA")) if path.exists()
                                   else self.legacy(name))
        return self.cache[key]


def render(region: dict, source: Source) -> np.ndarray:
    """2x RGBA of a region (tilemap.layout order); empty roof squares skipped."""
    items = tilemap.layout(region)
    w, h = tilemap.canvas_size(items)
    canvas = np.zeros((2 * h, 2 * w, 4), np.uint8)
    n = region["size"]
    for k, (name, x, y) in enumerate(items):
        if not name:
            continue
        row, col = region["row"] + k // n, region["col"] + k % n
        t = source.image(name, row * 100 + col, region["elevation"])
        m = t[..., 3] > 0
        sub = canvas[2 * y:2 * y + t.shape[0], 2 * x:2 * x + t.shape[1]]
        sub[m] = t[m]
    return canvas


def centre_crop(img: np.ndarray, width: int, height: int) -> np.ndarray:
    cy, cx = img.shape[0] // 2, img.shape[1] // 2
    return img[cy - height // 2:cy + height // 2, cx - width // 2:cx + width // 2]


def to_rgb(rgba: np.ndarray) -> Image.Image:
    a = rgba[..., 3:4].astype(np.float32) / 255
    rgb = rgba[..., :3] * a + np.array(BG) * (1 - a)
    return Image.fromarray(rgb.astype(np.uint8))


def labelled(img: Image.Image, text: str) -> Image.Image:
    out = Image.new("RGB", (img.width, img.height + 24), (20, 20, 20))
    ImageDraw.Draw(out).text((6, 5), text, fill=(235, 225, 190))
    out.paste(img, (0, 24))
    return out


def stack(images: list[Image.Image], horizontal: bool = False, gap: int = 4) -> Image.Image:
    if horizontal:
        out = Image.new("RGB", (sum(i.width for i in images) + gap * (len(images) - 1),
                                max(i.height for i in images)), (10, 10, 10))
        x = 0
        for i in images:
            out.paste(i, (x, 0))
            x += i.width + gap
        return out
    out = Image.new("RGB", (max(i.width for i in images),
                            sum(i.height for i in images) + gap * (len(images) - 1)), (10, 10, 10))
    y = 0
    for i in images:
        out.paste(i, (0, y))
        y += i.height + gap
    return out


def packs_sheet(width: int = 560, height: int = 300, size: int = 14) -> None:
    names = tilemap.tile_names()
    sources = [("legacy (doubled pixels)", Source("legacy")),
               ("scripted", Source("pack", PACKS_DIR / "scripted")),
               ("ai", Source("pack", PACKS_DIR / "ai"))]
    rows = []
    for label, m, families, layer in PACK_REGIONS:
        found = best_regions(families, size, 1, [m], layer)
        if not found:
            print(f"{label}: no {'/'.join(families)} {layer} in {m}, skipped")
            continue
        _, _, elevation, row, col = found[0]
        region = tilemap.map_region(m, elevation, row, col, size, layer, names)
        cells = [labelled(to_rgb(centre_crop(render(region, s), width, height)), f"{label}: {sl}")
                 for sl, s in sources]
        rows.append(stack(cells, horizontal=True))
    for part in range(0, len(rows), 5):
        path = OUT / f"packs_{part // 5 + 1}.png"
        stack(rows[part:part + 5]).save(path)
        print(path)


def usage() -> tuple[dict[str, int], dict[str, dict[str, int]]]:
    """Floor squares per tile over every map, and per family the maps."""
    names = tilemap.tile_names()
    use: dict[str, int] = {}
    fam_maps: dict[str, dict[str, int]] = {}
    for path in sorted((hdlib.WORK_DIR / "raw" / "maps").glob("*.map")):
        for elevation in range(3):
            grid = tilemap.map_grid(path.stem, elevation)
            if grid is None:
                continue
            values, counts = np.unique(grid, return_counts=True)
            for v, c in zip(values, counts):
                if v < 2 or v >= len(names):
                    continue
                use[names[v]] = use.get(names[v], 0) + int(c)
                f = fam_maps.setdefault(family(names[v]), {})
                f[path.stem] = f.get(path.stem, 0) + int(c)
    return use, fam_maps


def families_sheet(materials: dict[str, str]) -> None:
    """One row per floor family (most used first): its most used tiles."""
    use, fam_maps = usage()
    fams = sorted(fam_maps, key=lambda f: -sum(fam_maps[f].values()))
    n, w, h, label = 8, 160, 72, 230
    rows = []
    for f in fams:
        tiles = sorted((k for k in use if family(k) == f), key=lambda k: -use[k])
        row = Image.new("RGB", (label + n * w, h + 4), (36, 36, 36))
        d = ImageDraw.Draw(row)
        top = ", ".join(list(sorted(fam_maps[f], key=lambda m: -fam_maps[f][m]))[:3])
        kind = f"variants: {materials[f]}" if f in materials else NO_VARIANTS.get(f, "-")
        d.text((4, 4), f"{f}: {kind}\n{sum(fam_maps[f].values())} squares, "
                       f"{len(tiles)} tiles\n{top}", fill=(235, 225, 190))
        for i, t in enumerate(tiles[:n]):
            im = Image.fromarray(Source("legacy").legacy(t), "RGBA").resize((w, h), Image.NEAREST)
            row.paste(im, (label + i * w, 2), im)
        rows.append(row)
    for part in range(0, len(rows), 28):
        path = OUT / f"families_{part // 28 + 1}.png"
        stack(rows[part:part + 28], gap=0).save(path)
        print(path)


def best_regions(families: tuple[str, ...], size: int, count: int, maps: list[str] | None = None,
                 layer: str = "floor", exclude: tuple[str, ...] = ()) -> list[tuple]:
    """(score, map, elevation, row, col): windows where the families cover the
    most squares, one per map."""
    names = tilemap.tile_names()
    fam_of = np.array([family(n) in families and n not in exclude for n in names] + [False] * (4096 - len(names)))
    found = []
    for path in sorted((hdlib.WORK_DIR / "raw" / "maps").glob("*.map")):
        if maps and path.stem not in maps:
            continue
        best = None
        for elevation in range(3):
            grid = tilemap.map_grid(path.stem, elevation, layer)
            if grid is None:
                continue
            hit = fam_of[grid].astype(np.int32)
            s = np.pad(hit.cumsum(0).cumsum(1), ((1, 0), (1, 0)))
            for row in range(0, 100 - size, 2):
                for col in range(0, 100 - size, 2):
                    score = s[row + size, col + size] - s[row, col + size] - s[row + size, col] + s[row, col]
                    if best is None or score > best[0]:
                        best = (int(score), path.stem, elevation, row, col)
        if best and best[0] > 0:
            found.append(best)
    return sorted(found, reverse=True)[:count]


def proposal_sheet(sheet: str, materials: list[str], maps: list[str] | None, pack: str = "ai", size: int = 22,
                   width: int = 1200, height: int = 380, exclude: tuple[str, ...] = ()) -> Path:
    """Two real map regions where the first material's families cover the
    most squares: the current HD floor, then each material (option) with
    its variants (generated into work/ground/proposal/<material>)."""
    import ground_variants
    names = tilemap.tile_names()
    options = [(m, ground_variants.MATERIALS[m]) for m in materials]
    families = options[0][1].families
    for name, material in options:
        out = hdlib.WORK_DIR / "ground" / "proposal" / name / pack
        if out.exists():
            shutil.rmtree(out)
        ground_variants.generate(material, pack, out)
    panels = []
    for score, m, elevation, row, col in best_regions(families, size, 2, maps, exclude=exclude):
        region = tilemap.map_region(m, elevation, row, col, size, "floor", names)
        where = f"{m}, elevation {elevation}"
        panels.append(labelled(to_rgb(centre_crop(render(region, Source("pack", PACKS_DIR / pack)), width, height)),
                               f"current HD floor ({pack} pack): {where}"))
        for k, (name, material) in enumerate(options, 1):
            src = Source("pack", PACKS_DIR / pack, hdlib.WORK_DIR / "ground" / "proposal" / name / pack)
            panels.append(labelled(to_rgb(centre_crop(render(region, src), width, height)),
                                   f"option {k}: {name.replace('_', ' ')} variants ({material.exemplar}, "
                                   f"strength {material.amount}), 8 per tile: {where}"))
    path = OUT / f"proposal_{sheet}.png"
    stack(panels, gap=8).save(path)
    return path


def exemplars_sheet(materials: list[str]) -> Path:
    """The text-to-image exemplars (SDXL, no game art) the variants take
    their detail from."""
    import ground_variants
    cells = []
    for name in materials:
        material = ground_variants.MATERIALS[name]
        img = Image.open(ground_variants.WORK / material.exemplar).convert("RGB").resize((300, 300), Image.LANCZOS)
        cells.append(labelled(img, f"{name}: {material.exemplar}"))
    rows = [stack(cells[i:i + 4], horizontal=True) for i in range(0, len(cells), 4)]
    path = OUT / "exemplars.png"
    stack(rows).save(path)
    return path


def main() -> None:
    import ground_variants
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", choices=["packs", "families", "proposal", "exemplars"])
    ap.add_argument("materials", nargs="*", help="proposal: the material and its options (asphalt asphalt_cracked)")
    ap.add_argument("--maps", nargs="*", help="proposal: pick the regions from these maps only")
    ap.add_argument("--name", help="proposal: sheet name (default: the first material)")
    ap.add_argument("--exclude", nargs="*", default=[], help="proposal: tiles not counted when picking regions")
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    if args.what == "packs":
        packs_sheet()
    elif args.what == "families":
        materials = {f: name for name, m in ground_variants.MATERIALS.items() if m.approved
                     for f in m.families}
        families_sheet(materials)
    elif args.what == "exemplars":
        print(exemplars_sheet(args.materials or list(ground_variants.MATERIALS)))
    else:
        print(proposal_sheet(args.name or args.materials[0], args.materials, args.maps,
                             exclude=tuple(args.exclude)))


if __name__ == "__main__":
    main()
