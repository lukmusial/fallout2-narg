#!/usr/bin/env python3
"""HD pack for the art a content mod adds or changes (Phase 2.21: the
Restoration Project, pack `rpu`).

The mod's art is read through the engine's file chain of the installed,
modded game (legacy_art.engine_layers, as src/game.cc and src/sfall_ext.cc
open it: master.dat < critter.dat < f2_res < ce.dat < patchNNN.dat <
mods/mods_order.txt from top to bottom < data/) and compared frame by frame
with the original game's chain. Every frame of the HD art types
(legacy_art.HD_TYPES; critters stay legacy) that the mod adds or draws
differently goes into the pack; frames the mod leaves as they were are left
out, so on the original game the pack never shows anything new.

Then the normal pipeline: enhance_ai.py (Real-ESRGAN x2plus, the user's
choice for this pack) on the pack's own manifest, install.py, and
legacy_art.py hashes against the mod's chain, so the engine uses an image
only while the game draws exactly the mod's frame. The packs are tried in
order (`* = ai rpu` in hd/mixed/manifest.txt, src/hd_art.h): on the original
game the normal pack wins, on the mod this one wherever the mod's art is
drawn.

  mod_pack.py extract --mod DIR [--base DIR]    # art -> work dir (indexed PNGs, manifest)
  mod_pack.py enhance                           # enhance_ai.py --pack <pack>
  mod_pack.py install --mod DIR [--run-dir DIR] # -> <run dir>/data/hd/<pack>/ + legacy.txt
  mod_pack.py all --mod DIR                     # the three in a row
  options: --pack NAME (default rpu)

Work files go to tools/hd/work/<pack>/ (HD_WORK_DIR) and the enhanced images
to tools/hd/work/<pack>/assets/<pack>/ (HD_ASSETS_DIR), both gitignored.

What the extraction handles beyond extract.py:
- the mod's own tiles.lst, maps and protos: floor/roof kinds and the fixed
  tile neighbours (extract.tile_neighbours) and wall runs
  (wallmap.wall_neighbours) come from the mod's maps, and neighbours that
  are not in the pack are converted as context only;
- art with its own palette (art/intrface/<name>.pal next to <name>.frm, e.g.
  the endgame slides eg_*, death.frm, helpscrn.frm) is converted through it;
- multi-frame art per frame and direction (install.py packs .fhd files;
  frames the mod did not change stay empty entries).
Colour-cycling indices (229-254) stay transparent as everywhere
(hdlib.compose_rgba).
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

HD_DIR = Path(__file__).resolve().parent


def _pack_argument() -> str:
    for i, arg in enumerate(sys.argv):
        if arg == "--pack" and i + 1 < len(sys.argv):
            return sys.argv[i + 1]
        if arg.startswith("--pack="):
            return arg.split("=", 1)[1]
    return "rpu"


PACK = _pack_argument()
os.environ.setdefault("HD_WORK_DIR", str(HD_DIR / "work" / PACK))
os.environ.setdefault("HD_ASSETS_DIR", str(Path(os.environ["HD_WORK_DIR"]) / "assets"))

import extract  # noqa: E402
import hdlib  # noqa: E402
import legacy_art  # noqa: E402
import mapobjects  # noqa: E402

# Intro and ending sequences (seq*, arroyo01, dp) are not drawn through the
# HD layer; endgame slides are kept (their own palettes).
SKIP_PREFIXES = ("seq",)
SKIP = extract.INTERFACE_SKIP
# Layers that are the same files in both games (the mod is installed into a
# copy of the original; ce.dat is this repository's).
SHARED_LAYERS = ("master.dat", "critter.dat", "ce.dat")


class GameArchives(extract.Archives):
    """extract.Archives over an installed game's whole chain, mods included
    (legacy_art.engine_layers), lowest priority first."""

    def __init__(self, game: Path):
        self.chain = legacy_art.engine_layers(game, legacy_art.REPO_ROOT / "files/ce.dat")
        self.layers = [(layer.label, layer.path, layer.entries) for layer in self.chain]
        self.loose = {}

    def fetch(self, paths: list[str], raw: Path) -> dict[str, str]:
        owner = {}
        for layer in self.chain:
            for p in paths:
                if p.lower() in layer.entries:
                    owner[p.lower()] = layer
        by_layer: dict = {}
        for p, layer in owner.items():
            by_layer.setdefault(id(layer), (layer, []))[1].append(p)
        source = {}
        for layer, ps in by_layer.values():
            for p, data in layer.read_many(ps).items():
                dst = raw / p
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.write_bytes(data)
                source[p] = layer.label
        missing = sorted({p.lower() for p in paths} - set(source))
        if missing:
            print(f"warning: not found: {', '.join(missing[:20])}{' ...' if len(missing) > 20 else ''}",
                  file=sys.stderr)
        return source


def mod_art(mod: legacy_art.Game, base: legacy_art.Game) -> dict[str, dict]:
    """{art path: {"status": "new"|"changed", "frames": {(d, f), ...} or None
    (all), "source": layer}} of the HD types: art only the mod has, and art
    whose frames the mod draws differently (only those frames)."""
    candidates = {}
    for path, layer in mod.owner.items():
        parts = path.split("/")
        if len(parts) != 3 or parts[0] != "art" or parts[1] not in legacy_art.HD_TYPES:
            continue
        if not path.endswith(".frm"):
            continue
        stem = Path(path).stem
        if parts[1] == "intrface" and (stem.startswith(SKIP_PREFIXES) or path in SKIP):
            continue
        if path not in base.owner:
            candidates[path] = {"status": "new", "frames": None, "source": layer.label}
            continue
        if layer.label == base.owner[path].label and layer.label in SHARED_LAYERS:
            continue
        candidates[path] = {"status": "changed", "frames": None, "source": layer.label}

    changed = [p for p, c in candidates.items() if c["status"] == "changed"]
    mod_dec = legacy_art.decode_all(changed, mod)
    base_dec = legacy_art.decode_all(changed, base)
    for path in changed:
        m, b = mod_dec.get(path), base_dec.get(path)
        if m is None or m["frames"] is None:
            del candidates[path]
            continue
        if b is not None and b["bytes"] == m["bytes"]:
            del candidates[path]
            continue
        bf = (b or {}).get("frames") or {}
        differ = set()
        for (d, f), frame in m["frames"].items():
            old = bf.get((d, f)) or (bf.get((0, f)) if d else None)
            if old != frame:
                differ.add((d, f))
        if not differ:
            del candidates[path]
        else:
            candidates[path]["frames"] = differ
    return candidates


def own_palette(path: str, mod: legacy_art.Game) -> str | None:
    """art/intrface/<name>.pal for art/intrface/<name>.frm, when the game has it."""
    if not path.startswith("art/intrface/"):
        return None
    pal = path[:-4] + ".pal"
    return pal if pal in mod.owner else None


def to_png(frm: Path, dst: Path, pal: Path, frame: int | None = None, direction: int | None = None) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(extract.tool("ce-frm2png")), str(frm), str(dst), "--palette", str(pal)]
    if frame is not None:
        cmd += ["--frame", str(frame), "--direction", str(direction)]
    subprocess.run(cmd, check=True, capture_output=True)


def convert(path: str, raw: Path, pal: Path, frames: set | None) -> list[dict]:
    """Manifest entries of one FRM: one image, or one per frame and direction
    (only `frames` when given) for multi-frame art (install.py packs .fhd)."""
    typ, name = path.split("/")[1], Path(path).stem
    frm = raw / path
    directions, count, _ = mapobjects.frm_frames(frm)
    kind = "interface" if typ == "intrface" else typ.rstrip("s")
    if directions * count == 1:
        key = f"art/{typ}/{name}.png"
        to_png(frm, hdlib.SRC_DIR / key, pal)
        idx, _ = hdlib.load_indexed(hdlib.SRC_DIR / key)
        return [{"key": key, "name": name, "type": typ, "kind": kind,
                 "width": int(idx.shape[1]), "height": int(idx.shape[0])}]
    out = []
    for d in range(directions):
        for f in range(count):
            if frames is not None and (d, f) not in frames and not (directions == 1 and (0, f) in frames):
                continue
            key = f"art/{typ}/{name}/d{d}_f{f}.png"
            to_png(frm, hdlib.SRC_DIR / key, pal, f, d)
            idx, _ = hdlib.load_indexed(hdlib.SRC_DIR / key)
            out.append({"key": key, "name": name, "type": typ, "kind": kind,
                        "width": int(idx.shape[1]), "height": int(idx.shape[0]),
                        "fhd": f"art/{typ}/{name}.fhd", "direction": d, "frame": f,
                        "directions": directions, "frames": count})
    return out


def placed_walls(arch: GameArchives, raw: Path, maps: list[str]) -> tuple[list[str], set[str]]:
    """(maps whose objects could be read, wall art placed on them)."""
    kinds = ("items", "scenery", "walls", "misc")
    arch.fetch(["proto/items/items.lst", "proto/scenery/scenery.lst"] + [f"art/{k}/{k}.lst" for k in kinds], raw)
    protos = mapobjects.Protos(raw)
    arch.fetch(protos.files("items") + protos.files("scenery"), raw)
    lists = {k: mapobjects.read_art_list(raw / f"art/{k}/{k}.lst") for k in kinds}
    readable, walls = [], set()
    for m in maps:
        try:
            objects = mapobjects.read_map_objects(raw / m, protos)
        except Exception as e:  # noqa: BLE001 - reported, the map is left out
            print(f"warning: {m}: {e}", file=sys.stderr)
            continue
        readable.append(Path(m).stem)
        for o in objects:
            if not o["depth"] and ((o["fid"] >> 24) & 0xF) == mapobjects.OBJ_TYPE_WALL:
                art = mapobjects.fid_art_path(o["fid"], lists)
                if art:
                    walls.add(art)
    return readable, walls


def context_png(key: str, raw: Path, pal: Path) -> None:
    """Source PNG of a neighbour that is not in the pack (enhancer context)."""
    dst = hdlib.SRC_DIR / key
    if dst.exists():
        return
    parts = key.split("/")
    art = raw / f"art/{parts[1]}/{parts[2].removesuffix('.png')}.frm"
    if not art.exists():
        return
    if len(parts) == 3:
        to_png(art, dst, pal)
    else:
        d, f = parts[3].removesuffix(".png")[1:].split("_f")
        to_png(art, dst, pal, int(f), int(d))


def cmd_extract(args) -> int:
    t0 = time.perf_counter()
    raw = hdlib.WORK_DIR / "raw"
    raw.mkdir(parents=True, exist_ok=True)
    arch = GameArchives(args.mod)
    mod = legacy_art.Game(arch.chain)
    base = legacy_art.vanilla_game(args.base, None)
    print("mod chain:", " < ".join(layer.label for layer in arch.chain))
    print("base chain:", " < ".join(layer.label for layer in base.layers))

    art = mod_art(mod, base)
    counts: dict = {}
    for path, a in art.items():
        key = (path.split("/")[1], a["status"])
        counts[key] = counts.get(key, 0) + 1
    print("mod art:", {f"{t}/{s}": n for (t, s), n in sorted(counts.items())})

    palettes = sorted({p for p in (own_palette(path, mod) for path in art) if p})
    arch.fetch(["color.pal", "art/tiles/tiles.lst"] + sorted(art) + palettes, raw)
    pal = raw / "color.pal"

    # Tiles: floor or roof, and fixed neighbours, from the mod's own maps.
    tile_names = [l.strip().lower() for l in (raw / "art/tiles/tiles.lst").read_text(encoding="latin-1").splitlines()]
    maps, floors, roofs, _ = extract.area_tiles(arch, raw, tile_names, ("",))
    print(f"{len(maps)} maps of the mod: {len(floors)} floor, {len(roofs)} roof tiles placed")

    assets = []
    for path in sorted(art):
        own = own_palette(path, mod)
        entries = convert(path, raw, raw / own if own else pal, art[path]["frames"])
        for entry in entries:
            if entry["type"] == "tiles":
                entry["kind"] = "roof" if path in roofs and path not in floors else "tile"
            entry.update({"art": path, "mod": art[path]["status"], "source": art[path]["source"]})
            if own:
                entry["palette"] = own
        assets.extend(entries)

    pack_tiles = {Path(p).stem for p in art if p.startswith("art/tiles/")}
    neighbours = extract.tile_neighbours(raw, maps, tile_names)
    neighbours = {t: n for t, n in neighbours.items() if t in pack_tiles}
    hdlib.NEIGHBOURS.write_text(json.dumps(neighbours, indent=0, sort_keys=True))
    context = sorted({f"art/tiles/{n}.frm" for v in neighbours.values() for n in v.values()} - set(art))
    arch.fetch(context, raw)
    for p in context:
        context_png(f"art/tiles/{Path(p).stem}.png", raw, pal)
    print(f"{len(neighbours)} pack tiles have fixed neighbours ({len(context)} context tiles outside the pack)")

    # Walls: the runs they are placed in on the mod's maps.
    pack_walls = {p for p in art if p.startswith("art/walls/")}
    readable, walls = placed_walls(arch, raw, maps)
    arch.fetch(sorted(walls), raw)
    import wallmap

    runs = wallmap.wall_neighbours(readable)
    pack_keys = {a["key"] for a in assets if a["type"] == "walls"}
    runs = {k: v for k, v in runs.items() if k in pack_keys}
    hdlib.WALL_NEIGHBOURS.write_text(json.dumps(runs, indent=0, sort_keys=True))
    for v in runs.values():
        for key, _dx, _dy in v:
            context_png(key, raw, pal)
    print(f"{len(readable)} maps read for walls; {len(runs)} of {len(pack_keys)} pack wall images "
          f"have neighbours ({len(pack_walls)} wall FRMs in the pack)")

    hdlib.MANIFEST.write_text(json.dumps({"assets": assets, "regions": []}, indent=1))
    summary = {}
    for a in assets:
        summary[a["type"]] = summary.get(a["type"], 0) + 1
    print(f"{len(assets)} images of {len(art)} FRMs {summary} -> {hdlib.MANIFEST} "
          f"({time.perf_counter() - t0:.0f}s)")
    return 0


def cmd_enhance(args) -> int:
    t0 = time.perf_counter()
    subprocess.run([sys.executable, str(HD_DIR / "enhance_ai.py"), "--pack", PACK], check=True, cwd=HD_DIR)
    print(f"enhanced in {time.perf_counter() - t0:.0f}s")
    return 0


def cmd_install(args) -> int:
    cmd = [sys.executable, str(HD_DIR / "install.py"), PACK, "--mod", str(args.mod)]
    if args.run_dir:
        cmd += ["--run-dir", str(args.run_dir)]
    subprocess.run(cmd, check=True, cwd=HD_DIR)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=("extract", "enhance", "install", "all"))
    ap.add_argument("--pack", default=PACK)
    ap.add_argument("--mod", type=Path, help="the installed, modded game")
    ap.add_argument("--base", type=Path, default=hdlib.GAME_DIR, help="the original game (default FALLOUT2_DIR)")
    ap.add_argument("--run-dir", type=Path, help="install into <dir>/data/hd (default out/build/macos)")
    args = ap.parse_args()
    if args.command in ("extract", "install", "all") and args.mod is None:
        ap.error("--mod is required")
    steps = {"extract": [cmd_extract], "enhance": [cmd_enhance], "install": [cmd_install],
             "all": [cmd_extract, cmd_enhance, cmd_install]}[args.command]
    for step in steps:
        if step(args):
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
