#!/usr/bin/env python3
"""Extract the Phase 2.5 sample art from the installed game's .dat files.

Uses ce-dat-tool (archive access) and ce-frm2png (FRM -> palette-indexed PNG,
indices preserved, index 0 transparent).  Resolution order mirrors the
engine's file layering: master.dat < critter.dat < patch000.dat < loose files
in <game>/data.

Writes into the gitignored work dir (tools/hd/work by default):
  raw/...                       extracted .frm / .lst / .map / color.pal
  png/art/<type>/<name>.png     indexed source PNGs
  manifest.json                 asset list + map-region floor grids

  extract.py                    # default sample set
  extract.py --extra art/tiles/brick02.frm --extra art/scenery/car2.frm
"""

from __future__ import annotations

import argparse
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import hdlib
from PIL import Image

INTERFACE = [
    "art/intrface/mainmenu.frm",
    "art/intrface/lsgame.frm",
    "art/intrface/edtredt.frm",
    "art/intrface/edtrcrte.frm",
    "art/intrface/menuup.frm",
    "art/intrface/menudown.frm",
]

# Phase 2.6: everything the main menu, load/save screen, character editor and
# Stimpack hub draw as art (text and the big numbers stay legacy).
SCREENS = [
    # main menu (hi-res variants from f2_res.dat)
    "art/intrface/hr_mainmenu.frm", "art/intrface/hr_menu_bg.frm",
    # load/save
    "art/intrface/lscover.frm", "art/intrface/lilredup.frm", "art/intrface/lilreddn.frm",
    "art/intrface/uparwoff.frm", "art/intrface/uparwon.frm",
    "art/intrface/dnarwoff.frm", "art/intrface/dnarwon.frm",
    # character editor, its dialogs and the perk dialog
    "art/intrface/ageoff.frm", "art/intrface/ageon.frm", "art/intrface/agebox.frm",
    "art/intrface/nameoff.frm", "art/intrface/nameon.frm", "art/intrface/namebox.frm",
    "art/intrface/sexoff.frm", "art/intrface/sexon.frm",
    "art/intrface/slider.frm",
    "art/intrface/snegoff.frm", "art/intrface/snegon.frm", "art/intrface/splsoff.frm", "art/intrface/splson.frm",
    "art/intrface/stnegoff.frm", "art/intrface/stnegon.frm", "art/intrface/stplsoff.frm", "art/intrface/stplson.frm",
    "art/intrface/attribox.frm", "art/intrface/attribwn.frm", "art/intrface/charwin.frm", "art/intrface/donebox.frm",
    "art/intrface/femoff.frm", "art/intrface/femon.frm", "art/intrface/maleoff.frm", "art/intrface/maleon.frm",
    "art/intrface/slu.frm", "art/intrface/sld.frm", "art/intrface/sru.frm", "art/intrface/srd.frm",
    "art/intrface/bararrws.frm",
    "art/intrface/karmafdr.frm", "art/intrface/killsfdr.frm", "art/intrface/perksfdr.frm",
    "art/intrface/tgskloff.frm", "art/intrface/tgsklon.frm",
    "art/intrface/perkwin.frm",
    # options panel (character editor) and Stimpack hub buttons
    "art/intrface/opbase.frm", "art/intrface/opbtnoff.frm", "art/intrface/opbtnon.frm",
]

# Named indexed PNGs from this repo's ce.dat (Stimpack art), same palette indices.
CE_DAT_PNG = [
    "art/intrface/mainmenu_sp.png",
    "art/intrface/hr_menu_bg_sp.png",
    "art/intrface/opbase_sp.png",
]

# Floor tiles seen in Arroyo / Klamath / Den maps (counted from the map grids)
# plus a few named families from the plan; roofs; walls; scenery.
TILES = [
    "art/tiles/brick01.frm",
    "art/tiles/brn1000.frm",
    "art/tiles/capb000.frm",
    "art/tiles/cav4009.frm",
    "art/tiles/cav4010.frm",
    "art/tiles/cavb001.frm",
    "art/tiles/edg5000.frm",
    "art/tiles/edg5003.frm",
    "art/tiles/tepflr01.frm",
    "art/tiles/road02.frm",
    "art/tiles/curb16.frm",
    "art/tiles/cmt2000.frm",
    "art/tiles/slime09.frm",   # Klamath toxic caves: colour-cycling slime
    "art/tiles/shore05.frm",   # colour-cycling shoreline
    # roofs (also live in art/tiles)
    "art/tiles/arrf009.frm",
    "art/tiles/tp096.frm",
    "art/tiles/grof025.frm",
]

