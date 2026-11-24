#!/usr/bin/env python3
"""Generate main-menu art with one extra button plate (for the Stimpak button).

Both inputs are palette-indexed PNGs as written by `ce-frm2png --opaque`; pixel
indices are copied, never re-quantised, so the output uses the game palette exactly.

  menu_art.py --vanilla mainmenu.png --hires hr_menu_bg.png --out-dir DIR [--tint-plate N]

Outputs:
  DIR/mainmenu_sp.png   640x480 background with seven plates (vanilla layout)
  DIR/hr_menu_bg_sp.png button panel with seven plates (hi-res layout)
  DIR/opbase_sp.png     options-style panel with eight slots (Stimpak hub), when
                        --opbase is given

--tint-plate N recolours plate N (0-based, after extension) towards a medical
green, for the "distinct plate" mock-up variant.
"""

import argparse

import numpy as np
from PIL import Image

# Vanilla mainmenu.frm: plates are 35 rows tall on a 41-row pitch starting at y=15,
# spanning roughly x=16..210. Copying the 41-row band that ends with the last
# plate's shadow one pitch further down adds a seventh plate.
VANILLA_PITCH = 41
VANILLA_LAST_BAND = (214, 257)  # rows: gap above the EXIT plate .. its shadow
VANILLA_X = (14, 212)

# HR_MENU_BG.FRM (191x242): opaque plates separated by transparent rows; the last
# plate spans rows 205..241. A new plate follows after the same 5-row gap.
HIRES_LAST_PLATE = (205, 242)
HIRES_GAP = 5

TINT_RGB = np.array([70, 140, 90])

# OPBASE.FRM (164x217): five 34-row button slots on a 37-row pitch starting at
# y=17. The fork's opbase_help.png adds a slot by repeating one 37-row band; the
# Stimpak hub needs eight (a header slot and seven buttons).
OPBASE_FIRST_SLOT = 17
OPBASE_PITCH = 37
OPBASE_SLOTS = 5
STIMPAK_HUB_SLOTS = 8


def load_indexed(path):
    image = Image.open(path)
    if image.mode != "P":
        raise SystemExit(f"{path}: expected a palette-indexed PNG")
    palette = np.array(image.getpalette()[:768]).reshape(-1, 3)
    return np.array(image), palette, image.getpalette()


def save_indexed(pixels, palette_list, path):
    image = Image.fromarray(pixels.astype(np.uint8), mode="P")
    image.putpalette(palette_list)
    image.save(path)


def nearest_index(palette, rgb, allowed):
    distances = ((palette[allowed] - rgb) ** 2).sum(axis=1)
    return allowed[int(np.argmin(distances))]


def tint(pixels, palette, rows, cols):
    # Blend each pixel's colour towards TINT_RGB keeping its brightness, then map back
    # to the nearest palette entry (excluding transparent 0 and colour-cycling 229..255).
    allowed = np.arange(1, 229)
    cache = {}
    region = pixels[rows[0]:rows[1], cols[0]:cols[1]]
    for y in range(region.shape[0]):
        for x in range(region.shape[1]):
            index = int(region[y, x])
            if index == 0:
                continue
            if index not in cache:
                rgb = palette[index].astype(float)
                luminance = rgb.mean() / 128.0
                target = 0.45 * rgb + 0.55 * TINT_RGB * luminance
                cache[index] = nearest_index(palette, target, allowed)
            region[y, x] = cache[index]


def extend_vanilla(pixels):
    top, bottom = VANILLA_LAST_BAND
    x0, x1 = VANILLA_X
    out = pixels.copy()
    out[top + VANILLA_PITCH:bottom + VANILLA_PITCH, x0:x1] = pixels[top:bottom, x0:x1]
    return out


def extend_hires(pixels):
    top, bottom = HIRES_LAST_PLATE
    plate = pixels[top:bottom]
    gap = np.zeros((HIRES_GAP, pixels.shape[1]), dtype=pixels.dtype)
    return np.vstack([pixels, gap, plate])


def extend_opbase(pixels):
    end = OPBASE_FIRST_SLOT + OPBASE_SLOTS * OPBASE_PITCH
    band = pixels[end - OPBASE_PITCH:end]
    extra = [band] * (STIMPAK_HUB_SLOTS - OPBASE_SLOTS)
    return np.vstack([pixels[:end], *extra, pixels[end:]])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--vanilla", required=True)
    parser.add_argument("--hires", required=True)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--tint-plate", type=int, default=-1)
    parser.add_argument("--opbase")
    args = parser.parse_args()

    vanilla, vanilla_palette, vanilla_palette_list = load_indexed(args.vanilla)
    vanilla = extend_vanilla(vanilla)

    hires, hires_palette, hires_palette_list = load_indexed(args.hires)
    hires = extend_hires(hires)

    if args.tint_plate >= 0:
        n = args.tint_plate
        tint(vanilla, vanilla_palette, (15 + n * VANILLA_PITCH, 15 + n * VANILLA_PITCH + 35), (16, 210))
        plate_top = [0, 40, 81, 123, 164, 205, 247][n]
        tint(hires, hires_palette, (plate_top, plate_top + 37), (0, hires.shape[1]))

    save_indexed(vanilla, vanilla_palette_list, f"{args.out_dir}/mainmenu_sp.png")
    save_indexed(hires, hires_palette_list, f"{args.out_dir}/hr_menu_bg_sp.png")

    if args.opbase:
        opbase, _, opbase_palette_list = load_indexed(args.opbase)
        save_indexed(extend_opbase(opbase), opbase_palette_list, f"{args.out_dir}/opbase_sp.png")


if __name__ == "__main__":
    main()
