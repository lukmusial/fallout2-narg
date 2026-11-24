#!/usr/bin/env python3
"""Writes the menu images Stimpak makes at run time, for the HD art tools.

Stimpak (external/stimpak, src/stimpak_art.cc) builds mainmenu_sp.png,
hr_menu_bg_sp.png and opbase_sp.png in memory from the game's own images; they
are no longer files in ce.dat. The HD pipeline (tools/hd/extract.py) and the
test art (make_test_hd_art.py) need them as indexed PNGs, so this script makes
the same images with tools/dev/menu_art.py (same algorithm) into
out/stimpak-art/art/intrface/.

  stimpak_menu_art.py            make them if missing; print the directory
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUN_DIR = Path(os.environ.get("RUN_DIR", ROOT / "out/build/macos"))
OUT_DIR = ROOT / "out/stimpak-art"
NAMES = ["mainmenu_sp.png", "hr_menu_bg_sp.png", "opbase_sp.png"]
SOURCES = {"vanilla": "art/intrface/mainmenu.frm", "hires": "art/intrface/hr_menu_bg.frm", "opbase": "art/intrface/opbase.frm"}
ARCHIVES = ["patch000.dat", "f2_res.dat", "master.dat"]


def art_dir() -> Path:
    """The folder with art/intrface/<name> for every Stimpak menu image (made once)."""
    target = OUT_DIR / "art" / "intrface"
    if all((target / name).is_file() for name in NAMES):
        return OUT_DIR
    target.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        pngs = {}
        sources = dict(SOURCES, palette="color.pal")
        for key, entry in sources.items():
            frm = tmp / f"{key}.frm"
            for archive in ARCHIVES:
                result = subprocess.run([str(RUN_DIR / "ce-dat-tool"), str(RUN_DIR / archive), "cat", entry],
                                        capture_output=True)
                if result.returncode == 0 and result.stdout:
                    frm.write_bytes(result.stdout)
                    break
            else:
                sys.exit(f"{entry} not found in {', '.join(ARCHIVES)} (run tools/dev/setup-run-dir.sh)")
        for key in SOURCES:
            frm = tmp / f"{key}.frm"
            pngs[key] = tmp / f"{key}.png"
            subprocess.run([str(RUN_DIR / "ce-frm2png"), str(frm), str(pngs[key]), "--opaque",
                            "--palette", str(tmp / "palette.frm")], check=True, capture_output=True)
        subprocess.run([sys.executable, str(ROOT / "tools/dev/menu_art.py"), "--vanilla", str(pngs["vanilla"]),
                        "--hires", str(pngs["hires"]), "--opbase", str(pngs["opbase"]), "--out-dir", str(target)],
                       check=True)
    return OUT_DIR


if __name__ == "__main__":
    print(art_dir())
