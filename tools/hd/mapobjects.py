"""Objects placed on Fallout 2 maps (Phase 2.8).

Reads the object section of a .map file (or a save game's gzip-compressed
<MAP>.SAV, the same format) and returns the art (fid) of every object, the
way the engine's mapLoad does: header, global and local variables, tile
grids, scripts, then objects (src/map.cc mapLoad, src/scripts.cc
scriptRead, src/object.cc objectRead, src/proto.cc objectDataRead).

The per-object data depends on the item or scenery subtype, which comes from
the object's prototype, so a `Protos` lookup (proto/items, proto/scenery) is
needed.
"""

from __future__ import annotations

import gzip
import struct
from pathlib import Path

OBJ_TYPE_ITEM = 0
OBJ_TYPE_CRITTER = 1
OBJ_TYPE_SCENERY = 2
OBJ_TYPE_WALL = 3
OBJ_TYPE_TILE = 4
OBJ_TYPE_MISC = 5
OBJ_TYPE_INTERFACE = 6

ART_TYPE_DIRS = ["items", "critters", "scenery", "walls", "tiles", "misc", "intrface", "inven",
                 "heads", "backgrnd", "skilldex"]

ITEM_TYPE_WEAPON, ITEM_TYPE_AMMO, ITEM_TYPE_MISC, ITEM_TYPE_KEY = 3, 4, 5, 6
SCENERY_TYPE_DOOR, SCENERY_TYPE_STAIRS, SCENERY_TYPE_ELEVATOR = 0, 1, 2
SCENERY_TYPE_LADDER_UP, SCENERY_TYPE_LADDER_DOWN = 3, 4

SCRIPT_TYPE_SPATIAL = 1
SCRIPT_TYPE_TIMED = 2
SCRIPT_LIST_EXTENT_SIZE = 16


def read_map_bytes(path: Path) -> bytes:
    data = path.read_bytes()
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    return data


class Protos:
    """Item and scenery subtypes by pid, read from extracted proto files.
    `root` holds proto/items/items.lst, proto/items/<n>.pro, and the same for
    scenery."""

    def __init__(self, root: Path):
        self.root = root
        self.lists = {}
        self.cache: dict[int, int] = {}
        for kind in ("items", "scenery"):
            lst = root / "proto" / kind / f"{kind}.lst"
            names = []
            if lst.exists():
                names = [l.split()[0].lower() if l.split() else "" for l in
                         lst.read_text(encoding="latin-1").splitlines()]
            self.lists[kind] = names

    def files(self, kind: str) -> list[str]:
        return [f"proto/{kind}/{n}" for n in self.lists.get(kind, []) if n]

    def subtype(self, pid: int) -> int:
        if pid in self.cache:
            return self.cache[pid]
        kind = {OBJ_TYPE_ITEM: "items", OBJ_TYPE_SCENERY: "scenery"}[(pid >> 24) & 0xF]
        index = (pid & 0xFFFFFF) - 1
        names = self.lists[kind]
        if index < 0 or index >= len(names):
            raise ValueError(f"pid {pid:#x}: no {kind} proto")
        data = (self.root / "proto" / kind / names[index]).read_bytes()
        # pid, message id, fid, light distance, light intensity, flags,
        # extended flags, sid, subtype
        value = struct.unpack(">i", data[32:36])[0]
        self.cache[pid] = value
        return value


class Reader:
    def __init__(self, data: bytes, offset: int = 0):
        self.data = data
        self.offset = offset

    def i32(self) -> int:
        value = struct.unpack_from(">i", self.data, self.offset)[0]
        self.offset += 4
        return value

    def skip(self, count: int) -> None:
        self.offset += 4 * count


def _skip_scripts(r: Reader) -> None:
    for _type in range(5):
        count = r.i32()
        if count <= 0:
            continue
        extents = (count + SCRIPT_LIST_EXTENT_SIZE - 1) // SCRIPT_LIST_EXTENT_SIZE
        for _ in range(extents):
            for _ in range(SCRIPT_LIST_EXTENT_SIZE):
                sid = r.i32()
                r.skip(1)  # field_4
                script_type = (sid >> 24) & 0xFF
                if script_type == SCRIPT_TYPE_SPATIAL:
                    r.skip(2)
                elif script_type == SCRIPT_TYPE_TIMED:
                    r.skip(1)
                r.skip(14)
            r.skip(2)  # extent length, next


