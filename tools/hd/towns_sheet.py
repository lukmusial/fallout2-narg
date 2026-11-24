#!/usr/bin/env python3
"""Phase 2.17 review sheets: the other towns' floors and roofs in the engine.

Runs the game unattended (hidden window, no audio) on save slot 1 and moves
the party to a map of each town with the `gotomap` harness command. In one
tick the script captures the frame with no HD pack (legacy, pixel-doubled)
and with the mixed pack and ground variants, so critters and lights are the
same in both. Writes to docs/screens/phase2.17/:

  town_<map>.png      1:1 output-pixel crops (1280x960 game, 2x):
                      legacy | HD (mixed pack, ground variants)

  towns_sheet.py [--out DIR] [--reuse] [MAP ...]

Walls, scenery and critters of these towns have no pack art yet: they show
as doubled legacy pixels in both columns.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path

import hdlib
from screens_sheet import load, save, sheet

RUN_DIR = hdlib.BUILD_DIR
EXE = RUN_DIR / "Fallout II Community Edition.app/Contents/MacOS/Fallout II Community Edition"
WORK = RUN_DIR / "devout/phase2.17"

# map -> (label, gotomap arguments after the map name, crops (x, y, w, h) in
# logical pixels of the 640x380 map view)
TOWNS = {
    "newr1": ("New Reno: Virgin Street, asphalt and pavement", "", [(0, 0, 320, 190), (320, 190, 320, 190)]),
    "ncr1": ("NCR: downtown", "", [(0, 0, 320, 190), (320, 190, 320, 190)]),
    "vctycocl": ("Vault City: council area, lawns", "0 16072", [(0, 0, 320, 190), (320, 190, 320, 190)]),
    "vctydwtn": ("Vault City: downtown", "", [(0, 0, 320, 190), (320, 190, 320, 190)]),
    "broken1": ("Broken Hills", "", [(0, 0, 320, 190), (320, 190, 320, 190)]),
    "redment": ("Redding: entrance", "", [(0, 0, 320, 190), (320, 190, 320, 190)]),
    "modmain": ("Modoc", "", [(0, 0, 320, 190), (320, 190, 320, 190)]),
    "gecksetl": ("Gecko: settlement", "", [(0, 0, 320, 190), (320, 190, 320, 190)]),
    "geckpwpl": ("Gecko: power plant", "", [(0, 0, 320, 190), (320, 190, 320, 190)]),
    "sfchina": ("San Francisco: Chinatown", "", [(0, 0, 320, 190), (320, 190, 320, 190)]),
    "navarro": ("Navarro", "", [(0, 0, 320, 190), (320, 190, 320, 190)]),
    "mbclose": ("Military base: outside", "", [(0, 0, 320, 190), (320, 190, 320, 190)]),
    "encdet": ("Enclave: detention", "", [(0, 0, 320, 190), (320, 190, 320, 190)]),
}

SCRIPT = """until game 30000
wait 1500
gotomap {map} {where}
wait 3500
hdpack -
refresh
wait 300
fbshot legacy
hdpack mixed
refresh
wait 300
fbshot hd
dump state
quit 0
"""


def run(name: str, where: str, reuse: bool) -> Path:
    out = WORK / name
    if reuse and (out / "hd.png").exists():
        return out
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    script = out / "script.txt"
    script.write_text(SCRIPT.format(map=name, where=where))
    subprocess.run([str(hdlib.REPO_ROOT / "tools/dev/reset-saves.sh")], check=True, stdout=subprocess.DEVNULL)
    subprocess.run([str(EXE), f"--dev-script={script}", f"--dev-out={out}", "--dev-load-game=1",
                    "[graphics]hd=1", "[graphics]hd_pack=mixed", "[graphics]hd_ground_variants=1"],
                   cwd=RUN_DIR, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=180)
    if not (out / "hd.png").exists():
        raise SystemExit(f"{name}: the game wrote no screenshots")
    print(f"ran {name}")
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("maps", nargs="*", default=list(TOWNS))
    ap.add_argument("--out", type=Path, default=hdlib.REPO_ROOT / "docs/screens/phase2.17")
    ap.add_argument("--reuse", action="store_true", help="reuse game runs from an earlier invocation")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    labels = ["legacy (pixel-doubled)", "HD: mixed pack, ground variants"]
    for name in args.maps:
        label, where, crops = TOWNS[name]
        out = run(name, where, args.reuse)
        frames = [load(out / f"{kind}.png", double=False) for kind in ("legacy", "hd")]
        rows = [[f[y * 2:(y + h) * 2, x * 2:(x + w) * 2] for f in frames] for x, y, w, h in crops]
        save(sheet(f"{label} ({name}), floors and roofs, 1280x960", rows, labels), args.out / f"town_{name}.png")
    subprocess.run([str(hdlib.REPO_ROOT / "tools/dev/reset-saves.sh")], check=True, stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
