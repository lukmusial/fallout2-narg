#!/usr/bin/env python3
"""Legacy art behind the HD packs: source hashes and mod comparisons.

HD images are looked up by art path. A content mod (e.g. the Restoration
Project) may ship a different picture under the same path; the HD image made
from the original would then be drawn over the mod's art. To prevent that, a
pack records a hash of every legacy frame its images were made from, and the
engine (src/hd_art.cc) only uses an HD image while the running game's frame
still has that hash (else the legacy frame is drawn doubled).

  legacy_art.py hashes <pack dir> [<pack dir> ...] [--game DIR] [--f2-res DAT]
      Writes <pack dir>/legacy.txt for the installed pack, from the art the
      pack was made from (the vanilla game's layering: master.dat <
      critter.dat < f2_res.dat < ce.dat < patch000.dat < data/).

  legacy_art.py compare --base DIR --mod DIR <pack dir> [--json OUT] [--md OUT]
      Resolves every art path of the pack in both installed games (the
      engine's layering, mods/mods_order.txt included) and reports, per frame,
      where the mod's picture differs from what the pack was made from, and
      which art the mod adds that has no HD image.

legacy.txt format (one frame per line, `#` comments):

  <art path> <direction> <frame> <hash>

<art path> is the engine's normalised path (lower case, forward slashes, the
legacy file's own extension: art/intrface/mainmenu.frm, art/intrface/
mainmenu_sp.png), <hash> 16 hex digits: FNV-1a 64 over width and height
(uint16 little-endian each) followed by the frame's palette indices, row by
row (hdLegacyFrameHash in src/hd_format.h). Art with one direction is listed
as direction 0.
"""

from __future__ import annotations

import argparse
import io
import json
import os
import struct
import subprocess
import sys
import tempfile
from collections import Counter, defaultdict
from pathlib import Path

HD_DIR = Path(__file__).resolve().parent
REPO_ROOT = HD_DIR.parent.parent
DEFAULT_GAME = Path(os.environ.get("FALLOUT2_DIR", Path.home() / "Games/Fallout 2"))

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = (1 << 64) - 1

# Art types the packs cover (critters, heads, misc effects stay legacy).
HD_TYPES = ("intrface", "inven", "items", "scenery", "walls", "tiles", "skilldex")


def fnv1a64(data: bytes, h: int = FNV_OFFSET) -> int:
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & MASK64
    return h


def frame_hash(width: int, height: int, pixels: bytes) -> int:
    return fnv1a64(pixels, fnv1a64(struct.pack("<HH", width, height)))


def decode_frm(data: bytes) -> dict[tuple[int, int], tuple[int, int, bytes]]:
    """{(direction, frame): (width, height, indices)} of an FRM; art whose
    directions share one data block is returned as direction 0 only."""
    if len(data) < 0x3E:
        raise ValueError("short FRM")
    frames_per_dir = struct.unpack_from(">H", data, 8)[0]
    offsets = struct.unpack_from(">6I", data, 0x22)
    result = {}
    seen = {}
    for direction in range(6):
        if direction > 0 and offsets[direction] in seen:
            continue
        seen[offsets[direction]] = direction
        pos = 0x3E + offsets[direction]
        for frame in range(frames_per_dir):
            w, h, size = struct.unpack_from(">HHI", data, pos)
            pos += 12
            result[(direction, frame)] = (w, h, bytes(data[pos:pos + w * h]))
            pos += size
    return result


def decode_png(data: bytes) -> dict[tuple[int, int], tuple[int, int, bytes]]:
    """An indexed PNG (ce.dat named art) as frame 0 of direction 0."""
    from PIL import Image

    image = Image.open(io.BytesIO(data))
    if image.mode != "P":
        raise ValueError(f"not an indexed PNG ({image.mode})")
    return {(0, 0): (image.width, image.height, image.tobytes())}


def decode_art(path: str, data: bytes):
    return decode_png(data) if path.endswith(".png") else decode_frm(data)


# --- the engine's file layering ------------------------------------------------


def ce_dat_tool() -> Path:
    for build in ("macos-release", "macos"):
        tool = REPO_ROOT / "out/build" / build / "ce-dat-tool"
        if tool.exists():
            return tool
    raise SystemExit("ce-dat-tool missing: cmake --build out/build/macos --target ce-dat-tool")


