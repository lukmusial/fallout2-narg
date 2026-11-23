#!/usr/bin/env python3
"""Checks for the HD art pipeline.

Synthetic tests always run.  Tests on real art run when extract.py and the
enhancers have been run (otherwise they are skipped); AI seam tests also need
torch and the model weights.

  tools/hd/.venv/bin/python tools/hd/test_pipeline.py -v
"""

from __future__ import annotations

import json
import unittest

import numpy as np
from PIL import Image

import enhance_scripted
import hdlib
import tilemap

PACKS = ("scripted", "ai")

# Seam thresholds (see README): seam_excess in 0..255 levels, grad_excess as
# a ratio against the whole-patch reference.  Measured: scripted <= 4.4 /
# 1.07, AI <= 2.6 / 1.23.
SEAM_LIMITS = {"scripted": (6.0, 1.12), "ai": (4.0, 1.35)}


def synthetic(seed: int = 1) -> tuple[np.ndarray, np.ndarray]:
    rng = np.random.default_rng(seed)
    pal = rng.integers(0, 256, (256, 3), dtype=np.uint8)
    pal[0] = 0
    yy, xx = np.mgrid[0:40, 0:60]
    idx = (32 + (xx + yy) // 3 + rng.integers(0, 2, (40, 60))).astype(np.uint8)
    idx[(xx - 30) ** 2 + (yy - 20) ** 2 > 330] = 0          # transparent surround
    idx[18:22, 26:34] = 230                                  # cycling block (slime)
    idx[5, 30] = 254                                         # alarm pixel
    return idx, pal


def have_outputs() -> bool:
    if not hdlib.MANIFEST.exists():
        return False
    assets = hdlib.load_manifest()
    return all(hdlib.out_path(p, a).exists() for p in PACKS for a in assets)


class Synthetic(unittest.TestCase):
    def test_scripted_size_alpha_and_mask(self):
        idx, pal = synthetic()
        big = enhance_scripted.enhance_indices(idx, pal)
        self.assertEqual(big.shape, (80, 120, 3))
        rgba = hdlib.compose_rgba(big, idx)
        transparent, cycling = hdlib.masks(idx)
        hidden = hdlib.nearest2x(transparent | cycling)
        self.assertTrue((rgba[..., 3][hidden] == 0).all())
        self.assertTrue((rgba[..., 3][~hidden] == 255).all())
        m = hdlib.mask_image(idx)
        self.assertTrue((m[hdlib.nearest2x(cycling)] == 128).all())
        self.assertTrue((m[hdlib.nearest2x(transparent)] == 0).all())
        self.assertEqual(int((m == 128).sum()), 4 * int(cycling.sum()))

    def test_scripted_deterministic(self):
        idx, pal = synthetic(7)
        a = enhance_scripted.enhance_indices(idx, pal)
        b = enhance_scripted.enhance_indices(idx.copy(), pal.copy())
        self.assertTrue(np.array_equal(a, b))

    def test_constant_stays_constant(self):
        idx = np.full((12, 16), 77, np.uint8)
        pal = np.zeros((256, 3), np.uint8)
        pal[77] = (120, 90, 60)
        big = enhance_scripted.enhance_indices(idx, pal)
        self.assertLess(np.abs(big - np.array([120, 90, 60])).max(), 0.5)

    def test_no_bleed_from_transparent_black(self):
        # a bright sprite on index 0 (black): edge pixels must not darken
        idx = np.zeros((20, 20), np.uint8)
        idx[5:15, 5:15] = 9
        pal = np.zeros((256, 3), np.uint8)
        pal[9] = (200, 200, 200)
        big = enhance_scripted.enhance_indices(idx, pal)
        inside = hdlib.nearest2x(idx != 0)
        self.assertGreater(big[inside].min(), 195)

    def test_tile_lattice_wrap_covers_surroundings(self):
        # a fully opaque floor diamond, wrapped on the lattice, must leave no
        # holes in the band around the centre tile
        tile = None
        for name in ("edg5000", "cav4009"):
            p = hdlib.SRC_DIR / f"art/tiles/{name}.png"
            if p.exists():
                tile = hdlib.load_indexed(p)[0]
                break
        if tile is None:
            self.skipTest("no extracted tiles")
        pad = 8
        canvas, own = hdlib.wrap_pad_tile(tile, pad)
        near = own.copy()
        for dy in range(-pad, pad + 1):
            for dx in range(-pad, pad + 1):
                near |= np.roll(np.roll(own, dy, 0), dx, 1)
        self.assertEqual(int((canvas[near] == 0).sum()), 0)


@unittest.skipUnless(have_outputs(), "run extract.py, enhance_scripted.py and enhance_ai.py first")
class RealArt(unittest.TestCase):
    def test_outputs_exactly_2x_and_alpha_matches_source(self):
        for asset in hdlib.load_manifest():
            idx, _ = hdlib.load_indexed(hdlib.source_path(asset))
            transparent, cycling = hdlib.masks(idx)
            hidden = hdlib.nearest2x(transparent | cycling)
            mask = np.asarray(Image.open(hdlib.ASSETS_DIR / "masks" / asset["key"]))
            self.assertTrue(np.array_equal(mask, hdlib.mask_image(idx)), asset["key"])
            for pack in PACKS:
                with self.subTest(pack=pack, asset=asset["key"]):
                    im = Image.open(hdlib.out_path(pack, asset))
                    self.assertEqual(im.mode, "RGBA")
                    self.assertEqual(im.size, (2 * idx.shape[1], 2 * idx.shape[0]))
                    a = np.asarray(im)[..., 3]
                    self.assertTrue((a[hidden] == 0).all())
                    self.assertTrue((a[~hidden] == 255).all())

    def test_cycling_pixels_left_to_legacy(self):
        assets = [a for a in hdlib.load_manifest() if a["name"] in ("slime09", "shore05")]
        self.assertTrue(assets, "sample set should contain colour-cycling tiles")
        for asset in assets:
            idx, _ = hdlib.load_indexed(hdlib.source_path(asset))
            _, cycling = hdlib.masks(idx)
            self.assertGreater(int(cycling.sum()), 50)
            for pack in PACKS:
                a = np.asarray(Image.open(hdlib.out_path(pack, asset)))[..., 3]
                self.assertTrue((a[hdlib.nearest2x(cycling)] == 0).all(), (pack, asset["key"]))

    def test_scripted_pack_reproducible(self):
        asset = next(a for a in hdlib.load_manifest() if a["name"] == "menuup")
        idx, pal = hdlib.load_indexed(hdlib.source_path(asset))
        rgba = hdlib.compose_rgba(enhance_scripted.enhance_indices(idx, pal), idx)
        on_disk = np.asarray(Image.open(hdlib.out_path("scripted", asset)))
        self.assertTrue(np.array_equal(rgba, on_disk))

    def test_tile_seams(self):
        regions = json.loads(hdlib.MANIFEST.read_text())["regions"]
        self.assertTrue(regions)
        fns = tilemap.enhancers()
        for pack, fn in fns.items():
            limit_err, limit_grad = SEAM_LIMITS[pack]
            for region in regions:
                with self.subTest(pack=pack, map=region["map"]):
                    m = tilemap.evaluate(region, pack, fn)
                    self.assertLess(m["seam_excess"], limit_err, m)
                    self.assertLess(m["grad_excess"], limit_grad, m)
        if "ai" not in fns:
            self.skipTest("AI seam check skipped: torch or weights missing")


if __name__ == "__main__":
    unittest.main()
