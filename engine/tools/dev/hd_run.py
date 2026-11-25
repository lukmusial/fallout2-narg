#!/usr/bin/env python3
"""One hidden game run (--dev-script) in a throw-away run directory with a
chosen set of HD packs, for reviews and timings (Phase 2.22).

  hd_run.py <name> <script> [--packs ai,rpu,...] [--slot N] [--keep-cache] [[section]key=value ...]

The run directory (out/build/macos/hdrun/<name>) is an APFS clone of the app
next to links to the game data, links to the packs listed (default: every
pack in out/build/macos/data/hd), a copy of fallout2.cfg and fresh reference
saves. Outputs (screenshots, dumps, debug.log) go to its devout/ folder,
whose path is printed. The game always runs hidden; nothing takes focus.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUN_DIR = ROOT / "out/build/macos"
APP = "Fallout II Community Edition.app"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("name")
    ap.add_argument("script")
    ap.add_argument("--packs", help="comma-separated packs to link (default: all)")
    ap.add_argument("--slot", type=int, help="--dev-load-game slot")
    ap.add_argument("--keep-cache", action="store_true", help="keep data/hd/cache from an earlier run of this name")
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--slow", action="store_true", help="no --dev-fast")
    ap.add_argument("overrides", nargs="*")
    args = ap.parse_args()

    work = RUN_DIR / "hdrun" / args.name
    cache = work / "data/hd/cache"
    saved_cache = RUN_DIR / "hdrun" / f".{args.name}.cache"
    if args.keep_cache and cache.exists():
        shutil.rmtree(saved_cache, ignore_errors=True)
        cache.rename(saved_cache)
    if work.exists():
        shutil.rmtree(work)
    (work / "data/hd").mkdir(parents=True)
    subprocess.run(["cp", "-cR", str(RUN_DIR / APP), str(work / APP)], check=True)
    for name in ("master.dat", "critter.dat", "patch000.dat", "f2_res.dat", "sound", "ce.dat", "mods"):
        if (RUN_DIR / name).exists():
            (work / name).symlink_to((RUN_DIR / name).resolve())
    packs = [p.name for p in (RUN_DIR / "data/hd").iterdir() if p.is_dir() and p.name != "cache"]
    if args.packs is not None:
        packs = [p for p in args.packs.split(",") if p]
    for pack in packs:
        (work / "data/hd" / pack).symlink_to((RUN_DIR / "data/hd" / pack).resolve())
    if args.keep_cache and saved_cache.exists():
        saved_cache.rename(cache)
    shutil.copyfile(RUN_DIR / "fallout2.cfg", work / "fallout2.cfg")
    shutil.copytree(ROOT / "testdata/SAVEGAME", work / "data/SAVEGAME")

    out = work / "devout"
    out.mkdir()
    script = Path(args.script)
    if not script.is_absolute() and not script.exists():
        script = ROOT / "tests/scripts" / args.script
    cmd = [str(work / APP / "Contents/MacOS/Fallout II Community Edition"), f"--dev-script={script.resolve()}",
           f"--dev-out={out}"]
    if not args.slow:
        cmd.append("--dev-fast")
    if args.slot is not None:
        cmd.append(f"--dev-load-game={args.slot}")
    cmd += [o if o.startswith("--") else o for o in args.overrides]
    started = time.time()
    result = subprocess.run(cmd, cwd=work, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                            timeout=args.timeout)
    seconds = time.time() - started
    if (work / "debug.log").exists():
        shutil.copyfile(work / "debug.log", out / "debug.log")
    print(f"{out}  exit {result.returncode}  {seconds:.1f}s")
    if result.returncode != 0:
        print(result.stderr.strip()[-2000:], file=sys.stderr)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
