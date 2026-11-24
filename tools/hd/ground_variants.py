#!/usr/bin/env python3
"""Ground variants (Phase 2.14): 8 variants of every desert ground tile (edg*)
with real ground detail, so the ground stops repeating.

1. An exemplar texture of cracked dry earth is generated once, from a text
   prompt only, with the local SDXL model (no game art goes in):
   work/ground/exemplar_cracked.png (`--exemplar` regenerates it).
2. For each tile in a pack, every variant takes the fine detail (cracks,
   grain, relief) of a different crop of the exemplar and lays it over the
   tile's own colours. Rough tiles (rocks, gravel) get less than smooth ones.
   The detail fades out over the outer ~13 px of the diamond, so every variant
   joins every neighbour exactly like the original tile.
3. Output: <pack>/art/tiles/variants/<name>/<k>.png; the engine picks one per
   map square (`[graphics] hd_ground_variants`, src/tile.cc).

  ground_variants.py [--packs ai scripted] [--exemplar] [--count 8]
"""

import argparse
from pathlib import Path

import numpy as np
from PIL import Image
from scipy.ndimage import distance_transform_edt, gaussian_filter

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
WORK = HERE / "work" / "ground"
EXEMPLAR = WORK / "exemplar_cracked.png"
PACKS_DIR = ROOT / "out" / "build" / "macos" / "data" / "hd"
PROMPT = ("seamless top-down orthographic photo of dry cracked mud ground in a desert, irregular crack network, "
          "dusty, small stones, even light, high detail texture")
NEGATIVE = "shadows of objects, people, plants, perspective, horizon, blurry, text, watermark"

SCALE = 0.45     # exemplar 1024 px -> 461 px: crack cells the size of a few hexes
AMOUNT = 1.0     # detail strength (option 2 of the proposal)
EDGE_KEEP = 3    # px of the diamond edge kept exactly
EDGE_FADE = 10   # px over which the detail fades in


def make_exemplar():
    import torch
    from diffusers import AutoencoderKL, StableDiffusionXLPipeline
    vae = AutoencoderKL.from_pretrained("madebyollin/sdxl-vae-fp16-fix", torch_dtype=torch.float16)
    pipe = StableDiffusionXLPipeline.from_pretrained("stabilityai/stable-diffusion-xl-base-1.0", vae=vae,
                                                     torch_dtype=torch.float16, variant="fp16").to("mps")
    image = pipe(prompt=PROMPT, negative_prompt=NEGATIVE, num_inference_steps=30, guidance_scale=6.5,
                 width=1024, height=1024, generator=torch.Generator("cpu").manual_seed(21)).images[0]
    WORK.mkdir(parents=True, exist_ok=True)
    image.save(EXEMPLAR)


def detail_field():
    ex = Image.open(EXEMPLAR).convert("L")
    ex = ex.resize((int(ex.width * SCALE), int(ex.height * SCALE)), Image.LANCZOS)
    e = np.asarray(ex, np.float32) / 255
    hp = e - gaussian_filter(e, 6)
    return hp / (hp.std() + 1e-6)


def tile_roughness(rgb, opaque):
    lum = rgb.mean(-1) / 255
    hp = lum - gaussian_filter(lum, 3)
    return float(hp[opaque].std())


def make_variants(tile_path: Path, out_dir: Path, field: np.ndarray, count: int, reference: float, seed: int):
    t = np.asarray(Image.open(tile_path).convert("RGBA")).astype(np.float32)
    rgb, alpha = t[..., :3], t[..., 3]
    opaque = alpha > 0
    h, w = alpha.shape
    keep = np.clip((distance_transform_edt(opaque) - EDGE_KEEP) / EDGE_FADE, 0, 1)
    amount = AMOUNT * float(np.clip(reference / max(tile_roughness(rgb, opaque), 1e-4), 0.4, 1.0))
    lum = rgb.mean(-1, keepdims=True)
    rng = np.random.default_rng(seed)
    out_dir.mkdir(parents=True, exist_ok=True)
    for k in range(count):
        y = rng.integers(0, field.shape[0] - h)
        x = rng.integers(0, field.shape[1] - w)
        d = field[y:y + h, x:x + w]
        v = rgb + amount * 255 * 0.09 * d[..., None] * keep[..., None] * (0.6 + lum / 255)
        Image.fromarray(np.dstack([np.clip(v, 0, 255), alpha]).astype(np.uint8)).save(out_dir / f"{k}.png")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--packs", nargs="+", default=["ai", "scripted"])
    ap.add_argument("--exemplar", action="store_true", help="(re)generate the exemplar with SDXL")
    ap.add_argument("--count", type=int, default=8)
    args = ap.parse_args()

    if args.exemplar or not EXEMPLAR.exists():
        make_exemplar()
    field = detail_field()

    for pack in args.packs:
        tiles_dir = PACKS_DIR / pack / "art" / "tiles"
        tiles = sorted(p for p in tiles_dir.glob("edg*.png"))
        if not tiles:
            print(f"{pack}: no edg* tiles in {tiles_dir}")
            continue
        # the smoothest common ground tile is the reference for full strength
        reference_tile = tiles_dir / "edg5000.png"
        ref = np.asarray(Image.open(reference_tile if reference_tile.exists() else tiles[0]).convert("RGBA")).astype(np.float32)
        reference = tile_roughness(ref[..., :3], ref[..., 3] > 0)
        for index, tile in enumerate(tiles):
            make_variants(tile, tiles_dir / "variants" / tile.stem, field, args.count, reference, seed=index * 7919 + 1)
        print(f"{pack}: {len(tiles)} tiles x {args.count} variants -> {tiles_dir / 'variants'}")


if __name__ == "__main__":
    main()
