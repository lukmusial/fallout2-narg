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
    project: bool = False       # lay the exemplar onto the isometric floor plane
    seamless: bool = False      # edges: a field periodic on the floor lattice instead of no detail


MATERIALS = {
    # Phase 2.14, "option 2" of the proposal
    "desert": Material(("edg", "edgs"), "exemplar_cracked.png",
                       "seamless top-down orthographic photo of dry cracked mud ground in a desert, irregular crack "
                       "network, dusty, small stones, even light, high detail texture", 21, "edg5000",
                       approved=True),
    # Phase 2.17 (proposal, not approved yet): streets and other natural
    # ground; laid onto the isometric floor plane. "<material>_<option>" are
    # the alternatives shown next to them.
    "asphalt": Material(("road",), "exemplar_asphalt.png",
                        "old weathered asphalt road surface, dark grey, visible coarse gravel aggregate, a few long "
                        "thin cracks, oil stains, worn and dusty" + STYLE, 3, "road02", amount=0.7, project=True,
                        seamless=True),
    "asphalt_cracked": Material(("road",), "exemplar_asphalt_cracked.png",
                                "old weathered asphalt road surface, coarse aggregate, network of hairline cracks and "
                                "a few patched cracks, worn and dusty" + STYLE, 31, "road02", amount=0.8,
                                project=True, seamless=True),
    # Pavement slabs: the tiles' own joints run along the diamond edges, so
    # the detail fades out there (as desert ground) instead of a seamless field.
    "concrete": Material(("curb", "cmt"), "exemplar_concrete.png",
                         "cracked weathered concrete slab surface, irregular branching cracks, no seams, pitted and "
                         "stained" + STYLE, 3, "curb16", project=True),
    "concrete_light": Material(("curb", "cmt"), "exemplar_concrete.png",
                               "cracked weathered concrete slab surface, irregular branching cracks, no seams, pitted "
                               "and stained" + STYLE, 3, "curb16", amount=0.55, project=True),
    "cave": Material(("cav",), "exemplar_cave.png",
                     "dark packed earth cave floor, fine grit and dust, a few small scattered rocks, shallow "
                     "cracks" + STYLE, 3, "cav1000", amount=0.5, project=True, seamless=True),
    "grass": Material(("grass",), "exemplar_grass.png",
                      "short dry lawn grass, dense blades, a few bare patches" + STYLE, 71, "grass01",
                      scale=0.35, amount=0.8, project=True, seamless=True),
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


# One floor square on screen at 2x: the lattice steps of src/tile.cc
# (column (48, -12), row (32, 24)) doubled.
FLOOR_A = np.array([96.0, -24.0])
FLOOR_B = np.array([64.0, 48.0])


def project_to_floor(e: np.ndarray, scale: float) -> np.ndarray:
    """The exemplar (seen straight from above) as it looks on the game's
    isometric floor: features foreshortened and turned like the ground
    plane. `scale` keeps the screen area per exemplar the same as the plain
    resize. The result is the largest axis-aligned rectangle inside the
    projected exemplar."""
    from scipy.ndimage import affine_transform
    cell = np.sqrt(abs(FLOOR_A[0] * FLOOR_B[1] - FLOOR_A[1] * FLOOR_B[0]))   # screen px per square, as a side
    s = cell / scale                                   # exemplar px per square side
    m = np.column_stack([FLOOR_A, FLOOR_B]) / s        # plane (u, v) -> screen (x, y)
    corners = np.array([m @ c for c in ((0, 0), (e.shape[1], 0), (0, e.shape[0]), (e.shape[1], e.shape[0]))])
    lo, hi = corners.min(0), corners.max(0)
    w, h = (hi - lo).astype(int)
    inv = np.linalg.inv(m)
    # scipy works in (row, col): output (y, x) -> input (v, u)
    matrix = np.array([[inv[1, 1], inv[1, 0]], [inv[0, 1], inv[0, 0]]])
    offset = np.array([inv[1] @ lo, inv[0] @ lo])
    out = affine_transform(e, matrix, offset=offset, output_shape=(h, w), order=1, cval=np.nan)
    valid = ~np.isnan(out)
    best, box = 0, None
    cy = h // 2
    for half in range(20, h // 2):
        cols = valid[cy - half:cy + half].all(0)
        run, start, longest = 0, 0, (0, 0)
        for x, ok in enumerate(cols):
            run = run + 1 if ok else 0
            if run > longest[0]:
                longest = (run, x - run + 1)
        if longest[0] * 2 * half > best:
            best, box = longest[0] * 2 * half, (cy - half, cy + half, longest[1], longest[1] + longest[0])
    y0, y1, x0, x1 = box
    return out[y0:y1, x0:x1]


def detail_field(material: Material):
    ex = Image.open(WORK / material.exemplar).convert("L")
    if material.project:
        e = project_to_floor(np.asarray(ex, np.float32) / 255, material.scale)
    else:
        ex = ex.resize((int(ex.width * material.scale), int(ex.height * material.scale)), Image.LANCZOS)
        e = np.asarray(ex, np.float32) / 255
    hp = e - gaussian_filter(e, material.sigma)
    return hp / (hp.std() + 1e-6)


def tile_roughness(rgb, opaque):
    lum = rgb.mean(-1) / 255
    hp = lum - gaussian_filter(lum, 3)
    return float(hp[opaque].std())


def lattice_field(material: Material, shape: tuple[int, int]) -> np.ndarray:
    """A detail field periodic on the floor lattice (one period per square),
    for a tile image of `shape` (2x): every tile of the material shows the
    same field along its edges, so neighbouring squares join whatever
    variants they show. Built from the exemplar on the floor plane: in each
    lattice cell four neighbouring cells of the exemplar are blended with
    weights that make the result continuous and periodic (variance kept)."""
    from scipy.ndimage import map_coordinates
    e = np.asarray(Image.open(WORK / material.exemplar).convert("L"), np.float32) / 255
    cell = np.sqrt(abs(FLOOR_A[0] * FLOOR_B[1] - FLOOR_A[1] * FLOOR_B[0]))
    s = cell / material.scale                        # exemplar px per square side
    hp = e - gaussian_filter(e, material.sigma * s / cell)
    hp /= hp.std() + 1e-6
    h, w = shape
    y, x = np.mgrid[0:h, 0:w].astype(np.float64)
    inv = np.linalg.inv(np.column_stack([FLOOR_A, FLOOR_B]))
    u = inv[0, 0] * x + inv[0, 1] * y
    v = inv[1, 0] * x + inv[1, 1] * y
    fu, fv = u - np.floor(u), v - np.floor(v)
    base = (e.shape[0] - 2 * s) / 2
    out = np.zeros(shape)
    for du, wu in ((0, fu), (1, 1 - fu)):
        for dv, wv in ((0, fv), (1, 1 - fv)):
            coords = [base + (fv + dv) * s, base + (fu + du) * s]
            out += np.sqrt(wu * wv) * map_coordinates(hp, coords, order=1, mode="reflect")
    return out.astype(np.float32)


def make_variants(tile_path: Path, out_dir: Path, field: np.ndarray, count: int, reference: float, seed: int,
                  strength: float = 1.0, periodic: np.ndarray | None = None):
    t = np.asarray(Image.open(tile_path).convert("RGBA")).astype(np.float32)
    rgb, alpha = t[..., :3], t[..., 3]
    opaque = alpha > 0
    h, w = alpha.shape
    if periodic is not None and periodic.shape == (h, w):
        keep = np.clip(distance_transform_edt(opaque) / EDGE_FADE, 0, 1)
    else:
        periodic = None
        keep = np.clip((distance_transform_edt(opaque) - EDGE_KEEP) / EDGE_FADE, 0, 1)
    amount = strength * float(np.clip(reference / max(tile_roughness(rgb, opaque), 1e-4), 0.4, 1.0))
    lum = rgb.mean(-1, keepdims=True)
    rng = np.random.default_rng(seed)
    out_dir.mkdir(parents=True, exist_ok=True)
    for k in range(count):
        y = rng.integers(0, field.shape[0] - h)
        x = rng.integers(0, field.shape[1] - w)
        d = field[y:y + h, x:x + w]
        if periodic is None:
            d = d * keep
        else:
            d = d * keep + periodic * np.sqrt(1 - keep * keep)
        v = rgb + amount * 255 * 0.09 * d[..., None] * (0.6 + lum / 255)
        Image.fromarray(np.dstack([np.clip(v, 0, 255), alpha]).astype(np.uint8)).save(out_dir / f"{k}.png")


def material_tiles(tiles_dir: Path, material: Material) -> list[Path]:
    return sorted(p for p in tiles_dir.glob("*.png") if tile_family(p.stem) in material.families)


def generate(material: Material, pack: str, out_root: Path | None = None, count: int = 8,
             field: np.ndarray | None = None) -> int:
    """Variants of every tile of the material in a pack, into
    <pack>/art/tiles/variants (or out_root/<name>); returns the tile count."""
    tiles_dir = PACKS_DIR / pack / "art" / "tiles"
    tiles = material_tiles(tiles_dir, material)
    if not tiles:
        return 0
    if field is None:
        field = detail_field(material)
    # a smooth common tile of the material is the reference for full strength
    reference_tile = tiles_dir / f"{material.reference}.png"
    ref = np.asarray(Image.open(reference_tile if reference_tile.exists() else tiles[0])
                     .convert("RGBA")).astype(np.float32)
    reference = tile_roughness(ref[..., :3], ref[..., 3] > 0)
    periodic = lattice_field(material, (2 * 36, 2 * 80)) if material.seamless else None
    for index, tile in enumerate(tiles):
        make_variants(tile, (out_root or tiles_dir / "variants") / tile.stem, field, count, reference,
                      seed=index * 7919 + 1, strength=material.amount, periodic=periodic)
    return len(tiles)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--packs", nargs="+", default=["ai", "scripted"])
    ap.add_argument("--materials", nargs="+", choices=list(MATERIALS),
                    default=[name for name, m in MATERIALS.items() if m.approved],
                    help="(one per family: options of a material overwrite each other)")
    ap.add_argument("--exemplar", action="store_true", help="(re)generate the exemplars with SDXL")
    ap.add_argument("--count", type=int, default=8)
    ap.add_argument("--out", type=Path, help="write the variants to OUT/<pack>/<name>/<k>.png instead of the pack")
    args = ap.parse_args()

    for name in args.materials:
        material = MATERIALS[name]
        if args.exemplar or not (WORK / material.exemplar).exists():
            make_exemplar(material)
        field = detail_field(material)
        for pack in args.packs:
            out = args.out / pack if args.out else None
            n = generate(material, pack, out, args.count, field)
            print(f"{pack} {name}: {n} {'/'.join(material.families)} tiles x {args.count} variants -> "
                  f"{out or PACKS_DIR / pack / 'art/tiles/variants'}")


if __name__ == "__main__":
    main()
