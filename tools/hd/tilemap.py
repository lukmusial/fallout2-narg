#!/usr/bin/env python3
"""Assemble floor-tile patches of real map regions (1x and HD) and measure
seam quality.

Placement follows src/tile.cc squareTileToScreenXY (column index runs right
to left on screen; +48,-12 per column, +32,+24 per row) and the draw order
of tileRenderFloorsInRect (row-major, increasing tile number).  Index 0 is
transparent at 1x; at 2x the RGBA alpha is binary.

  tilemap.py                # print seam metrics for every manifest region
"""

from __future__ import annotations

import json

import numpy as np
from PIL import Image

import hdlib


def layout(region: dict) -> list[tuple[str, int, int]]:
    """[(tile name, x, y)] in draw order, positions normalised to >= 0."""
    items = []
    n = region["size"]
    for r in range(n):
        for c in range(n):
            row = region["row"] + r
            col = region["col"] + c
            tc = 99 - col
            x = 48 * tc + 32 * row
            y = -12 * tc + 24 * row
            items.append((region["tiles"][r][c], x, y))
    mx = min(x for _, x, _ in items)
    my = min(y for _, _, y in items)
    return [(t, x - mx, y - my) for t, x, y in items]


def canvas_size(items) -> tuple[int, int]:
    return (max(x for _, x, _ in items) + hdlib.TILE_W, max(y for _, _, y in items) + hdlib.TILE_H)


def assemble_indices(region: dict) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """1x composite: (indices, palette, owner map with the draw slot of the
    tile that owns each pixel, -1 = empty)."""
    items = layout(region)
    w, h = canvas_size(items)
    canvas = np.zeros((h, w), np.uint8)
    owner = np.full((h, w), -1, np.int32)
    palette = None
    for k, (name, x, y) in enumerate(items):
        idx, palette = hdlib.load_indexed(hdlib.SRC_DIR / f"art/tiles/{name}.png")
        m = idx != 0
        sub = canvas[y:y + idx.shape[0], x:x + idx.shape[1]]
        sub[m] = idx[m]
        owner[y:y + idx.shape[0], x:x + idx.shape[1]][m] = k
    return canvas, palette, owner


def assemble_rgba(region: dict, tiles: dict[str, np.ndarray]) -> np.ndarray:
    """2x composite from per-tile RGBA arrays (binary alpha)."""
    items = layout(region)
    w, h = canvas_size(items)
    canvas = np.zeros((2 * h, 2 * w, 4), np.uint8)
    for name, x, y in items:
        t = tiles[name]
        m = t[..., 3] > 0
        sub = canvas[2 * y:2 * y + t.shape[0], 2 * x:2 * x + t.shape[1]]
        sub[m] = t[m]
    return canvas


def load_pack_tiles(region: dict, pack: str) -> dict[str, np.ndarray]:
    names = {t for row in region["tiles"] for t in row}
    return {n: np.asarray(Image.open(hdlib.ASSETS_DIR / pack / f"art/tiles/{n}.png").convert("RGBA"))
            for n in names}


def seam_masks(owner: np.ndarray, band: int = 2) -> tuple[np.ndarray, np.ndarray]:
    """(seam, interior) masks at 2x.  Seam = within `band` px of a boundary
    between two different tile slots; both restricted to pixels whose
    neighbourhood is fully covered so the patch border is excluded."""
    o2 = hdlib.nearest2x(owner)
    covered = o2 >= 0
    boundary = np.zeros_like(covered)
    boundary[:, 1:] |= o2[:, 1:] != o2[:, :-1]
    boundary[:, :-1] |= o2[:, 1:] != o2[:, :-1]
    boundary[1:] |= o2[1:] != o2[:-1]
    boundary[:-1] |= o2[1:] != o2[:-1]
    boundary &= covered

    def dilate(m, r):
        out = m.copy()
        for dy in range(-r, r + 1):
            for dx in range(-r, r + 1):
                out |= np.roll(np.roll(m, dy, 0), dx, 1)
        return out

    inner = ~dilate(~covered, 10)
    seam = dilate(boundary, band) & inner
    interior = inner & ~dilate(boundary, 6)
    return seam, interior


def seam_metrics(hd: np.ndarray, ref: np.ndarray, owner: np.ndarray) -> dict:
    """Compare a per-tile assembled patch against `ref` (the same enhancer run
    on the whole 1x patch at once, i.e. with true neighbours).  Also reports
    the cross-seam gradient ratio of `hd` itself (1.0 = seams invisible)."""
    seam, interior = seam_masks(owner)
    a = hd[..., :3].astype(np.float32)
    r = ref[..., :3].astype(np.float32)
    err = np.abs(a - r).mean(-1)
    gy = np.abs(np.diff(a, axis=0, append=a[-1:])).mean(-1)
    gx = np.abs(np.diff(a, axis=1, append=a[:, -1:])).mean(-1)
    grad = gx + gy
    return {
        "seam_err": float(err[seam].mean()),
        "interior_err": float(err[interior].mean()),
        "seam_grad_ratio": float(grad[seam].mean() / max(grad[interior].mean(), 1e-6)),
    }


def reference(region: dict, enhance) -> tuple[np.ndarray, np.ndarray]:
    """Run enhance(indices, palette) on the whole 1x composite."""
    idx, pal, owner = assemble_indices(region)
    big = enhance(idx, pal)
    return hdlib.compose_rgba(big, idx), owner


def evaluate(region: dict, pack: str, enhance) -> dict:
    """Seam metrics of the pack's per-tile results for one region.
    seam_excess: extra mean abs error (0..255) on seams over the interior vs.
    the whole-patch reference; grad_excess: cross-seam gradient ratio of the
    per-tile patch divided by that of the reference (1.0 = no added seams)."""
    ref, owner = reference(region, enhance)
    hd = assemble_rgba(region, load_pack_tiles(region, pack))
    m = seam_metrics(hd, ref, owner)
    r = seam_metrics(ref, ref, owner)
    m["seam_excess"] = m["seam_err"] - m["interior_err"]
    m["grad_excess"] = m["seam_grad_ratio"] / r["seam_grad_ratio"]
    return m


def enhancers() -> dict:
    """pack -> enhance(indices, palette) for the whole-patch reference."""
    import enhance_scripted
    out = {"scripted": lambda i, p: enhance_scripted.enhance_indices(i, p)}
    try:
        import enhance_ai
        net, scale = enhance_ai.load_model(enhance_ai.DEFAULT_MODEL)
        out["ai"] = lambda i, p: enhance_ai.enhance_indices(net, scale, i, p, detail=0.6)
    except (ImportError, SystemExit):
        pass
    return out


def main() -> None:
    data = json.loads(hdlib.MANIFEST.read_text())
    for pack, fn in enhancers().items():
        for region in data["regions"]:
            m = evaluate(region, pack, fn)
            print(f"{pack:9} {region['map']:9}", json.dumps({k: round(v, 3) for k, v in m.items()}))


if __name__ == "__main__":
    main()
