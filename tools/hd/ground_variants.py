#!/usr/bin/env python3
"""Ground variants (Phase 2.14, more materials in 2.17): 8 variants of every
natural-ground and street tile, with real surface detail, so the ground stops
repeating.

1. Per material (MATERIALS: desert ground, asphalt, concrete, ...), an
   exemplar texture is generated once, from a text prompt only, with the local
   SDXL model (no game art goes in): work/ground/exemplar_<material>.png
   (`--exemplar` regenerates them).
2. For each tile of the material's families in a pack, every variant takes
   the fine detail (cracks, grain, relief) of a different crop of the exemplar
   and lays it over the tile's own colours. Rough tiles (rocks, gravel,
   rubble) get less than smooth ones. The detail fades out over the outer
   ~13 px of the diamond, so every variant joins every neighbour exactly like
   the original tile.
3. Output: <pack>/art/tiles/variants/<name>/<k>.png; the engine picks one per
   map square (`[graphics] hd_ground_variants`, src/tile.cc).

Indoor, regular and patterned floors (metal plates, tiles, carpet, wood,
grids, brick) get no variants: they should stay regular.

  ground_variants.py [--packs ai scripted] [--materials desert asphalt ...]
                     [--exemplar] [--count 8]
"""

import argparse
import re
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image
from scipy.ndimage import distance_transform_edt, gaussian_filter

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
WORK = HERE / "work" / "ground"
PACKS_DIR = ROOT / "out" / "build" / "macos" / "data" / "hd"
NEGATIVE = "shadows of objects, people, plants, perspective, horizon, blurry, text, watermark"
STYLE = ", seamless top-down orthographic photo, even diffuse light, realistic, high detail texture"


@dataclass(frozen=True)
class Material:
    families: tuple[str, ...]   # tile name = family + digits (edg5000, road12)
    exemplar: str               # work/ground/<exemplar>
    prompt: str
    seed: int
    reference: str              # smooth common tile: full strength
    scale: float = 0.45         # exemplar 1024 px -> 461 px
    amount: float = 1.0         # detail strength
    sigma: float = 6.0          # high-pass cut-off (exemplar px after scaling)
    approved: bool = False      # installed by default (else proposal only: --materials NAME)


MATERIALS = {
    # Phase 2.14, "option 2" of the proposal
    "desert": Material(("edg", "edgs"), "exemplar_cracked.png",
                       "seamless top-down orthographic photo of dry cracked mud ground in a desert, irregular crack "
                       "network, dusty, small stones, even light, high detail texture", 21, "edg5000",
                       approved=True),
    # Phase 2.17 (proposal): streets and other natural ground
    "asphalt": Material(("road",), "exemplar_asphalt.png",
                        "old weathered asphalt road surface, coarse aggregate, network of hairline cracks and a few "
                        "patched cracks, worn and dusty" + STYLE, 31, "road02"),
    "concrete": Material(("curb", "cmt"), "exemplar_concrete.png",
                         "old cracked concrete pavement, weathered cement surface with hairline cracks, small chips, "
                         "faint stains and dust" + STYLE, 41, "curb16"),
    "dirt": Material(("towr", "capb"), "exemplar_dirt.png",
                     "packed dry dirt ground, fine soil with footprints, small pebbles and shallow ruts, "
                     "dusty" + STYLE, 51, "towr01", amount=0.8),
    "cave": Material(("cav",), "exemplar_cave.png",
                     "rough rocky cave floor, packed earth with gravel, small rocks and fine cracks" + STYLE, 61,
                     "cav1000", amount=0.7),
    "grass": Material(("grass",), "exemplar_grass.png",
                      "short dry lawn grass, dense blades, a few bare patches" + STYLE, 71, "grass01",
                      scale=0.35, amount=0.8),
}

EDGE_KEEP = 3    # px of the diamond edge kept exactly
EDGE_FADE = 10   # px over which the detail fades in


def tile_family(name: str) -> str:
    return re.sub(r"\d+$", "", name)


_pipe = None


