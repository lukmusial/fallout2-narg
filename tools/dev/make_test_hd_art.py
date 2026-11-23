#!/usr/bin/env python3
"""Generate placeholder HD (2x) art for engine tests from the installed game.

  make_test_hd_art.py [--force] [ART_PATH ...]

Each ART_PATH (default: the main menu backgrounds, vanilla and hi-res) is read
from the game archives (patch000.dat, f2_res.dat, master.dat) with
ce-dat-tool, converted to colours exactly as the engine presents them (6-bit
palette expanded by bit replication), upscaled 2x with a Lanczos filter and
written as an RGBA PNG to RUN_DIR/data/hd/test/<art path>.png, where the game
finds it through its normal file layering. Index 0 stays opaque here because
the menu background is drawn without transparency.

This is test art only (a simple filter, no enhancement); the real packs come
from tools/hd/. Output stays in the gitignored run directory.
"""

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
RUN_DIR = Path(os.environ.get("RUN_DIR", ROOT / "out/build/macos"))
DEFAULT_ART = ["art/intrface/mainmenu.frm", "art/intrface/hr_mainmenu.frm"]
ARCHIVES = ["patch000.dat", "f2_res.dat", "master.dat"]


def dat_cat(entry):
    tool = RUN_DIR / "ce-dat-tool"
    for archive in ARCHIVES:
        result = subprocess.run([str(tool), str(RUN_DIR / archive), "cat", entry], capture_output=True)
        if result.returncode == 0 and result.stdout:
            return result.stdout
    sys.exit(f"{entry} not found in {', '.join(ARCHIVES)}")


def palette8():
    raw = np.frombuffer(dat_cat("color.pal")[:768], dtype=np.uint8).reshape(256, 3).astype(np.uint16)
    raw = np.where(raw <= 63, raw, 0)
    return ((raw << 2) | (raw >> 4)).astype(np.uint8)


def frm_indices(entry, workdir):
    frm = workdir / "in.frm"
    png = workdir / "in.png"
    frm.write_bytes(dat_cat(entry))
    pal = workdir / "color.pal"
    pal.write_bytes(dat_cat("color.pal"))
    subprocess.run([str(RUN_DIR / "ce-frm2png"), str(frm), str(png), "--palette", str(pal)], check=True,
                   stdout=subprocess.DEVNULL)
    image = Image.open(png)
    if image.mode != "P":
        sys.exit(f"{entry}: expected an indexed PNG from ce-frm2png, got {image.mode}")
    return np.asarray(image)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("art", nargs="*", default=DEFAULT_ART)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    colors = palette8()
    for art in args.art:
        out = RUN_DIR / "data/hd/test" / Path(art.lower()).with_suffix(".png")
        if out.exists() and not args.force:
            continue
        entry = art.replace("/", "\\")
        with tempfile.TemporaryDirectory() as tmp:
            indices = frm_indices(entry, Path(tmp))
        rgb = colors[indices]
        image = Image.fromarray(rgb, "RGB")
        image = image.resize((image.width * 2, image.height * 2), Image.LANCZOS).convert("RGBA")
        out.parent.mkdir(parents=True, exist_ok=True)
        image.save(out)
        print(f"wrote {out}")


if __name__ == "__main__":
    main()
