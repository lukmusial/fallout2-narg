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

    def test_fill_holes_bounded_at_image_edges(self):
        # Opaque art touching the frame edge next to a hole: the fill must
        # stay within the colours around it (it used to grow without bound).
        rgb = np.zeros((6, 8, 3), np.float32)
        rgb[:, :3] = 200.0
        holes = np.zeros((6, 8), bool)
        holes[:, 3:] = True
        out = hdlib.fill_holes(rgb, holes)
        self.assertLessEqual(float(out.max()), 200.0 + 1e-3)
        self.assertGreaterEqual(float(out[holes].min()), 200.0 - 1e-3)

    def test_fhd_container_layout(self):
        import install
        blob = b"\x89PNG\r\n\x1a\n" + b"x" * 8
        data = install.fhd_bytes(1, 3, {(0, 0): blob, (0, 2): blob})
        magic, directions, frames, flags = install.FHD_HEADER.unpack_from(data, 0)
        self.assertEqual((magic, directions, frames, flags), (b"FHD1", 1, 3, 0))
        entries = [install.FHD_ENTRY.unpack_from(data, install.FHD_HEADER.size + i * 8) for i in range(3)]
        self.assertEqual(entries[1], (0, 0))
        self.assertEqual(data[entries[2][0]:entries[2][0] + entries[2][1]], blob)

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

    def test_area_tiles_complete(self):
        """Every floor and roof tile on the area maps has an image in both packs."""
        if not any(a.get("area") for a in hdlib.load_manifest()):
            self.skipTest("run extract.py --areas first")
        import extract
        lst = (hdlib.WORK_DIR / "raw/art/tiles/tiles.lst").read_text(encoding="latin-1").splitlines()
        names = [line.strip().lower().removesuffix(".frm") for line in lst]
        used = set()
        for path in sorted((hdlib.WORK_DIR / "raw/maps").glob("*.map")):
            if not path.stem.startswith(extract.AREA_PREFIXES):
                continue
            for grid in extract.read_map_grids(path).values():
                for word in grid:
                    floor, roof = word & 0xFFF, (word >> 16) & 0xFFF
                    if 1 <= floor < len(names):
                        used.add(names[floor])
                    if 2 <= roof < len(names):
                        used.add(names[roof])
        self.assertGreater(len(used), 900)
        for pack in PACKS:
            missing = sorted(n for n in used if not (hdlib.ASSETS_DIR / pack / f"art/tiles/{n}.png").exists())
            self.assertEqual(missing, [], pack)

    def test_area_objects_complete(self):
        # Phase 2.8: every wall, scenery and item art on the area maps and
        # in the reference saves, every frame of multi-frame art, in both packs.
        assets = [a for a in hdlib.load_manifest() if a["type"] in ("walls", "scenery", "items") and a.get("area")]
        if not assets:
            self.skipTest("extract.py --areas --objects not run")
        self.assertGreater(len({a.get("fhd", a["key"]) for a in assets}), 1200)
        self.assertTrue(any("fhd" in a for a in assets))
        for pack in PACKS:
            missing = sorted(a["key"] for a in assets if not hdlib.out_path(pack, a).exists())
            self.assertEqual(missing, [], pack)

    def test_all_tiles_complete(self):
        """Phase 2.17: every tile of tiles.lst (all maps, scripts) in both packs."""
        tiles = [a for a in hdlib.load_manifest() if a["type"] == "tiles"]
        if len(tiles) < 3000:
            self.skipTest("run extract.py --all-tiles first")
        for pack in PACKS:
            missing = sorted(a["key"] for a in tiles if not hdlib.out_path(pack, a).exists())
            self.assertEqual(missing, [], pack)

    def test_picture_tiles_seams(self):
        """Pieces of a larger picture are padded with their real neighbours:
        the Gecko reactor shows no added seams (it did with self-padding)."""
        if not hdlib.NEIGHBOURS.exists():
            self.skipTest("run extract.py --all-tiles first")
        context = hdlib.tile_context("geka020")
        self.assertEqual(len(context), 24)
        self.assertIn((-48, 12), context)        # map column +1: geka019
        region = tilemap.map_region("geckpwpl", 0, 36, 34, 6)
        m = tilemap.evaluate(region, "scripted", lambda i, p: enhance_scripted.enhance_indices(i, p))
        self.assertLess(m["grad_excess"], 1.1, m)

    def test_ground_lattice_field(self):
        """Phase 2.17: the edge field of seamless ground variants is periodic
        on the floor lattice, so neighbouring squares join whatever variant
        each shows."""
        try:
            import ground_variants
        except ImportError:
            self.skipTest("scipy missing (ground_variants.py runs in the SDXL venv)")
        material = ground_variants.MATERIALS["asphalt"]
        if not (ground_variants.WORK / material.exemplar).exists():
            self.skipTest("no exemplar")
        h, w = 200, 300
        p = ground_variants.lattice_field(material, (h, w))
        self.assertGreater(p.std(), 0.3)
        for dx, dy in ((96, -24), (64, 48), (-32, 72)):
            a = p[max(0, -dy):h - max(0, dy), max(0, -dx):w - max(0, dx)]
            b = p[max(0, dy):h - max(0, -dy), max(0, dx):w - max(0, -dx)]
            self.assertLess(float(np.abs(a - b).max()), 0.05, (dx, dy))

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