class Layer:
    """One archive or folder of the engine's file chain."""

    def __init__(self, label: str, path: Path):
        self.label = label
        self.path = path
        self.entries: dict[str, str] = {}  # normalised path -> name inside
        if path.is_dir():
            for p in path.rglob("*"):
                if p.is_file():
                    rel = p.relative_to(path).as_posix()
                    self.entries[rel.lower()] = rel
        else:
            out = subprocess.run([str(ce_dat_tool()), str(path), "list"], check=True,
                                 capture_output=True, text=True, encoding="latin-1").stdout
            for line in out.splitlines():
                line = line.strip()
                if line:
                    self.entries[line.replace("\\", "/").lower()] = line
        self._cache_dir: Path | None = None

    def read_many(self, paths: list[str]) -> dict[str, bytes]:
        paths = [p for p in paths if p in self.entries]
        if not paths:
            return {}
        if self.path.is_dir():
            return {p: (self.path / self.entries[p]).read_bytes() for p in paths}
        with tempfile.TemporaryDirectory() as tmp:
            listfile = Path(tmp) / "list.txt"
            listfile.write_text("\n".join(self.entries[p] for p in paths) + "\n", encoding="latin-1")
            out = Path(tmp) / "out"
            subprocess.run([str(ce_dat_tool()), str(self.path), "extract", "--lower",
                            "--file-list", str(listfile), str(out)], check=True, capture_output=True)
            return {p: (out / p).read_bytes() for p in paths if (out / p).exists()}


def read_ini_value(path: Path, section: str, key: str) -> str | None:
    if not path.exists():
        return None
    current = None
    for line in path.read_text(errors="replace").splitlines():
        line = line.split(";", 1)[0].strip()
        if line.startswith("[") and line.endswith("]"):
            current = line[1:-1].strip().lower()
        elif "=" in line and current == section.lower():
            k, v = line.split("=", 1)
            if k.strip().lower() == key.lower():
                return v.strip()
    return None


def native(game: Path, path: str) -> Path:
    return game / path.replace("\\", "/")


def engine_layers(game: Path, ce_dat: Path, f2_res: Path | None = None) -> list[Layer]:
    """The engine's chain for an installed game, lowest priority first:
    master.dat < critter.dat < f2_res.dat < ce.dat < patchNNN.dat < mods
    (mods_order.txt) < critter_patches < master_patches (data/)."""
    layers = [Layer("master.dat", game / "master.dat"), Layer("critter.dat", game / "critter.dat")]
    if f2_res is None:
        name = (read_ini_value(game / "fallout2.cfg", "system", "f2_res_dat")
                or read_ini_value(game / "f2_res.ini", "main", "f2_res_dat") or "f2_res.dat")
        f2_res = native(game, name)
    if f2_res.exists():
        layers.append(Layer(f"f2_res ({f2_res})", f2_res))
    layers.append(Layer("ce.dat", ce_dat))
    index = 0
    while (game / f"patch{index:03d}.dat").exists():
        layers.append(Layer(f"patch{index:03d}.dat", game / f"patch{index:03d}.dat"))
        index += 1
    order = game / "mods/mods_order.txt"
    if order.exists():
        for line in order.read_text(errors="replace").splitlines():
            line = line.strip()
            if not line or line[0] in ";#":
                continue
            mod = game / "mods" / line
            if mod.exists():
                layers.append(Layer(f"mods/{line}", mod))
    if (game / "data").is_dir():
        layers.append(Layer("data/", game / "data"))
    return layers


class Game:
    def __init__(self, layers: list[Layer]):
        self.layers = layers
        self.owner: dict[str, Layer] = {}
        for layer in layers:
            for p in layer.entries:
                self.owner[p] = layer

    def read(self, paths: list[str]) -> dict[str, bytes]:
        by_layer = defaultdict(list)
        for p in paths:
            if p in self.owner:
                by_layer[self.owner[p]].append(p)
        result = {}
        for layer, ps in by_layer.items():
            result.update(layer.read_many(ps))
        return result


# --- packs ---------------------------------------------------------------------


def read_fhd_frames(path: Path) -> list[tuple[int, int]]:
    data = path.read_bytes()
    magic, directions, frames, _flags = struct.unpack_from("<4sHHI", data, 0)
    if magic != b"FHD1":
        raise ValueError(f"{path}: not an FHD1 file")
    result = []
    for d in range(directions):
        for f in range(frames):
            _offset, size = struct.unpack_from("<II", data, 12 + 8 * (d * frames + f))
            if size:
                result.append((d, f))
    return result


