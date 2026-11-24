#!/usr/bin/env python3
"""Repaints the palm tree of the high-resolution main menu (HR_MAINMENU.FRM).

The original is a coarse 3D render whose fronds are crumpled, blocky shapes;
the AI 2x upscale keeps them. This replaces just the palm (canopy and trunk)
of a pack's 2x image with a repaint by the local SDXL model (inpainting, no
network; option 2 of docs/screens/phase2.19_palm/, chosen by the user). The
sky, wall and armour stay as they are. Run it after install.py, which writes
the plain upscale:

  palm_repaint.py [--packs ai] [--strength 0.8] [--seed 11]
"""

import argparse
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parents[2]
PACKS = ROOT / "out" / "build" / "macos" / "data" / "hd"
ART = Path("art") / "intrface" / "hr_mainmenu.png"
BOX = (1920, 0, 2560, 768)  # the palm's corner of the 2560x1440 image
PROMPT = ("a tall palm tree with long arching feathery fronds, seen from below against a dusk sky, "
          "backlit, detailed leaflets, realistic, cinematic lighting, muted desaturated colours, post-apocalyptic")
NEGATIVE = "blurry, low poly, crumpled, blocky, cartoon, text, watermark, extra trees, people"


def palm_mask(crop: Image.Image) -> Image.Image:
    """Soft mask of the palm in the crop: not sky (blue or dusk pink), not the
    grey armour, within the canopy area and the trunk's column."""
    a = np.asarray(crop).astype(np.float32) / 255
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    lum = 0.3 * r + 0.59 * g + 0.11 * b
    sky = ((b > r * 0.95) & (b > g * 0.9) & (lum > 0.25)) | ((r > 0.45) & (lum > 0.45) & (np.abs(r - g) < 0.25))
    grey = (np.abs(r - g) < 0.04) & (np.abs(g - b) < 0.05) & (lum > 0.35)
    m = ~(sky | grey)
    yy, xx = np.mgrid[0:m.shape[0], 0:m.shape[1]]
    m &= ~((yy > 560) & (xx < 220)) & ~((yy > 520) & (xx > 560))
    image = Image.fromarray((m * 255).astype(np.uint8)).filter(ImageFilter.MedianFilter(5)).filter(ImageFilter.MaxFilter(9))
    m = np.asarray(image) > 128
    m &= ((yy < 480) & (xx > 60)) | ((xx > 470) & (xx < 600))
    m &= ~((xx < 150) & (yy > 300))
    return Image.fromarray((m * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(7)).filter(ImageFilter.GaussianBlur(5))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--packs", nargs="+", default=["ai"])
    ap.add_argument("--strength", type=float, default=0.8)
    ap.add_argument("--seed", type=int, default=11)
    args = ap.parse_args()

    import torch
    from diffusers import AutoencoderKL, StableDiffusionXLInpaintPipeline
    vae = AutoencoderKL.from_pretrained("madebyollin/sdxl-vae-fp16-fix", torch_dtype=torch.float16, local_files_only=True)
    pipe = StableDiffusionXLInpaintPipeline.from_pretrained("stabilityai/stable-diffusion-xl-base-1.0", vae=vae,
                                                            torch_dtype=torch.float16, variant="fp16",
                                                            local_files_only=True).to("mps")
    for pack in args.packs:
        path = PACKS / pack / ART
        image = Image.open(path).convert("RGBA")
        if image.size != (2560, 1440):
            raise SystemExit(f"{path}: expected 2560x1440, got {image.size}")
        crop = image.crop(BOX).convert("RGB")
        mask = palm_mask(crop)
        size = (832, 1000)
        out = pipe(prompt=PROMPT, negative_prompt=NEGATIVE, image=crop.resize(size, Image.LANCZOS),
                   mask_image=mask.resize(size), strength=args.strength, num_inference_steps=40, guidance_scale=6.5,
                   generator=torch.Generator("cpu").manual_seed(args.seed), width=size[0], height=size[1]).images[0]
        out = out.resize(crop.size, Image.LANCZOS)
        m = np.asarray(mask, np.float32)[..., None] / 255
        comp = np.asarray(crop, np.float32) * (1 - m) + np.asarray(out, np.float32) * m
        repainted = Image.fromarray(comp.clip(0, 255).astype(np.uint8)).convert("RGBA")
        # The art is opaque here; keep the pack image's own alpha.
        repainted.putalpha(image.crop(BOX).getchannel("A"))
        image.paste(repainted, BOX[:2])
        image.save(path)
        print(f"{pack}: palm repainted in {path}")


if __name__ == "__main__":
    main()
