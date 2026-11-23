#!/usr/bin/env python3
"""Where does the AI pack do worse than the scripted one? (mixed manifest)

For every wall, scenery and item asset of the areas (extract.py --areas
--objects), measured against the 8-bit source at 1x on its opaque pixels:

  drift   mean absolute colour error of the output box-downsampled to 1x
          (0..255): colour that moved away from the art
  edges   correlation of gradient magnitudes (1x): how much of the source's
          structure survives (1 = all; thin rails, fences and lettering
          drop first when a model blurs them into blobs)
  flicker multi-frame art: mean change between consecutive frames of the
          output, minus the change of the source (an enhancer that treats
          frames differently makes still parts shimmer)

An asset is a candidate for `= scripted` in the mixed manifest when the AI
output is clearly worse on one of these than scripted. Writes
work/pack_review.json and, with --sheet, a sheet of the worst candidates
(source pixel-doubled | scripted | ai).

  pack_review.py [--top N] [--sheet docs/screens/phase2.8/mixed_candidates.png]
                 [--art art/walls/jds1001.frm ...] [--zoom 2]
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

import hdlib
from screens_sheet import font, save

PACKS = ("scripted", "ai")
EDGE_MARGIN = 0.12      # ai edges this much below scripted
DRIFT_MARGIN = 6.0      # ai drift this many levels above scripted
FLICKER_MARGIN = 3.0    # ai flicker this many levels above scripted


def down2(a: np.ndarray) -> np.ndarray:
    h, w = a.shape[0] // 2, a.shape[1] // 2
    return a[:h * 2, :w * 2].reshape(h, 2, w, 2, -1).mean((1, 3))


def gradient(a: np.ndarray) -> np.ndarray:
    lum = a @ np.array([0.299, 0.587, 0.114])
    gx = np.zeros_like(lum)
    gy = np.zeros_like(lum)
    gx[:, 1:-1] = lum[:, 2:] - lum[:, :-2]
    gy[1:-1] = lum[2:] - lum[:-2]
    return np.hypot(gx, gy)


def metrics(asset: dict) -> dict | None:
    idx, pal = hdlib.load_indexed(hdlib.source_path(asset))
    transparent, cycling = hdlib.masks(idx)
    opaque = ~(transparent | cycling)
    # interior only: the 1-px silhouette edge is the same by construction
    inner = opaque.copy()
    inner[1:] &= opaque[:-1]
    inner[:-1] &= opaque[1:]
    inner[:, 1:] &= opaque[:, :-1]
    inner[:, :-1] &= opaque[:, 1:]
    if inner.sum() < 40:
        return None
    src = pal[idx].astype(np.float32)
    gsrc = gradient(src)[inner]
    out = {}
    for pack in PACKS:
        path = hdlib.out_path(pack, asset)
        if not path.exists():
            return None
        rgba = np.asarray(Image.open(path).convert("RGBA"), dtype=np.float32)
        small = down2(rgba[..., :3])
        out[f"drift_{pack}"] = float(np.abs(small - src)[inner].mean())
        g = gradient(small)[inner]
        out[f"edges_{pack}"] = float(np.corrcoef(gsrc, g)[0, 1]) if gsrc.std() > 0 and g.std() > 0 else 1.0
    return out


def flicker(frames: list[dict]) -> dict:
    """Mean frame-to-frame change on pixels the source keeps still."""
    frames = sorted(frames, key=lambda a: (a["direction"], a["frame"]))
    result = {}
    for pack in PACKS:
        changes = []
        for a, b in zip(frames, frames[1:]):
            if a["direction"] != b["direction"] or (a["width"], a["height"]) != (b["width"], b["height"]):
                continue
            ia, _ = hdlib.load_indexed(hdlib.source_path(a))
            ib, _ = hdlib.load_indexed(hdlib.source_path(b))
            still = hdlib.nearest2x((ia == ib) & (ia != 0))
            pa = np.asarray(Image.open(hdlib.out_path(pack, a)).convert("RGB"), dtype=np.float32)
            pb = np.asarray(Image.open(hdlib.out_path(pack, b)).convert("RGB"), dtype=np.float32)
            if still.sum() > 100:
                changes.append(float(np.abs(pa - pb)[still].mean()))
        if changes:
            result[f"flicker_{pack}"] = float(np.mean(changes))
    return result


def reasons(m: dict) -> list[str]:
    out = []
    if m.get("edges_ai", 1) < m.get("edges_scripted", 1) - EDGE_MARGIN:
        out.append(f"edges {m['edges_ai']:.2f} vs {m['edges_scripted']:.2f}")
    if m.get("drift_ai", 0) > m.get("drift_scripted", 0) + DRIFT_MARGIN:
        out.append(f"drift {m['drift_ai']:.1f} vs {m['drift_scripted']:.1f}")
    if m.get("flicker_ai", 0) > m.get("flicker_scripted", 0) + FLICKER_MARGIN:
        out.append(f"flicker {m['flicker_ai']:.1f} vs {m['flicker_scripted']:.1f}")
    return out


def make_sheet(entries: list[tuple[str, dict, dict]], path: Path, zoom: int = 1) -> None:
    f = font(14)
    cells = []
    for art, asset, m in entries:
        idx, pal = hdlib.load_indexed(hdlib.source_path(asset))
        src = hdlib.nearest2x(hdlib.preview_palette(pal)[idx])
        alpha = hdlib.nearest2x(idx != 0)
        row = [np.where(alpha[..., None], src, 40).astype(np.uint8)]
        for pack in PACKS:
            rgba = np.asarray(Image.open(hdlib.out_path(pack, asset)).convert("RGBA"))
            row.append(np.where(rgba[..., 3:] > 0, rgba[..., :3], 40).astype(np.uint8))
        row = [c.repeat(zoom, 0).repeat(zoom, 1) for c in row]
        cells.append((f"{art}: {', '.join(reasons(m)) or 'not flagged'}", row))
    width = 12 + max(sum(c.shape[1] + 12 for c in row) for _, row in cells)
    width = max(width, 700)
    height = 40 + sum(22 + max(c.shape[0] for c in row) + 12 for _, row in cells)
    canvas = Image.new("RGB", (width, height), (24, 24, 24))
    draw = ImageDraw.Draw(canvas)
    title = "Mixed-pack candidates: source (pixel-doubled) | scripted | ai" + (f", zoomed {zoom}x" if zoom > 1 else "")
    draw.text((12, 10), title, fill=(230, 230, 230), font=font(16))
    y = 40
    for label, row in cells:
        draw.text((12, y), label, fill=(200, 200, 160), font=f)
        y += 22
        x = 12
        for c in row:
            canvas.paste(Image.fromarray(c), (x, y))
            x += c.shape[1] + 12
        y += max(c.shape[0] for c in row) + 12
    save(canvas, path)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--top", type=int, default=12, help="candidates on the sheet")
    ap.add_argument("--sheet", type=Path)
    ap.add_argument("--art", action="append", default=[], help="put these art files on the sheet instead")
    ap.add_argument("--zoom", type=int, default=1)
    args = ap.parse_args()
    assets = [a for a in hdlib.load_manifest() if a["type"] in ("walls", "scenery", "items") and a.get("area")]
    by_art: dict[str, list[dict]] = {}
    for a in assets:
        art = a.get("fhd", a["key"]).rsplit(".", 1)[0] + ".frm"
        by_art.setdefault(art, []).append(a)
    results = {}
    for art, group in sorted(by_art.items()):
        ms = [m for m in (metrics(a) for a in group) if m]
        if not ms:
            continue
        m = {k: float(np.mean([x[k] for x in ms])) for k in ms[0]}
        if len(group) > 1:
            m.update(flicker(group))
        results[art] = m
    out = hdlib.WORK_DIR / "pack_review.json"
    out.write_text(json.dumps(results, indent=1))
    flagged = {art: m for art, m in results.items() if reasons(m)}
    print(f"{len(results)} art files measured, {len(flagged)} flagged")
    for key in ("edges", "drift"):
        for pack in PACKS:
            values = [m[f"{key}_{pack}"] for m in results.values()]
            print(f"  {key:6} {pack:8} median {np.median(values):6.2f}  p95 {np.percentile(values, 95):6.2f}")
    ranked = sorted(flagged.items(), key=lambda kv: (kv[1].get("edges_ai", 1) - kv[1].get("edges_scripted", 1)))
    for art, m in ranked:
        print(f"  {art}: {', '.join(reasons(m))}")
    if args.sheet:
        entries = []
        chosen = [(art, results[art]) for art in args.art if art in results] if args.art else ranked[:args.top]
        for art, m in chosen:
            group = sorted(by_art[art], key=lambda a: (a.get("direction", 0), a.get("frame", 0)))
            entries.append((art, group[0], m))
        if entries:
            make_sheet(entries, args.sheet, args.zoom)


if __name__ == "__main__":
    main()
