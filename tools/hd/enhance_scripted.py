#!/usr/bin/env python3
"""Deterministic (no ML) 2x enhancer for 8-bit Fallout art.

Pipeline per asset (numpy only, bit-for-bit reproducible):
  1. palette -> RGB; transparent (index 0) and colour-cycling (229..254)
     pixels are holes, filled from neighbours so nothing bleeds across
     silhouettes.  Floor tiles are first wrapped with copies of themselves on
     the isometric lattice so edge pixels see a continuation (seamless).
  2. palette-aware de-dither: a 3x3 bilateral filter whose range sigma per
     pixel is the distance from its palette colour to the nearest other
     palette colour, i.e. it merges dither partners (neighbouring palette
     entries) but stops at real edges.
  3. edge-directed 2x: directional cubic convolution interpolation (DCCI,
     Zhou/Shen/Zeng 2012): diagonal pass then axial pass, each picking or
     blending the direction with the smaller gradient sum; anti-ringing clamp.
  4. gradient restoration at 2x: a 5x5 range-limited smoothing whose range is
     about one palette step, turning quantisation bands into ramps while
     leaving edges (multi-step jumps) alone; then a light edge-aware unsharp
     mask to keep contours crisp.
  5. alpha: crisp, nearest-neighbour 2x of the source silhouette; cycling
     pixels alpha 0 (see hdlib).

  enhance_scripted.py                 # every asset in work/manifest.json
  enhance_scripted.py mainmenu edg5000
"""

from __future__ import annotations

import argparse
import json
import time

import numpy as np

import hdlib

PACK = "scripted"
LUMA = np.array([0.299, 0.587, 0.114], dtype=np.float32)
TILE_PAD = 8


# --- helpers ----------------------------------------------------------------

def shift(a: np.ndarray, dy: int, dx: int) -> np.ndarray:
    """a shifted so result[y, x] = a[y+dy, x+dx] with edge replication."""
    h, w = a.shape[:2]
    p = max(abs(dy), abs(dx))
    pad = ((p, p), (p, p)) + ((0, 0),) * (a.ndim - 2)
    ap = np.pad(a, pad, mode="edge")
    return ap[p + dy:p + dy + h, p + dx:p + dx + w]


def box_sum(a: np.ndarray, r: int) -> np.ndarray:
    out = np.zeros_like(a)
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            out += shift(a, dy, dx)
    return out


def palette_steps(palette: np.ndarray) -> np.ndarray:
    """Per index: RGB distance to the nearest different usable palette entry."""
    p = palette.astype(np.float32)
    d = np.sqrt(((p[:, None] - p[None]) ** 2).sum(-1))
    usable = np.ones(256, bool)
    usable[0] = False
    usable[hdlib.CYCLE_FIRST:hdlib.CYCLE_LAST + 1] = False
    d[:, ~usable] = np.inf
    d[d == 0] = np.inf
    steps = d.min(1)
    steps[~np.isfinite(steps)] = 16.0
    return np.clip(steps, 6.0, 40.0).astype(np.float32)


# --- 1x: de-dither -----------------------------------------------------------

def dedither(rgb: np.ndarray, sigma: np.ndarray, valid: np.ndarray, strength: float = 0.6,
             iterations: int = 1) -> np.ndarray:
    out = rgb.copy()
    spatial = {(0, 0): 1.0}
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if (dy, dx) != (0, 0):
                spatial[(dy, dx)] = 0.6 if dy == 0 or dx == 0 else 0.35
    s2 = 2.0 * (1.1 * sigma) ** 2
    vf = valid.astype(np.float32)
    for _ in range(iterations):
        acc = np.zeros_like(out)
        wsum = np.zeros(out.shape[:2], np.float32)
        for (dy, dx), ws in spatial.items():
            n = shift(out, dy, dx)
            d2 = ((n - out) ** 2).sum(-1)
            w = ws * np.exp(-d2 / s2) * shift(vf, dy, dx)
            acc += w[..., None] * n
            wsum += w
        out = np.where(valid[..., None], acc / np.maximum(wsum, 1e-6)[..., None], out)
    return rgb + strength * (out - rgb)


# --- 2x: directional cubic convolution ---------------------------------------

def _cubic(a, b, c, d):
    return (-a + 9.0 * b + 9.0 * c - d) / 16.0


NOISE_FLOOR = 6.0  # gradient units; keeps texture/dither from steering edges


