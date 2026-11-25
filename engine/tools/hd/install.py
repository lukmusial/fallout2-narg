#!/usr/bin/env python3
"""Install the generated HD packs where the engine looks them up.

  install.py [PACK ...]          # default: scripted ai
  install.py --run-dir DIR       # default: out/build/macos (HD_BUILD_DIR)

Copies assets/hd/<pack>/ to <run dir>/data/hd/<pack>/. The engine finds
hd/<pack>/art/<type>/<name>.png through its normal file layering (data/ is the
patches folder), so `[graphics] hd_pack=<pack>` then selects it. Multi-frame
art (doors, fires, containers: the enhanced frames
art/<type>/<name>/d<direction>_f<frame>.png) is packed into one
art/<type>/<name>.fhd container (src/hd_format.h). Files that are already
identical are skipped; files no longer in the pack are removed. Each pack
then gets legacy.txt (legacy_art.py): the hash of the legacy frame every image
was made from, which the engine checks before using an image.
The run directory is gitignored: the packs are derived from the game's art
and never go into git.
"""

from __future__ import annotations

import argparse
import filecmp
import json
import shutil
import struct
from pathlib import Path

import hdlib


FHD_HEADER = struct.Struct("<4sHHI")
FHD_ENTRY = struct.Struct("<II")


def fhd_bytes(directions: int, frames: int, blobs: dict[tuple[int, int], bytes]) -> bytes:
    """An FHD1 container (src/hd_format.h); missing frames get no entry."""
    table_end = FHD_HEADER.size + FHD_ENTRY.size * directions * frames
    entries = []
    data = bytearray()
    for d in range(directions):
        for f in range(frames):
            blob = blobs.get((d, f))
            if blob is None:
                entries.append((0, 0))
            else:
                entries.append((table_end + len(data), len(blob)))
                data += blob
    out = bytearray(FHD_HEADER.pack(b"FHD1", directions, frames, 0))
    for entry in entries:
        out += FHD_ENTRY.pack(*entry)
    return bytes(out + data)


def frame_sets() -> dict[str, list[dict]]:
    """Manifest assets that are frames of multi-frame art, by .fhd path."""
    if not hdlib.MANIFEST.exists():
        return {}
    sets: dict[str, list[dict]] = {}
    for asset in json.loads(hdlib.MANIFEST.read_text())["assets"]:
        if "fhd" in asset:
            sets.setdefault(asset["fhd"], []).append(asset)
    return sets


def install(pack: str, run_dir: Path) -> tuple[int, int, int]:
    src = hdlib.ASSETS_DIR / pack
    if not src.is_dir():
        raise SystemExit(f"{src} missing: run enhance_{'ai' if pack == 'ai' else 'scripted'}.py first")
    dst = run_dir / "data" / "hd" / pack
    copied = kept = removed = 0
    wanted = set()
    sets = frame_sets()
    frame_keys = {a["key"] for assets in sets.values() for a in assets}
    for path in sorted(src.rglob("*.png")):
        rel = path.relative_to(src)
        if rel.as_posix() in frame_keys:
            continue
        wanted.add(rel)
        target = dst / rel
        if target.exists() and filecmp.cmp(path, target, shallow=False):
            kept += 1
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
        copied += 1
    for fhd, assets in sorted(sets.items()):
        blobs = {}
        for asset in assets:
            frame = src / asset["key"]
            if frame.exists():
                blobs[(asset["direction"], asset["frame"])] = frame.read_bytes()
        if not blobs:
            continue
        data = fhd_bytes(assets[0]["directions"], assets[0]["frames"], blobs)
        rel = Path(fhd)
        wanted.add(rel)
        target = dst / rel
        if target.exists() and target.read_bytes() == data:
            kept += 1
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        copied += 1
    if dst.is_dir():
        for path in list(dst.rglob("*.png")) + list(dst.rglob("*.fhd")):
            rel = path.relative_to(dst)
            # ground variants are written into the installed pack (ground_variants.py)
            if rel not in wanted and rel.parts[:3] != ("art", "tiles", "variants"):
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
    # legacy.txt: the source frame of every image, so the engine skips images
    # whose art a mod has replaced (legacy_art.py, src/hd_art.h).
    import legacy_art

    legacy_art.cmd_hashes(argparse.Namespace(packs=[args.run_dir / "data/hd" / p for p in args.packs],
                                             game=hdlib.GAME_DIR, f2_res=None))


if __name__ == "__main__":
    main()