def pack_assets(pack: Path) -> dict[str, list[tuple[int, int]]]:
    """{asset stem (art/<type>/<name>): [(direction, frame), ...]} of a pack;
    ground variants (art/tiles/variants/) belong to their base tile."""
    assets = {}
    for p in pack.rglob("*"):
        if not p.is_file() or p.suffix not in (".png", ".fhd"):
            continue
        rel = p.relative_to(pack).as_posix()
        if rel.startswith("art/tiles/variants/") or not rel.startswith("art/"):
            continue
        stem = rel[: -len(p.suffix)]
        assets[stem] = read_fhd_frames(p) if p.suffix == ".fhd" else [(0, 0)]
    return assets


def legacy_path(stem: str, game: Game) -> str | None:
    for ext in (".frm", ".png", ".pcx"):
        if stem + ext in game.owner:
            return stem + ext
    return None


def resolve(stems, game: Game) -> dict[str, str]:
    result = {}
    for stem in stems:
        path = legacy_path(stem, game)
        if path is not None:
            result[stem] = path
    return result


def decode_all(paths: list[str], game: Game) -> dict[str, dict]:
    data = game.read(paths)
    decoded = {}
    for p, raw in data.items():
        try:
            decoded[p] = {"bytes": raw, "frames": decode_art(p, raw)}
        except Exception as e:  # noqa: BLE001 - reported, not fatal
            decoded[p] = {"bytes": raw, "frames": None, "error": str(e)}
    return decoded


def vanilla_game(game_dir: Path, f2_res: Path | None) -> Game:
    return Game(engine_layers(game_dir, REPO_ROOT / "files/ce.dat",
                              f2_res or REPO_ROOT / "files/f2_res.dat"))


def cmd_hashes(args) -> int:
    game = vanilla_game(args.game, args.f2_res)
    for pack in args.packs:
        assets = pack_assets(pack)
        paths = resolve(assets, game)
        decoded = decode_all(sorted(set(paths.values())), game)
        lines = ["# Legacy frames this pack was made from (tools/hd/legacy_art.py):",
                 "# <art path> <direction> <frame> <FNV-1a 64 of width, height, indices>"]
        missing = 0
        for stem in sorted(assets):
            path = paths.get(stem)
            art = decoded.get(path) if path else None
            if art is None or art["frames"] is None:
                missing += 1
                continue
            for d, f in sorted(assets[stem]):
                frame = art["frames"].get((d, f)) or (art["frames"].get((0, f)) if d else None)
                if frame is None:
                    continue
                lines.append(f"{path} {d} {f} {frame_hash(*frame):016x}")
        (pack / "legacy.txt").write_text("\n".join(lines) + "\n")
        print(f"{pack}: {len(lines) - 2} frames of {len(assets)} assets; {missing} assets without a legacy source")
    return 0