def make_exemplar(material: Material, path: Path | None = None, seed: int | None = None):
    """Text-to-image with the local SDXL base model (offline: nothing is
    downloaded or sent anywhere)."""
    global _pipe
    import os
    os.environ.setdefault("HF_HUB_OFFLINE", "1")
    import torch
    from diffusers import AutoencoderKL, StableDiffusionXLPipeline
    if _pipe is None:
        vae = AutoencoderKL.from_pretrained("madebyollin/sdxl-vae-fp16-fix", torch_dtype=torch.float16,
                                            local_files_only=True)
        _pipe = StableDiffusionXLPipeline.from_pretrained("stabilityai/stable-diffusion-xl-base-1.0", vae=vae,
                                                          torch_dtype=torch.float16, variant="fp16",
                                                          local_files_only=True).to("mps")
    seed = material.seed if seed is None else seed
    image = _pipe(prompt=material.prompt, negative_prompt=NEGATIVE, num_inference_steps=30, guidance_scale=6.5,
                  width=1024, height=1024, generator=torch.Generator("cpu").manual_seed(seed)).images[0]
    WORK.mkdir(parents=True, exist_ok=True)
    image.save(path or WORK / material.exemplar)


def detail_field(material: Material):
    ex = Image.open(WORK / material.exemplar).convert("L")
    ex = ex.resize((int(ex.width * material.scale), int(ex.height * material.scale)), Image.LANCZOS)
    e = np.asarray(ex, np.float32) / 255
    hp = e - gaussian_filter(e, material.sigma)
    return hp / (hp.std() + 1e-6)


def tile_roughness(rgb, opaque):
    lum = rgb.mean(-1) / 255
    hp = lum - gaussian_filter(lum, 3)
    return float(hp[opaque].std())


def make_variants(tile_path: Path, out_dir: Path, field: np.ndarray, count: int, reference: float, seed: int,
                  strength: float = 1.0):
    t = np.asarray(Image.open(tile_path).convert("RGBA")).astype(np.float32)
    rgb, alpha = t[..., :3], t[..., 3]
    opaque = alpha > 0
    h, w = alpha.shape
    keep = np.clip((distance_transform_edt(opaque) - EDGE_KEEP) / EDGE_FADE, 0, 1)
    amount = strength * float(np.clip(reference / max(tile_roughness(rgb, opaque), 1e-4), 0.4, 1.0))
    lum = rgb.mean(-1, keepdims=True)
    rng = np.random.default_rng(seed)
    out_dir.mkdir(parents=True, exist_ok=True)
    for k in range(count):
        y = rng.integers(0, field.shape[0] - h)
        x = rng.integers(0, field.shape[1] - w)
        d = field[y:y + h, x:x + w]
        v = rgb + amount * 255 * 0.09 * d[..., None] * keep[..., None] * (0.6 + lum / 255)
        Image.fromarray(np.dstack([np.clip(v, 0, 255), alpha]).astype(np.uint8)).save(out_dir / f"{k}.png")


def material_tiles(tiles_dir: Path, material: Material) -> list[Path]:
    return sorted(p for p in tiles_dir.glob("*.png") if tile_family(p.stem) in material.families)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--packs", nargs="+", default=["ai", "scripted"])
    ap.add_argument("--materials", nargs="+", choices=list(MATERIALS),
                    default=[name for name, m in MATERIALS.items() if m.approved])
    ap.add_argument("--exemplar", action="store_true", help="(re)generate the exemplars with SDXL")
    ap.add_argument("--count", type=int, default=8)
    ap.add_argument("--out", type=Path, help="write the variants to OUT/<pack>/<name>/<k>.png instead of the pack "
                                             "(proposal sheets, materials_sheet.py)")
    args = ap.parse_args()

    for name in args.materials:
        material = MATERIALS[name]
        if args.exemplar or not (WORK / material.exemplar).exists():
            make_exemplar(material)
        field = detail_field(material)
        for pack in args.packs:
            tiles_dir = PACKS_DIR / pack / "art" / "tiles"
            tiles = material_tiles(tiles_dir, material)
            if not tiles:
                print(f"{pack} {name}: no {'/'.join(material.families)} tiles in {tiles_dir}")
                continue
            # a smooth common tile of the material is the reference for full strength
            reference_tile = tiles_dir / f"{material.reference}.png"
            ref = np.asarray(Image.open(reference_tile if reference_tile.exists() else tiles[0])
                             .convert("RGBA")).astype(np.float32)
            reference = tile_roughness(ref[..., :3], ref[..., 3] > 0)
            for index, tile in enumerate(tiles):
                out = (args.out / pack if args.out else tiles_dir / "variants") / tile.stem
                make_variants(tile, out, field, args.count, reference,
                              seed=index * 7919 + 1, strength=material.amount)
            print(f"{pack} {name}: {len(tiles)} tiles x {args.count} variants -> "
                  f"{args.out / pack if args.out else tiles_dir / 'variants'}")


if __name__ == "__main__":
    main()
