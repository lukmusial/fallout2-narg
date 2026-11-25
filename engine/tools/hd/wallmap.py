#!/usr/bin/env python3
"""Wall runs of real maps (Phase 2.18): walls are drawn as adjacent pieces,
one FRM per hex, that must join. Assembles the walls of a map window offline
the way src/object.cc places them (objectGetRect: hex position, the art's
per-direction offsets, the object's own offset; frame 0) and measures seams
like tilemap.py: the pieces enhanced one by one against the same enhancer run
on the whole 1x composite, where every piece has its true neighbours.

  wallmap.py newr1 ncr1 vctydwtn ...     # seam metrics, a window per map
  wallmap.py --sheet DIR newr1 ...       # also legacy | scripted | ai crops
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

import numpy as np
from PIL import Image

import hdlib
import mapobjects
import tilemap

HEX_W = 200


def hex_xy(tile: int) -> tuple[int, int]:
    """src/tile.cc tileToScreenXY with the view at hex 0 (relative coordinates)."""
    col = HEX_W - 1 - tile % HEX_W
    row = tile // HEX_W
    x = 48 * (col // 2)
    y = -12 * (col // 2)
    if col & 1:
        x += 32
    return x + 16 * row, y + 12 * row


_frm_cache: dict[str, tuple] = {}


def frm_offsets(path: Path) -> tuple[tuple[int, ...], tuple[int, ...]]:
    b = path.read_bytes()
    return struct.unpack_from(">6h", b, 10), struct.unpack_from(">6h", b, 22)


def map_walls(name: str, elevation: int, kinds=(mapobjects.OBJ_TYPE_WALL,)) -> list[dict]:
    """Placed objects of `kinds` on one elevation, with their art, frame and
    1x screen rectangle (relative coordinates), in draw order."""
    raw = hdlib.WORK_DIR / "raw"
    protos = mapobjects.Protos(raw)
    lists = {k: mapobjects.read_art_list(raw / f"art/{k}/{k}.lst") for k in ("walls", "scenery", "items")}
    out = []
    for obj in mapobjects.read_map_objects(raw / "maps" / f"{name}.map", protos):
        otype = (obj["fid"] >> 24) & 0xF
        if obj["depth"] or obj["elevation"] != elevation or otype not in kinds or obj["tile"] < 0:
            continue
        art = mapobjects.fid_art_path(obj["fid"], lists)
        if not art or not (raw / art).exists():
            continue
        if art not in _frm_cache:
            directions, count, frames = mapobjects.frm_frames(raw / art)
            _frm_cache[art] = (directions, count, frames, frm_offsets(raw / art))
        directions, count, frames, (xo, yo) = _frm_cache[art]
        d = obj["rotation"] if directions > 1 else 0
        f = obj["frame"] if obj["frame"] < count else 0
        w, h, _ = frames[d][f]
        sx, sy = hex_xy(obj["tile"])
        sx += 16 + xo[obj["rotation"] % 6] + obj["x"]
        sy += 8 + yo[obj["rotation"] % 6] + obj["y"]
        stem = Path(art).stem
        key = (f"art/{art.split('/')[1]}/{stem}/d{d}_f{f}.png" if directions * count > 1
               else f"art/{art.split('/')[1]}/{stem}.png")
        out.append({"art": art, "key": key, "x": sx - w // 2, "y": sy - h + 1, "w": w, "h": h,
                    "tile": obj["tile"]})
    # objects draw by hex row, then from right to left within a row (the
    # engine's object lists); close enough for walls, which rarely overlap
    out.sort(key=lambda o: (o["tile"] // HEX_W, -(o["tile"] % HEX_W)))
    return out


def best_window(walls: list[dict], width: int, height: int) -> tuple[int, int] | None:
    """Top-left of the width x height window covering the most wall area."""
    if not walls:
        return None
    best, where = -1, None
    for o in walls:
        for cx, cy in ((o["x"], o["y"]), (o["x"] - width // 2, o["y"] - height // 2)):
            area = sum(max(0, min(cx + width, p["x"] + p["w"]) - max(cx, p["x"]))
                       * max(0, min(cy + height, p["y"] + p["h"]) - max(cy, p["y"])) for p in walls)
            if area > best:
                best, where = area, (cx, cy)
    return where


def assemble_indices(walls, x0, y0, width, height):
    canvas = np.zeros((height, width), np.uint8)
    owner = np.full((height, width), -1, np.int32)
    palette = None
    for k, o in enumerate(walls):
        idx, palette = hdlib.load_indexed(hdlib.SRC_DIR / o["key"])
        _paste(canvas, owner, idx, idx != 0, o["x"] - x0, o["y"] - y0, k)
    return canvas, palette, owner


def assemble_rgba(walls, x0, y0, width, height, pack):
    canvas = np.zeros((2 * height, 2 * width, 4), np.uint8)
    dummy = np.zeros((2 * height, 2 * width), np.int32)
    for o in walls:
        path = hdlib.ASSETS_DIR / pack / o["key"]
        t = np.asarray(Image.open(path).convert("RGBA"))
        _paste(canvas, dummy, t, t[..., 3] > 0, 2 * (o["x"] - x0), 2 * (o["y"] - y0), 0)
    return canvas


def _paste(canvas, owner, src, mask, x, y, k):
    h, w = mask.shape
    H, W = owner.shape
    sx0, sy0 = max(0, -x), max(0, -y)
    dx0, dy0 = max(0, x), max(0, y)
    cw, ch = min(w - sx0, W - dx0), min(h - sy0, H - dy0)
    if cw <= 0 or ch <= 0:
        return
    m = mask[sy0:sy0 + ch, sx0:sx0 + cw]
    canvas[dy0:dy0 + ch, dx0:dx0 + cw][m] = src[sy0:sy0 + ch, sx0:sx0 + cw][m]
    owner[dy0:dy0 + ch, dx0:dx0 + cw][m] = k


def evaluate(name: str, packs: dict, width: int = 320, height: int = 200, elevation: int = 0):
    walls = map_walls(name, elevation)
    where = best_window(walls, width, height)
    if where is None:
        return None
    x0, y0 = where
    idx, pal, owner = assemble_indices(walls, x0, y0, width, height)
    result = {"map": name, "walls": len({o["art"] for o in walls}), "window": [x0, y0]}
    images = {"legacy": hdlib.nearest2x(np.dstack([hdlib.preview_palette(pal)[idx], (idx != 0) * 255])
                                        .astype(np.uint8))}
    for pack, fn in packs.items():
        ref = hdlib.compose_rgba(fn(idx, pal), idx)
        hd = assemble_rgba(walls, x0, y0, width, height, pack)
        m = tilemap.seam_metrics(hd, ref, owner)
        r = tilemap.seam_metrics(ref, ref, owner)
        result[pack] = {"seam_excess": round(m["seam_err"] - m["interior_err"], 2),
                        "grad_excess": round(m["seam_grad_ratio"] / r["seam_grad_ratio"], 3)}
        images[pack] = hd
    return result, images


def enhancers(names: list[str]) -> dict:
    """pack -> enhance(indices, palette) as the packs' objects were made."""
    out = {}
    if "scripted" in names:
        import enhance_scripted
        out["scripted"] = lambda i, p: enhance_scripted.enhance_indices(i, p)
    if "ai" in names:
        import enhance_ai
        net, scale = enhance_ai.load_model(enhance_ai.DEFAULT_MODEL)
        out["ai"] = lambda i, p: enhance_ai.enhance_indices(net, scale, i, p)
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("maps", nargs="+")
    ap.add_argument("--sheet", type=Path, help="write <map>_walls.png crops here")
    ap.add_argument("--packs", nargs="+", default=["scripted", "ai"])
    args = ap.parse_args()
    packs = enhancers(args.packs)
    for name in args.maps:
        out = evaluate(name, packs)
        if out is None:
            print(f"{name}: no walls")
            continue
        result, images = out
        print(json.dumps(result), flush=True)
        if args.sheet:
            args.sheet.mkdir(parents=True, exist_ok=True)
            row = [np.where(images[k][..., 3:] > 0, images[k][..., :3], 24).astype(np.uint8)
                   for k in ("legacy", *packs)]
            Image.fromarray(np.concatenate(row, axis=1)).save(args.sheet / f"{name}_walls.png")


