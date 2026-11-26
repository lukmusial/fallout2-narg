#!/usr/bin/env python3
"""Fixtures for tests/unit/hd_upscale_test.cc (Phase 2.22): a few frames of
the installed game's art and what enhance_scripted.py makes of them.

For each fixture writes tests/unit/fixtures/upscale/<name>.in.png (palette
PNG, the frame's indices and the palette) and <name>.out.png (RGBA, the
Python reference: enhance_indices + compose_rgba, no tile neighbours or wall
context, which the runtime upscale does not have either), and lists them in
fixtures.txt (`<name> <tile 0|1>`).

  make_upscale_fixtures.py            # needs ce-dat-tool and ce-frm2png built
"""

from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

import enhance_scripted
import hdlib

OUT_DIR = hdlib.REPO_ROOT / "tests" / "unit" / "fixtures" / "upscale"

# Tiles with real neighbours (the runtime upscale takes them from the map):
# name, tile, {(row delta, screen column delta): neighbour tile}.
CONTEXT_FIXTURES = [
    ("tile_ctx_edg5000", "art/tiles/edg5000.frm", {(0, 1): "art/tiles/edg5001.frm", (1, 0): "art/tiles/edg5002.frm",
                                                    (-1, -1): "art/tiles/ruf1000.frm", (1, 1): "art/tiles/edg5001.frm"}),
]

# name, art path, frame, direction, tile, crop (x, y, w, h) or None
FIXTURES = [
    ("tile_edg5000", "art/tiles/edg5000.frm", 0, 0, True, None),
    ("tile_ruf1000", "art/tiles/ruf1000.frm", 0, 0, True, None),
    # Transparent surroundings and colour-cycling screen pixels.
    ("scenery_comp5", "art/scenery/comp5.frm", 0, 0, False, None),
    ("scenery_monitr7", "art/scenery/monitr7.frm", 0, 0, False, None),
    ("scenery_firepit", "art/scenery/firepit.frm", 0, 0, False, None),
    # Interface art: the options dialog (opaque, text plates, bevels).
    ("intrface_opbase", "art/intrface/opbase.frm", 0, 0, False, None),
]


def tool(name: str) -> Path:
    path = hdlib.BUILD_DIR / name
    if not path.exists():
        raise SystemExit(f"{path} missing: cmake --build out/build/macos --target ce-dat-tool ce-frm2png")
    return path


def extract(paths: list[str], raw: Path) -> None:
    """Extracts art paths (and color.pal) from the game's archives in the
    engine's layer order (later archives win)."""
    for dat in ("master.dat", "critter.dat", "patch000.dat"):
        archive = hdlib.GAME_DIR / dat
        if not archive.exists():
            continue
        listing = subprocess.run([str(tool("ce-dat-tool")), str(archive), "list"], check=True,
                                 capture_output=True, text=True, encoding="latin-1").stdout
        entries = {line.strip().replace("\\", "/").lower(): line.strip() for line in listing.splitlines() if line.strip()}
        hits = [entries[p] for p in paths if p in entries]
        if not hits:
            continue
        listfile = raw / f".{dat}.list"
        listfile.write_text("\n".join(hits) + "\n", encoding="latin-1")
        subprocess.run([str(tool("ce-dat-tool")), str(archive), "extract", "--lower", "--file-list", str(listfile),
                        str(raw)], check=True, capture_output=True)


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        raw = Path(tmp)
        extra = [p for _, tile, ctx in CONTEXT_FIXTURES for p in [tile, *ctx.values()]]
        extract([f[1] for f in FIXTURES] + extra + ["color.pal"], raw)
        pal = raw / "color.pal"
        lines = []
        for name, path, frame, direction, tile, crop in FIXTURES:
            src = raw / path
            png = raw / f"{name}.png"
            subprocess.run([str(tool("ce-frm2png")), str(src), str(png), "--palette", str(pal), "--frame", str(frame),
                            "--direction", str(direction)], check=True, capture_output=True)
            idx, palette = hdlib.load_indexed(png)
            if crop is not None:
                x, y, w, h = crop
                idx = idx[y:y + h, x:x + w].copy()
            big = enhance_scripted.enhance_indices(idx, palette, tile=tile)
            rgba = hdlib.compose_rgba(big, idx)

            image = Image.fromarray(idx, "P")
            image.putpalette(palette.flatten().tolist())
            image.save(OUT_DIR / f"{name}.in.png")
            Image.fromarray(rgba, "RGBA").save(OUT_DIR / f"{name}.out.png")
            holes = int(((idx == 0) | ((idx >= hdlib.CYCLE_FIRST) & (idx <= hdlib.CYCLE_LAST))).sum())
            cycling = int(((idx >= hdlib.CYCLE_FIRST) & (idx <= hdlib.CYCLE_LAST)).sum())
            print(f"{name}: {idx.shape[1]}x{idx.shape[0]}, {holes} holes ({cycling} cycling)")
            lines.append(f"{name} {1 if tile else 0}")
        def indexed(path):
            png = raw / (Path(path).stem + ".ctx.png")
            subprocess.run([str(tool("ce-frm2png")), str(raw / path), str(png), "--palette", str(pal)], check=True,
                           capture_output=True)
            return hdlib.load_indexed(png)

        for name, path, ctx in CONTEXT_FIXTURES:
            idx, palette = indexed(path)
            # hdlib's context is keyed by screen offset: column step (+48,
            # -12), row step (+32, +24).
            context = {}
            for (r, c), other in ctx.items():
                neighbour, _ = indexed(other)
                context[(c * 48 + r * 32, c * -12 + r * 24)] = neighbour
                image = Image.fromarray(neighbour, "P")
                image.putpalette(palette.flatten().tolist())
                image.save(OUT_DIR / f"{name}.ctx{r + 1}{c + 1}.png")
            big = enhance_scripted.enhance_indices(idx, palette, tile=True, context=context)
            rgba = hdlib.compose_rgba(big, idx)
            image = Image.fromarray(idx, "P")
            image.putpalette(palette.flatten().tolist())
            image.save(OUT_DIR / f"{name}.in.png")
            Image.fromarray(rgba, "RGBA").save(OUT_DIR / f"{name}.out.png")
            print(f"{name}: {idx.shape[1]}x{idx.shape[0]}, {len(context)} neighbours")
            lines.append(f"{name} 1")
        (OUT_DIR / "fixtures.txt").write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
