#!/usr/bin/env python3
"""Install the generated HD packs where the engine looks them up.

  install.py [PACK ...]          # default: scripted ai
  install.py --run-dir DIR       # default: out/build/macos (HD_BUILD_DIR)

Copies assets/hd/<pack>/ to <run dir>/data/hd/<pack>/. The engine finds
hd/<pack>/art/<type>/<name>.png through its normal file layering (data/ is the
patches folder), so `[graphics] hd_pack=<pack>` then selects it. Files that
are already identical are skipped; files no longer in the pack are removed.
The run directory is gitignored: the packs are derived from the game's art
and never go into git.
"""

from __future__ import annotations

import argparse
import filecmp
import shutil
from pathlib import Path

import hdlib


def install(pack: str, run_dir: Path) -> tuple[int, int, int]:
    src = hdlib.ASSETS_DIR / pack
    if not src.is_dir():
        raise SystemExit(f"{src} missing: run enhance_{'ai' if pack == 'ai' else 'scripted'}.py first")
    dst = run_dir / "data" / "hd" / pack
    copied = kept = removed = 0
    wanted = set()
    for path in sorted(src.rglob("*.png")):
        rel = path.relative_to(src)
        wanted.add(rel)
        target = dst / rel
        if target.exists() and filecmp.cmp(path, target, shallow=False):
            kept += 1
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
        copied += 1
    if dst.is_dir():
        for path in dst.rglob("*.png"):
            if path.relative_to(dst) not in wanted:
                path.unlink()
                removed += 1
    return copied, kept, removed


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("packs", nargs="*", default=["scripted", "ai"])
    ap.add_argument("--run-dir", type=Path, default=hdlib.BUILD_DIR)
    args = ap.parse_args()
    for pack in args.packs:
        copied, kept, removed = install(pack, args.run_dir)
        print(f"{pack}: {copied} copied, {kept} unchanged, {removed} removed -> {args.run_dir / 'data/hd' / pack}")


if __name__ == "__main__":
    main()