OBJECTS = [
    "art/walls/adb1026.frm",
    "art/walls/temple16.frm",
    "art/scenery/car1.frm",
    "art/scenery/drock1.frm",
]

# (map, elevation, first row, first col, size): small floor patches used to
# demonstrate tile seams.  Chosen from regions without grid000 placeholders.
MAP_REGIONS = [
    ("arvillag", 0, 18, 49, 6),   # Arroyo village: desert edges, cave ground
    ("kladwtwn", 0, 80, 44, 6),   # Klamath downtown: road, curb, desert
    ("arcaves", 0, 25, 51, 6),    # Arroyo caves: temple floor -> cave floor
    ("denbus1", 0, 63, 34, 6),    # Den business: road, curb, building floor
]


def tool(name: str) -> Path:
    path = hdlib.BUILD_DIR / name
    if not path.exists():
        raise SystemExit(f"{path} missing: cmake --build out/build/macos --target ce-dat-tool ce-frm2png")
    return path


def list_dat(dat: Path) -> dict[str, str]:
    out = subprocess.run([str(tool("ce-dat-tool")), str(dat), "list"], check=True,
                         capture_output=True, text=True, encoding="latin-1").stdout
    entries = {}
    for line in out.splitlines():
        line = line.strip()
        if line:
            entries[line.replace("\\", "/").lower()] = line
    return entries


class Archives:
    def __init__(self):
        g = hdlib.GAME_DIR
        self.layers: list[tuple[str, Path, dict[str, str]]] = []
        for name in ("master.dat", "critter.dat", "patch000.dat"):
            dat = g / name
            if dat.exists():
                self.layers.append((name, dat, list_dat(dat)))
        # The hi-res patch archive the engine loads (HR_*.FRM).
        f2res = hdlib.REPO_ROOT / "files" / "f2_res.dat"
        if f2res.exists():
            self.layers.append(("f2_res.dat", f2res, list_dat(f2res)))
        self.loose = {}
        data = g / "data"
        if data.is_dir():
            for p in data.rglob("*"):
                if p.is_file() and p.suffix.lower() in (".frm", ".lst", ".map", ".pal"):
                    self.loose[str(p.relative_to(data)).lower()] = p

    def fetch(self, paths: list[str], raw: Path) -> dict[str, str]:
        """Extract lowercase paths into raw/, honouring the layer order.
        Returns path -> source label."""
        wanted = {p.lower() for p in paths}
        source = {}
        for label, dat, entries in self.layers:
            hits = [p for p in wanted if p in entries]
            if not hits:
                continue
            listfile = raw / f".{label}.list"
            listfile.write_text("\n".join(entries[p] for p in sorted(hits)) + "\n", encoding="latin-1")
            subprocess.run([str(tool("ce-dat-tool")), str(dat), "extract", "--lower",
                            "--file-list", str(listfile), str(raw)],
                           check=True, capture_output=True)
            for p in hits:
                source[p] = label
        for p in wanted:
            if p in self.loose:
                dst = raw / p
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(self.loose[p], dst)
                source[p] = "data/"
        missing = sorted(wanted - set(source))
        if missing:
            print("warning: not found:", ", ".join(missing), file=sys.stderr)
        return source