def _read_object(r: Reader, protos: Protos, version: int, out: list, depth: int) -> None:
    (oid, tile, x, y, sx, sy, frame, rotation, fid, flags, elevation, pid, cid,
     light_distance, light_intensity, _f74, sid, script_index) = \
        struct.unpack_from(">18i", r.data, r.offset)
    r.offset += 18 * 4
    length, capacity, _ptr = r.i32(), r.i32(), r.i32()
    if length < 0 or capacity < length:
        raise ValueError(f"bad inventory {length}/{capacity} at {r.offset}")
    otype = (pid >> 24) & 0xF
    if otype == OBJ_TYPE_CRITTER:
        r.skip(1 + 7 + 3)  # reaction, combat data, hp, radiation, poison
    else:
        r.skip(1)  # data flags
        if otype == OBJ_TYPE_ITEM:
            sub = protos.subtype(pid)
            r.skip({ITEM_TYPE_WEAPON: 2, ITEM_TYPE_AMMO: 1, ITEM_TYPE_MISC: 1, ITEM_TYPE_KEY: 1}.get(sub, 0))
        elif otype == OBJ_TYPE_SCENERY:
            sub = protos.subtype(pid)
            if sub == SCENERY_TYPE_DOOR:
                r.skip(1)
            elif sub in (SCENERY_TYPE_STAIRS, SCENERY_TYPE_ELEVATOR):
                r.skip(2)
            elif sub in (SCENERY_TYPE_LADDER_UP, SCENERY_TYPE_LADDER_DOWN):
                r.skip(1 if version == 19 else 2)
        elif otype == OBJ_TYPE_MISC:
            if 0x5000010 <= pid <= 0x5000017:
                r.skip(4)
    out.append({"fid": fid & 0xFFFFFFFF, "pid": pid, "tile": tile, "x": x, "y": y, "frame": frame,
                "rotation": rotation,
                "flags": flags & 0xFFFFFFFF, "elevation": elevation, "depth": depth})
    for _ in range(length):
        r.skip(1)  # quantity
        _read_object(r, protos, version, out, depth + 1)


def read_map_objects(path: Path, protos: Protos) -> list[dict]:
    """Every object of the map (inventory items too, with depth > 0)."""
    b = read_map_bytes(path)
    (version, _name, _ent, _elev, _rot, nloc, _sid, flags, _dark, nglob, _idx, _t) = \
        struct.unpack(">i16s10i", b[:60])
    if version not in (19, 20):
        raise ValueError(f"{path}: unsupported map version {version}")
    off = 236 + 4 * nglob + 4 * nloc
    for e in range(3):
        if not flags & (2 << e):
            off += 40000
    r = Reader(b, off)
    _skip_scripts(r)
    total = r.i32()
    objects: list[dict] = []
    for elevation in range(3):
        count = r.i32()
        for _ in range(count):
            _read_object(r, protos, version, objects, 0)
    top = sum(1 for o in objects if o["depth"] == 0)
    if top != total:
        raise ValueError(f"{path}: read {top} objects, header says {total}")
    return objects


def fid_art_path(fid: int, art_lists: dict[str, list[str]]) -> str | None:
    """art/<type>/<name>.frm for a non-critter fid, or None."""
    otype = (fid >> 24) & 0xF
    if otype == OBJ_TYPE_CRITTER or otype >= len(ART_TYPE_DIRS):
        return None
    folder = ART_TYPE_DIRS[otype]
    names = art_lists.get(folder, [])
    index = fid & 0xFFF
    if index >= len(names) or not names[index]:
        return None
    return f"art/{folder}/{names[index]}"


def read_art_list(path: Path) -> list[str]:
    names = []
    for line in path.read_text(encoding="latin-1").splitlines():
        token = line.split()[0].lower() if line.split() else ""
        token = token.split(";")[0].strip()
        names.append(token)
    return names


def frm_frames(path: Path):
    """(directions, frames per direction, frames) of an FRM; frames[d][f] is
    (width, height, bytes). directions is 1 when every direction points at
    the same data, else 6."""
    b = path.read_bytes()
    _version, _fps, _action, count = struct.unpack_from(">IHHH", b, 0)
    offsets = struct.unpack_from(">6I", b, 34)
    directions = 1 if len(set(offsets)) == 1 else 6
    frames = []
    for d in range(directions):
        pos = 62 + offsets[d]
        row = []
        for _ in range(count):
            w, h, size, _x, _y = struct.unpack_from(">HHIhh", b, pos)
            pos += 12
            row.append((w, h, b[pos:pos + w * h]))
            pos += size
        frames.append(row)
    return directions, count, frames