def cmd_compare(args) -> int:
    base = vanilla_game(args.base, args.f2_res)
    mod = Game(engine_layers(args.mod, REPO_ROOT / "files/ce.dat"))
    assets = {}
    for pack in args.packs:
        for stem, frames in pack_assets(pack).items():
            assets.setdefault(stem, set()).update(frames)

    base_paths = resolve(assets, base)
    mod_paths = resolve(assets, mod)
    candidates = sorted(s for s in assets if s in base_paths and s in mod_paths
                        and (mod.owner[mod_paths[s]].label != base.owner[base_paths[s]].label
                             or base_paths[s] != mod_paths[s]))
    base_dec = decode_all(sorted({base_paths[s] for s in candidates}), base)
    mod_dec = decode_all(sorted({mod_paths[s] for s in candidates}), mod)

    rows = []
    counts = Counter()
    for stem in candidates:
        b, m = base_dec.get(base_paths[stem]), mod_dec.get(mod_paths[stem])
        typ = stem.split("/")[1]
        row = {"art": mod_paths[stem], "type": typ, "mod_source": mod.owner[mod_paths[stem]].label,
               "hd_frames": len(assets[stem])}
        if b is None or m is None or b["frames"] is None or m["frames"] is None:
            row["status"] = "unreadable"
        elif b["bytes"] == m["bytes"]:
            row["status"] = "identical"
        else:
            changed = sized = 0
            worst = 0.0
            for d, f in sorted(assets[stem]):
                bf = b["frames"].get((d, f)) or b["frames"].get((0, f))
                mf = m["frames"].get((d, f)) or m["frames"].get((0, f))
                if bf is None or mf is None:
                    changed += 1
                    worst = 1.0
                elif bf[:2] != mf[:2]:
                    changed += 1
                    sized += 1
                    worst = 1.0
                elif bf[2] != mf[2]:
                    changed += 1
                    diff = sum(x != y for x, y in zip(bf[2], mf[2])) / max(1, len(bf[2]))
                    worst = max(worst, diff)
            row["changed_frames"] = changed
            row["resized_frames"] = sized
            row["worst_pixel_share"] = round(worst, 4)
            row["status"] = "changed" if changed else "same_pixels"
        counts[(typ, row["status"])] += 1
        rows.append(row)

    # Art the mod adds (or that only the mod has) in the HD types: drawn as 2x.
    new_art = []
    for path, layer in sorted(mod.owner.items()):
        parts = path.split("/")
        if len(parts) != 3 or parts[0] != "art" or parts[1] not in HD_TYPES:
            continue
        if not path.endswith((".frm", ".png")):
            continue
        if path in base.owner:
            continue
        new_art.append({"art": path, "type": parts[1], "mod_source": layer.label})
    new_counts = Counter(a["type"] for a in new_art)

    report = {
        "base": str(args.base), "mod": str(args.mod), "packs": [str(p) for p in args.packs],
        "pack_assets": len(assets),
        "overridden": rows,
        "summary": {f"{t}/{s}": n for (t, s), n in sorted(counts.items())},
        "new_without_hd": new_art,
        "new_without_hd_by_type": dict(sorted(new_counts.items())),
    }
    if args.json:
        args.json.write_text(json.dumps(report, indent=1))
    if args.md:
        args.md.write_text(markdown(report))
    print(json.dumps({k: report[k] for k in ("pack_assets", "summary", "new_without_hd_by_type")}, indent=1))
    return 0


def markdown(report: dict) -> str:
    rows = report["overridden"]
    out = [f"Pack assets: {report['pack_assets']}; art paths the mod resolves from another "
           f"file than the base game: {len(rows)}.", "",
           "| type | identical | same pixels | changed (stale HD) | unreadable |",
           "|---|---|---|---|---|"]
    types = sorted({r["type"] for r in rows})
    for t in types:
        c = Counter(r["status"] for r in rows if r["type"] == t)
        out.append(f"| {t} | {c['identical']} | {c['same_pixels']} | {c['changed']} | {c['unreadable']} |")
    out += ["", "Changed art (an HD image made from the original would cover the mod's picture):", "",
            "| art | from | HD frames | changed | resized | worst pixel share |", "|---|---|---|---|---|---|"]
    for r in sorted((r for r in rows if r["status"] == "changed"), key=lambda r: r["art"]):
        out.append(f"| `{r['art']}` | {r['mod_source']} | {r['hd_frames']} | {r['changed_frames']} | "
                   f"{r['resized_frames']} | {r['worst_pixel_share']:.1%} |")
    out += ["", "Art the mod adds in the HD types (no HD image; drawn as the legacy frame doubled):", "",
            "| type | files |", "|---|---|"]
    for t, n in report["new_without_hd_by_type"].items():
        out.append(f"| {t} | {n} |")
    return "\n".join(out) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    h = sub.add_parser("hashes")
    h.add_argument("packs", nargs="+", type=Path)
    h.add_argument("--game", type=Path, default=DEFAULT_GAME)
    h.add_argument("--f2-res", type=Path)
    c = sub.add_parser("compare")
    c.add_argument("packs", nargs="+", type=Path)
    c.add_argument("--base", type=Path, default=DEFAULT_GAME)
    c.add_argument("--mod", type=Path, required=True)
    c.add_argument("--f2-res", type=Path, help="base game's f2_res.dat (default files/f2_res.dat)")
    c.add_argument("--json", type=Path)
    c.add_argument("--md", type=Path)
    args = parser.parse_args()
    return cmd_hashes(args) if args.command == "hashes" else cmd_compare(args)


if __name__ == "__main__":
    sys.exit(main())