def read_map_floor(path: Path, elevation: int) -> list[list[int]] | None:
    """Return the 100x100 floor/roof fid grid (32-bit words) of one elevation."""
    b = path.read_bytes()
    (version, _name, _ent, _elev, _rot, nloc, _sid, flags, _dark, nglob, _idx, _t) = \
        struct.unpack(">i16s10i", b[:60])
    if version not in (19, 20):
        raise ValueError(f"{path}: unsupported map version {version}")
    off = 236 + 4 * nglob + 4 * nloc
    for e in range(3):
        if flags & (2 << e):
            continue
        grid = struct.unpack(">10000I", b[off:off + 40000])
        off += 40000
        if e == elevation:
            return [list(grid[r * 100:(r + 1) * 100]) for r in range(100)]
    return None


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--extra", action="append", default=[], help="additional art path (e.g. art/tiles/brick02.frm)")
    ap.add_argument("--no-maps", action="store_true", help="skip the map-region tiles")
    args = ap.parse_args()

    raw = hdlib.WORK_DIR / "raw"
    raw.mkdir(parents=True, exist_ok=True)
    arch = Archives()

    base = ["color.pal", "art/tiles/tiles.lst"]
    maps = [] if args.no_maps else sorted({f"maps/{m}.map" for m, *_ in MAP_REGIONS})
    arch.fetch(base + maps, raw)
    tile_names = [l.strip().lower() for l in (raw / "art/tiles/tiles.lst").read_text(encoding="latin-1").splitlines()]

    regions = []
    region_tiles: set[str] = set()
    for m, elev, r0, c0, n in ([] if args.no_maps else MAP_REGIONS):
        grid = read_map_floor(raw / f"maps/{m}.map", elev)
        if grid is None:
            print(f"warning: {m} elevation {elev} missing", file=sys.stderr)
            continue
        floor = [[grid[r][c] & 0xFFF for c in range(c0, c0 + n)] for r in range(r0, r0 + n)]
        names = [[tile_names[i].removesuffix(".frm") for i in row] for row in floor]
        regions.append({"map": m, "elevation": elev, "row": r0, "col": c0, "size": n, "tiles": names})
        region_tiles.update(f"art/tiles/{x}.frm" for row in names for x in row)

    art = list(dict.fromkeys(INTERFACE + SCREENS + TILES + OBJECTS + sorted(region_tiles) + [e.lower() for e in args.extra]))
    sources = arch.fetch(art, raw)

    pal = raw / "color.pal"
    assets = []
    for path in art:
        if path not in sources:
            continue
        frm = raw / path
        typ = path.split("/")[1]
        name = Path(path).stem
        key = f"art/{typ}/{name}.png"
        dst = hdlib.SRC_DIR / key
        dst.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(tool("ce-frm2png")), str(frm), str(dst), "--palette", str(pal)],
                       check=True, capture_output=True)
        idx, _ = hdlib.load_indexed(dst)
        if path in TILES or path in region_tiles:
            kind = "roof" if path in TILES[-3:] else "tile"
        elif typ == "intrface":
            kind = "interface"
        else:
            kind = typ.rstrip("s") if typ.endswith("s") else typ
        assets.append({
            "key": key, "name": name, "type": typ, "kind": kind,
            "width": int(idx.shape[1]), "height": int(idx.shape[0]),
            "source": sources[path], "sample": path in INTERFACE + TILES + OBJECTS,
        })
        print(f"{sources[path]:>12}  {key}  {idx.shape[1]}x{idx.shape[0]}")

    # ce.dat PNGs: re-saved with the palette ce-frm2png writes, so both kinds
    # of source decode to the same colours.
    palette_ref = next((hdlib.SRC_DIR / a["key"] for a in assets if a["type"] == "intrface"), None)
    for path in CE_DAT_PNG:
        src = hdlib.REPO_ROOT / "files" / "ce.dat" / path
        if not src.exists() or palette_ref is None:
            print(f"warning: {src} missing", file=sys.stderr)
            continue
        idx, _ = hdlib.load_indexed(src)
        _, pal = hdlib.load_indexed(palette_ref)
        dst = hdlib.SRC_DIR / path
        dst.parent.mkdir(parents=True, exist_ok=True)
        image = Image.fromarray(idx, "P")
        image.putpalette(pal.flatten().tolist())
        image.save(dst)
        name = Path(path).stem
        assets.append({
            "key": path, "name": name, "type": "intrface", "kind": "interface",
            "width": int(idx.shape[1]), "height": int(idx.shape[0]),
            "source": "ce.dat", "sample": False,
        })
        print(f"{'ce.dat':>12}  {path}  {idx.shape[1]}x{idx.shape[0]}")

    hdlib.MANIFEST.write_text(json.dumps({"assets": assets, "regions": regions}, indent=1))
    print(f"{len(assets)} assets, {len(regions)} map regions -> {hdlib.MANIFEST}")


if __name__ == "__main__":
    main()