if __name__ == "__main__":
    main()


def wall_neighbours(maps: list[str], margin: int = 16) -> dict[str, list]:
    """{wall key: [[neighbour key, dx, dy], ...]}: the walls around a wall
    piece (within `margin` px of its frame, 1x) in its most common
    placement over every map and elevation, positions relative to its
    top-left corner. Pieces of a wall run are enhanced with these real
    neighbours around them instead of transparency (hdlib.object_context),
    so their edges continue into the next piece."""
    from collections import Counter
    contexts: dict[str, Counter] = {}
    for name in maps:
        for elevation in range(3):
            try:
                walls = map_walls(name, elevation)
            except (OSError, ValueError):
                continue
            for o in walls:
                near = []
                for p in walls:
                    if p is o:
                        continue
                    if (p["x"] < o["x"] + o["w"] + margin and p["x"] + p["w"] > o["x"] - margin
                            and p["y"] < o["y"] + o["h"] + margin and p["y"] + p["h"] > o["y"] - margin):
                        near.append((p["key"], p["x"] - o["x"], p["y"] - o["y"]))
                if near:
                    contexts.setdefault(o["key"], Counter())[tuple(sorted(near))] += 1
    return {key: [list(n) for n in c.most_common(1)[0][0]] for key, c in contexts.items()}