def _blend(p1, p2, g1, g2, lo, hi, k=5.0, t=1.15):
    """Choose/blend two directional estimates; gN = gradient sum along the
    direction pN interpolates (small = edge runs that way)."""
    g1 = g1 + NOISE_FLOOR
    g2 = g2 + NOISE_FLOOR
    w1 = 1.0 / g1 ** k
    w2 = 1.0 / g2 ** k
    soft = (w1[..., None] * p1 + w2[..., None] * p2) / (w1 + w2)[..., None]
    r = g1 / g2
    out = np.where((r < 1.0 / t)[..., None], p1, np.where((r > t)[..., None], p2, soft))
    return np.clip(out, lo, hi)


def dcci2x(rgb: np.ndarray, guide: np.ndarray | None = None) -> np.ndarray:
    """2x DCCI of rgb; edge directions are measured on guide (a smoother
    version of the same image) so residual dither does not create worms."""
    if guide is None:
        guide = rgb
    img = np.concatenate([rgb, guide], -1)
    h, w, _ = rgb.shape
    P = 3
    L = np.pad(img, ((P, P), (P, P), (0, 0)), mode="edge")
    Y = (L[..., 3:] * LUMA).sum(-1) / 255.0 * 32.0  # gradient scale for the k=5 weights

    # pass 1: centre of each LR 2x2 cell -> HR (2i+1, 2j+1)
    def at(a, dy, dx):
        return a[P + dy:P + dy + h, P + dx:P + dx + w]

    g45 = np.zeros((h, w), np.float32)   # along up-right diagonal
    g135 = np.zeros((h, w), np.float32)  # along down-right diagonal
    for r in range(-1, 3):
        for c in range(-1, 3):
            if r - 1 >= -1 and c + 1 <= 2:
                g45 += np.abs(at(Y, r, c) - at(Y, r - 1, c + 1))
            if r + 1 <= 2 and c + 1 <= 2:
                g135 += np.abs(at(Y, r, c) - at(Y, r + 1, c + 1))
    p135 = _cubic(at(L, -1, -1), at(L, 0, 0), at(L, 1, 1), at(L, 2, 2))
    p45 = _cubic(at(L, 2, -1), at(L, 1, 0), at(L, 0, 1), at(L, -1, 2))
    corners = np.stack([at(L, 0, 0), at(L, 0, 1), at(L, 1, 0), at(L, 1, 1)])
    centre = _blend(p45, p135, g45, g135, corners.min(0), corners.max(0))

    hr = np.full((2 * h, 2 * w, 6), np.nan, np.float32)
    hr[0::2, 0::2] = img
    hr[1::2, 1::2] = centre

    # pass 2: remaining (y+x odd) pixels from horizontal / vertical neighbours
    Q = 4
    Hp = np.pad(hr, ((Q, Q), (Q, Q), (0, 0)), mode="edge")
    # edge padding of a NaN-patterned grid breaks parity on the border rows;
    # refill border NaNs from the nearest known value.
    nanm = np.isnan(Hp[..., 0])
    if nanm.any():
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            fill = np.isnan(Hp[..., 0]) & ~np.isnan(shift(Hp, dy, dx)[..., 0])
            Hp[fill] = shift(Hp, dy, dx)[fill]
        Hp[np.isnan(Hp)] = 0.0
    HY = (Hp[..., 3:] * LUMA).sum(-1) / 255.0 * 32.0
    hh, ww = 2 * h, 2 * w

    def at2(a, dy, dx):
        return a[Q + dy:Q + dy + hh, Q + dx:Q + dx + ww]

    dh = np.abs(at2(HY, 0, -1) - at2(HY, 0, 1))
    dv = np.abs(at2(HY, -1, 0) - at2(HY, 1, 0))
    gh = np.zeros((hh, ww), np.float32)
    gv = np.zeros((hh, ww), np.float32)
    # dh/dv at odd-parity positions compare two known pixels; offsets with an
    # even dy+dx keep that parity.
    for dy in (-2, -1, 0, 1, 2):
        for dx in (-2, -1, 0, 1, 2):
            if (dy + dx) % 2 == 0:
                gh += shift(dh, dy, dx)
                gv += shift(dv, dy, dx)
    ph = _cubic(at2(Hp, 0, -3), at2(Hp, 0, -1), at2(Hp, 0, 1), at2(Hp, 0, 3))
    pv = _cubic(at2(Hp, -3, 0), at2(Hp, -1, 0), at2(Hp, 1, 0), at2(Hp, 3, 0))
    nb = np.stack([at2(Hp, 0, -1), at2(Hp, 0, 1), at2(Hp, -1, 0), at2(Hp, 1, 0)])
    axial = _blend(ph, pv, gh, gv, nb.min(0), nb.max(0))
    yy, xx = np.mgrid[0:hh, 0:ww]
    odd = ((yy + xx) % 2 == 1)
    hr[odd] = axial[odd]
    return hr[..., :3]


