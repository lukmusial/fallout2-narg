#!/usr/bin/env python3
"""Generate placeholder HD (2x) art for engine tests from the installed game.

  make_test_hd_art.py [--force] [ART_PATH ...]

Each ART_PATH (default: the main menu backgrounds: vanilla, hi-res and the
Stimpak seven-plate mainmenu_sp.png the menu actually shows) is read from the
game archives (patch000.dat, f2_res.dat, master.dat, then ce.dat for named
indexed PNGs) with ce-dat-tool, converted to colours exactly as the engine presents them (6-bit
palette expanded by bit replication), upscaled 2x with a Lanczos filter and
written as an RGBA PNG to RUN_DIR/data/hd/test/<art path>.png, where the game
finds it through its normal file layering. Index 0 stays opaque here because
the menu background is drawn without transparency.

It also writes two packs that exercise the other lookup routes with the same
images: hd/testfhd/ (each image as a one-frame .fhd container, built with
tools/dev/fhd.py) and hd/testmixed/ (only a manifest.txt that takes the main
menu background from the "test" pack and everything else from a missing pack),
and hd/testlegacy/ (the vanilla and hi-res main menu images with a legacy.txt
that has the right source hash for the first and a wrong one for the second,
for the legacy source check, tools/hd/legacy_art.py).

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

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "hd"))
import legacy_art  # noqa: E402
import stimpak_menu_art  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
RUN_DIR = Path(os.environ.get("RUN_DIR", ROOT / "out/build/macos"))
DEFAULT_ART = ["art/intrface/mainmenu.frm", "art/intrface/hr_mainmenu.frm", "art/intrface/mainmenu_sp.png"]
ARCHIVES = ["patch000.dat", "f2_res.dat", "master.dat", "ce.dat"]


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
    if entry.lower().endswith(".png"):
        # Named art: already an indexed PNG with the game's indices. Stimpak's
        # menu images are made at run time; stimpak_menu_art.py writes them.
        png = workdir / "in.png"
        name = entry.replace("\\", "/")
        if Path(name).name in stimpak_menu_art.NAMES:
            png.write_bytes((stimpak_menu_art.art_dir() / name).read_bytes())
        else:
            png.write_bytes(dat_cat(entry))
        image = Image.open(png)
        if image.mode != "P":
            sys.exit(f"{entry}: expected an indexed PNG, got {image.mode}")
        return np.asarray(image)
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
        fhd = RUN_DIR / "data/hd/testfhd" / Path(art.lower()).with_suffix(".fhd")
        if out.exists() and fhd.exists() and not args.force:
            continue
        entry = art.replace("/", "\\")
        with tempfile.TemporaryDirectory() as tmp:
            indices = frm_indices(entry, Path(tmp))
        rgb = colors[indices]
        image = Image.fromarray(rgb, "RGB")
        image = image.resize((image.width * 2, image.height * 2), Image.LANCZOS).convert("RGBA")
        out.parent.mkdir(parents=True, exist_ok=True)
        image.save(out)
        print(f"wrote {out}", flush=True)

        fhd.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([sys.executable, str(ROOT / "tools/dev/fhd.py"), "build", str(fhd), str(out)], check=True)

    legacy = RUN_DIR / "data/hd/testlegacy/legacy.txt"
    if not legacy.exists() or args.force:
        lines = ["# Test legacy sources (tools/dev/make_test_hd_art.py): right for", "# mainmenu, wrong for hr_mainmenu"]
        for art, right in (("art/intrface/mainmenu.frm", True), ("art/intrface/hr_mainmenu.frm", False)):
            with tempfile.TemporaryDirectory() as tmp:
                indices = frm_indices(art.replace("/", "\\"), Path(tmp))
            height, width = indices.shape
            value = legacy_art.frame_hash(width, height, np.ascontiguousarray(indices, dtype=np.uint8).tobytes())
            lines.append(f"{art} 0 0 {value if right else value ^ 1:016x}")
            image = RUN_DIR / "data/hd/test" / Path(art).with_suffix(".png")
            target = legacy.parent / Path(art).with_suffix(".png")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(image.read_bytes())
        legacy.write_text("\n".join(lines) + "\n")
        print(f"wrote {legacy}")

    manifest = RUN_DIR / "data/hd/testmixed/manifest.txt"
    if not manifest.exists() or args.force:
        manifest.parent.mkdir(parents=True, exist_ok=True)
        manifest.write_text("# Test manifest (tools/dev/make_test_hd_art.py)\n"
                            "* = missing-pack\n"
                            "art/intrface/mainmenu.frm = test\n")
        print(f"wrote {manifest}")


if __name__ == "__main__":
    main()
