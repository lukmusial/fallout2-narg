"""Shared helpers for the HD art pipeline (Phase 2.5).

Conventions
-----------
* Source art is read as palette-indexed PNG produced by ``ce-frm2png`` so the
  original Fallout palette indices survive.  Index 0 is transparent.
* Indices 229..254 are animated by the engine's colour cycling (slime,
  monitors, fire, shoreline, alarm).  HD output leaves them fully transparent
  so the legacy palette animation shows through the HD layer.
* Every enhancer writes ``assets/hd/<pack>/art/<type>/<name>.png`` (RGBA, exactly
  2x the source) and the shared mask ``assets/hd/masks/art/<type>/<name>.png``
  (L, 2x): 255 = HD pixel, 128 = colour-cycling (legacy shows through),
  0 = transparent (index 0).
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import numpy as np
from PIL import Image

HD_DIR = Path(__file__).resolve().parent
REPO_ROOT = HD_DIR.parent.parent
WORK_DIR = Path(os.environ.get("HD_WORK_DIR", HD_DIR / "work"))
ASSETS_DIR = Path(os.environ.get("HD_ASSETS_DIR", REPO_ROOT / "assets" / "hd"))
MODELS_DIR = Path(os.environ.get("HD_MODELS_DIR", HD_DIR / "models"))
BUILD_DIR = Path(os.environ.get("HD_BUILD_DIR", REPO_ROOT / "out" / "build" / "macos"))
GAME_DIR = Path(os.environ.get("FALLOUT2_DIR", "/Users/you/Games/Fallout 2"))

SRC_DIR = WORK_DIR / "png"          # indexed source PNGs: png/art/<type>/<name>.png
MANIFEST = WORK_DIR / "manifest.json"

CYCLE_FIRST = 229
CYCLE_LAST = 254

# First frame of the engine's colour-cycling tables (src/cycle.cc), used only
# to preview cycling pixels (color.pal stores white placeholders there).
_CYCLE_PREVIEW = (
    [(0, 108, 0), (11, 115, 7), (27, 123, 15), (43, 131, 27)]                          # 229 slime
    + [(107, 107, 111), (99, 103, 127), (87, 107, 143), (0, 147, 163), (107, 187, 255)]  # 233 monitors
    + [(255, 0, 0), (215, 0, 0), (147, 43, 11), (255, 119, 0), (255, 59, 0)]            # 238 fire slow
    + [(71, 0, 0), (123, 0, 0), (179, 0, 0), (123, 0, 0), (71, 0, 0)]                   # 243 fire fast
    + [(83, 63, 43), (75, 59, 43), (67, 55, 39), (63, 51, 39), (55, 47, 35), (51, 43, 35)]  # 248 shore
    + [(252, 0, 0)]                                                                     # 254 alarm
)


def preview_palette(palette: np.ndarray) -> np.ndarray:
    p = palette.copy()
    p[CYCLE_FIRST:CYCLE_LAST + 1] = np.array(_CYCLE_PREVIEW, dtype=np.uint8)
    return p


TILE_W = 80
TILE_H = 36


def load_indexed(path: Path) -> tuple[np.ndarray, np.ndarray]:
    """Return (indices HxW uint8, palette 256x3 uint8) from an indexed PNG."""
    image = Image.open(path)
    if image.mode != "P":
        raise ValueError(f"{path}: expected palette PNG, got {image.mode}")
    indices = np.asarray(image, dtype=np.uint8).copy()
    raw = image.getpalette() or []
    palette = np.zeros((256, 3), dtype=np.uint8)
    pal = np.asarray(raw, dtype=np.uint8).reshape(-1, 3)[:256]
    palette[: len(pal)] = pal
    return indices, palette


def masks(indices: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """(transparent, cycling) boolean masks at source resolution."""
    transparent = indices == 0
    cycling = (indices >= CYCLE_FIRST) & (indices <= CYCLE_LAST)
    return transparent, cycling


def nearest2x(a: np.ndarray) -> np.ndarray:
    return np.repeat(np.repeat(a, 2, axis=0), 2, axis=1)


def mask_image(indices: np.ndarray) -> np.ndarray:
    """2x L mask: 255 HD, 128 colour cycling, 0 transparent."""
    transparent, cycling = masks(indices)
    m = np.full(indices.shape, 255, dtype=np.uint8)
    m[cycling] = 128
    m[transparent] = 0
    return nearest2x(m)


def fill_holes(rgb: np.ndarray, holes: np.ndarray, max_iter: int = 64) -> np.ndarray:
    """Replace hole pixels with the mean of neighbouring non-hole pixels,
    growing inward, so filters do not bleed black/cycling colours across
    silhouettes.  rgb is float HxWx3."""
    out = rgb.copy()
    known = ~holes
    if known.all() or not known.any():
        return out
    out[holes] = 0.0
    for _ in range(max_iter):
        if known.all():
            break
        acc = np.zeros_like(out)
        cnt = np.zeros(out.shape[:2], dtype=np.float32)
        k = known.astype(np.float32)
        # Zero padding for the sums as for the counts: "edge" padding added
        # border pixels to the sums without counting them, so fills next to
        # an opaque border pixel grew without bound (garbage AI output for
        # sprites whose art touches the frame edge).
        padded = np.pad(out * k[..., None], ((1, 1), (1, 1), (0, 0)), mode="constant")
        padk = np.pad(k, 1, mode="constant")
        h, w = known.shape
        for dy in (0, 1, 2):
            for dx in (0, 1, 2):
                if dy == 1 and dx == 1:
                    continue
                acc += padded[dy:dy + h, dx:dx + w]
                cnt += padk[dy:dy + h, dx:dx + w]
        grow = (~known) & (cnt > 0)
        out[grow] = acc[grow] / cnt[grow, None]
        known = known | grow
    return out


def compose_rgba(rgb2x: np.ndarray, indices: np.ndarray) -> np.ndarray:
    """Combine an enhanced 2x RGB float image with the crisp 2x alpha derived
    from the source (index 0 and cycling indices -> alpha 0)."""
    m = mask_image(indices)
    rgba = np.zeros(rgb2x.shape[:2] + (4,), dtype=np.uint8)
    rgba[..., :3] = np.clip(np.rint(rgb2x), 0, 255).astype(np.uint8)
    rgba[..., 3] = np.where(m == 255, 255, 0).astype(np.uint8)
    rgba[m != 255, :3] = 0
    return rgba


def load_manifest() -> list[dict]:
    if not MANIFEST.exists():
        raise SystemExit(f"{MANIFEST} missing: run extract.py first")
    return json.loads(MANIFEST.read_text())["assets"]


def select(assets: list[dict], names: list[str] | None) -> list[dict]:
    if not names:
        return assets
    wanted = {n.lower() for n in names}
    return [a for a in assets if a["name"] in wanted or a["key"] in wanted]


def source_path(asset: dict) -> Path:
    return SRC_DIR / asset["key"]


def out_path(pack: str, asset: dict) -> Path:
    return ASSETS_DIR / pack / asset["key"]


def save_outputs(pack: str, asset: dict, rgba: np.ndarray, indices: np.ndarray) -> Path:
    dst = out_path(pack, asset)
    dst.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(rgba, "RGBA").save(dst, optimize=False)
    mdst = ASSETS_DIR / "masks" / asset["key"]
    mdst.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(mask_image(indices), "L").save(mdst)
    return dst


def is_tile(asset: dict) -> bool:
    return asset["type"] == "tiles"


# --- isometric floor-tile lattice -------------------------------------------
# From src/tile.cc squareTileToScreenXY: one column step moves (+48, -12) on
# screen (columns run right-to-left), one row step moves (+32, +24).
COL_STEP = (48, -12)
ROW_STEP = (32, 24)


def lattice_offsets(radius: int = 1):
    """Screen offsets of the neighbouring lattice positions around a tile."""
    for r in range(-radius, radius + 1):
        for c in range(-radius, radius + 1):
            if r == 0 and c == 0:
                continue
            yield (c * COL_STEP[0] + r * ROW_STEP[0], c * COL_STEP[1] + r * ROW_STEP[1])


def wrap_pad_tile(indices: np.ndarray, pad: int,
                  context: dict[tuple[int, int], np.ndarray] | None = None) -> tuple[np.ndarray, np.ndarray]:
    """Surround a floor tile by copies of itself on the isometric lattice so
    edge pixels see a plausible continuation; `context` gives the real
    neighbour at some lattice offsets instead (tile_context).  Returns
    (padded indices, 'own' mask marking pixels that belong to the centre
    tile)."""
    h, w = indices.shape
    canvas = np.zeros((h + 2 * pad, w + 2 * pad), dtype=np.uint8)
    own = np.zeros_like(canvas, dtype=bool)
    opaque = indices != 0
    for dx, dy in lattice_offsets(2):
        x0, y0 = pad + dx, pad + dy
        other = (context or {}).get((dx, dy))
        if other is not None and other.shape == indices.shape:
            _blit(canvas, other, other != 0, x0, y0)
        else:
            _blit(canvas, indices, opaque, x0, y0)
    _blit(canvas, indices, opaque, pad, pad)
    own[pad:pad + h, pad:pad + w] = opaque
    return canvas, own


# Phase 2.17: tiles that are pieces of one larger picture (the Gecko reactor,
# temple mosaics, building outlines) always have the same neighbours on the
# maps. Padding them with copies of themselves gave thin seams across the
# picture; extract.py --all-tiles writes, per tile, the neighbour each
# lattice offset has on (nearly) every placement, and the enhancers pad with
# those instead.
NEIGHBOURS = WORK_DIR / "tile_neighbours.json"
_neighbours: dict | None = None


def tile_context(name: str) -> dict[tuple[int, int], np.ndarray]:
    """{screen offset: neighbour indices} of a tile's fixed neighbours."""
    global _neighbours
    if _neighbours is None:
        _neighbours = json.loads(NEIGHBOURS.read_text()) if NEIGHBOURS.exists() else {}
    out = {}
    for key, other in _neighbours.get(name, {}).items():
        dr, dc = (int(v) for v in key.split(","))
        # map column +1 is one lattice column to the left on screen
        offset = (-dc * COL_STEP[0] + dr * ROW_STEP[0], -dc * COL_STEP[1] + dr * ROW_STEP[1])
        path = SRC_DIR / "art" / "tiles" / f"{other}.png"
        if path.exists():
            out[offset] = load_indexed(path)[0]
    return out


def _blit(canvas, src, opaque, x0, y0):
    h, w = src.shape
    H, W = canvas.shape
    sx0, sy0 = max(0, -x0), max(0, -y0)
    dx0, dy0 = max(0, x0), max(0, y0)
    cw = min(w - sx0, W - dx0)
    ch = min(h - sy0, H - dy0)
    if cw <= 0 or ch <= 0:
        return
    s = src[sy0:sy0 + ch, sx0:sx0 + cw]
    o = opaque[sy0:sy0 + ch, sx0:sx0 + cw]
    d = canvas[dy0:dy0 + ch, dx0:dx0 + cw]
    d[o] = s[o]
