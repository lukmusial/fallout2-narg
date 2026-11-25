#!/usr/bin/env python3
"""Build, list and extract .fhd HD art containers (format: src/hd_format.h).

  fhd.py build OUT.fhd [--directions N] PNG|- [PNG|- ...]
      Frames in direction-major order (all frames of direction 0, then 1, ...);
      "-" leaves a frame without an HD image (the engine uses the legacy one).
      The frame count per direction is len(PNGs) / N.
  fhd.py build OUT.fhd --from-dir DIR
      Takes DIR/d<direction>_f<frame>.png (missing ones become "-").
  fhd.py list FILE.fhd
  fhd.py extract FILE.fhd OUTDIR      writes OUTDIR/d<direction>_f<frame>.png

PNG streams are stored as given (no re-encoding), so any colour type works.
Only the standard library is needed.
"""

import argparse
import re
import struct
import sys
from pathlib import Path

MAGIC = b"FHD1"
HEADER = struct.Struct("<4sHHI")
ENTRY = struct.Struct("<II")
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def build(out, directions, blobs):
    if directions < 1 or directions > 6:
        sys.exit("directions must be 1..6")
    if not blobs or len(blobs) % directions:
        sys.exit(f"{len(blobs)} frames do not split into {directions} directions")
    frames = len(blobs) // directions
    table_end = HEADER.size + ENTRY.size * len(blobs)
    entries = []
    data = bytearray()
    for blob in blobs:
        if blob is None:
            entries.append((0, 0))
        else:
            if not blob.startswith(PNG_SIGNATURE):
                sys.exit("not a PNG stream")
            entries.append((table_end + len(data), len(blob)))
            data += blob
    with open(out, "wb") as f:
        f.write(HEADER.pack(MAGIC, directions, frames, 0))
        for entry in entries:
            f.write(ENTRY.pack(*entry))
        f.write(data)
    print(f"wrote {out}: {directions} direction(s) x {frames} frame(s), {sum(1 for b in blobs if b)} image(s)")


def read(path):
    raw = Path(path).read_bytes()
    magic, directions, frames, flags = HEADER.unpack_from(raw, 0)
    if magic != MAGIC or flags != 0:
        sys.exit(f"{path}: not an FHD1 file")
    entries = [ENTRY.unpack_from(raw, HEADER.size + i * ENTRY.size) for i in range(directions * frames)]
    return raw, directions, frames, entries


def png_size(blob):
    if blob[:8] != PNG_SIGNATURE:
        return None
    return struct.unpack(">II", blob[16:24])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    b = sub.add_parser("build")
    b.add_argument("out")
    b.add_argument("--directions", type=int, default=1)
    b.add_argument("--from-dir")
    b.add_argument("frames", nargs="*")
    l = sub.add_parser("list")
    l.add_argument("file")
    e = sub.add_parser("extract")
    e.add_argument("file")
    e.add_argument("outdir")
    args = parser.parse_args()

    if args.command == "build":
        if args.from_dir:
            found = {}
            for png in Path(args.from_dir).glob("d*_f*.png"):
                m = re.fullmatch(r"d(\d+)_f(\d+)\.png", png.name)
                if m:
                    found[(int(m.group(1)), int(m.group(2)))] = png.read_bytes()
            if not found:
                sys.exit(f"no d<direction>_f<frame>.png files in {args.from_dir}")
            directions = max(d for d, _ in found) + 1
            frames = max(f for _, f in found) + 1
            blobs = [found.get((d, f)) for d in range(directions) for f in range(frames)]
            build(args.out, directions, blobs)
        else:
            blobs = [None if name == "-" else Path(name).read_bytes() for name in args.frames]
            build(args.out, args.directions, blobs)
    elif args.command == "list":
        raw, directions, frames, entries = read(args.file)
        print(f"{args.file}: {directions} direction(s) x {frames} frame(s)")
        for i, (offset, size) in enumerate(entries):
            d, f = divmod(i, frames)
            desc = "-" if size == 0 else "%dx%d PNG, %d bytes" % (*png_size(raw[offset:offset + size]), size)
            print(f"  d{d} f{f}: {desc}")
    else:
        raw, directions, frames, entries = read(args.file)
        out = Path(args.outdir)
        out.mkdir(parents=True, exist_ok=True)
        for i, (offset, size) in enumerate(entries):
            if size:
                d, f = divmod(i, frames)
                (out / f"d{d}_f{f}.png").write_bytes(raw[offset:offset + size])


if __name__ == "__main__":
    main()