def bilinear2x(a: np.ndarray) -> np.ndarray:
    """Exact 2x bilinear (pixel-centre aligned: weights 3/4, 1/4)."""
    def up(x, axis):
        prev = np.concatenate([np.take(x, [0], axis), np.take(x, range(x.shape[axis] - 1), axis)], axis)
        nxt = np.concatenate([np.take(x, range(1, x.shape[axis]), axis), np.take(x, [-1], axis)], axis)
        lo = 0.75 * x + 0.25 * prev
        hi = 0.75 * x + 0.25 * nxt
        out = np.stack([lo, hi], axis + 1)
        shape = list(x.shape)
        shape[axis] *= 2
        return out.reshape(shape)
    return up(up(a, 0), 1)


# --- 2x: gradient restoration -------------------------------------------------

def restore_gradients(rgb: np.ndarray, sigma2x: np.ndarray, valid: np.ndarray) -> np.ndarray:
    s2 = 2.0 * (0.6 * sigma2x) ** 2
    vf = valid.astype(np.float32)
    acc = np.zeros_like(rgb)
    wsum = np.zeros(rgb.shape[:2], np.float32)
    for dy in range(-2, 3):
        for dx in range(-2, 3):
            ws = np.exp(-(dy * dy + dx * dx) / (2 * 1.6 ** 2))
            n = shift(rgb, dy, dx)
            d2 = ((n - rgb) ** 2).sum(-1)
            w = ws * np.exp(-d2 / s2) * shift(vf, dy, dx)
            acc += w[..., None] * n
            wsum += w
    smooth = np.where((wsum > 1e-6)[..., None], acc / np.maximum(wsum, 1e-6)[..., None], rgb)
    # edge-aware unsharp mask: sharpen only where the local contrast is large
    blur = box_sum(smooth, 1) / 9.0
    detail = smooth - blur
    edge = np.abs(((shift(smooth, 0, 1) - shift(smooth, 0, -1)) * LUMA).sum(-1)) + \
        np.abs(((shift(smooth, 1, 0) - shift(smooth, -1, 0)) * LUMA).sum(-1))
    amount = 0.45 * np.clip((edge - 12.0) / 40.0, 0.0, 1.0)
    return smooth + amount[..., None] * detail


# --- driver -------------------------------------------------------------------

def enhance_indices(indices: np.ndarray, palette: np.ndarray, tile: bool = False,
                    dedither_strength: float = 0.55) -> np.ndarray:
    """Return the enhanced 2x RGB (float) for an index image."""
    if tile:
        work, _own = hdlib.wrap_pad_tile(indices, TILE_PAD)
    else:
        work = indices
    transparent, cycling = hdlib.masks(work)
    holes = transparent | cycling
    rgb = palette[work].astype(np.float32)
    rgb = hdlib.fill_holes(rgb, holes)
    steps = palette_steps(palette)[work]
    # base: fully de-dithered, upscaled edge-directed; grain: what the
    # de-dither removed, re-added at reduced strength with a soft (bilinear)
    # upscale so leftover dither becomes fine grain instead of diagonal worms.
    base = dedither(rgb, steps, ~holes, strength=1.0, iterations=2)
    guide = dedither(base, steps, ~holes, strength=1.0, iterations=1)
    big = dcci2x(base, guide)
    big = restore_gradients(big, hdlib.nearest2x(steps), hdlib.nearest2x(~holes))
    big += (1.0 - dedither_strength) * bilinear2x(rgb - base)
    if tile:
        p = 2 * TILE_PAD
        big = big[p:p + 2 * indices.shape[0], p:p + 2 * indices.shape[1]]
    return np.clip(big, 0, 255)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help="asset names or keys (default: all in manifest)")
    args = ap.parse_args()
    assets = hdlib.select(hdlib.load_manifest(), args.names)
    timings = {}
    for asset in assets:
        t0 = time.perf_counter()
        idx, pal = hdlib.load_indexed(hdlib.source_path(asset))
        big = enhance_indices(idx, pal, tile=hdlib.is_tile(asset))
        rgba = hdlib.compose_rgba(big, idx)
        dst = hdlib.save_outputs(PACK, asset, rgba, idx)
        dt = time.perf_counter() - t0
        timings[asset["key"]] = round(dt, 3)
        print(f"{dt:7.2f}s  {dst.relative_to(hdlib.REPO_ROOT)}")
    tfile = hdlib.WORK_DIR / f"timings_{PACK}.json"
    old = json.loads(tfile.read_text()) if tfile.exists() else {}
    old.update(timings)
    tfile.write_text(json.dumps(old, indent=1))


if __name__ == "__main__":
    main()
