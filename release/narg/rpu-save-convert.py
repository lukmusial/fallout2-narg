#!/usr/bin/env python3
"""Make saves from an unmodded Fallout 2 load with the Restoration Project.

  python3 narg/rpu-save-convert.py <game folder> [--restore] [SLOT01 ...]

RPU declares more global variables (data/vault13.gam: 791 instead of the
1.02 patch's 696) and more party members (data/party.txt: 29 instead of 26).
A save holds data sized by both lists as they were when it was made, and the
engine reads it with the current ones, so an old save fails to load
("Error reading load function #3 data" in debug.log). For every slot in
data/SAVEGAME (or the slots named) this tool:

- with --restore, first copies back the slots RPU's installer moved to
  backup/rpu/data/SAVEGAME (slots already in data/SAVEGAME are left alone);
- appends RPU's new global variables with their starting values, in both
  copies the save holds, and moves GVAR_TORR_BRAHMIN_KILLED from 202 to 74,
  where RPU keeps it;
- adds the new party members: their proto (the game's own), no perks, their
  AI packet from data/ai.txt, and no level-ups.

The original is kept as SAVE.DAT.vanilla; slots already converted, and saves
made during combat, are left alone. Game files are read as the engine finds
them: data/, then the .dat files in mods/mods_order.txt (the bottom line
wins), patch000.dat, master.dat.

Maps you have already visited keep their state from the save, so RPU's
changes to those maps (restored people, items, scenery) show up only in maps
you have not visited yet. Quests RPU added start as if new.
"""

from __future__ import annotations

import argparse
import gzip
import re
import shutil
import struct
import sys
import zlib
from pathlib import Path

HEADER_SIZE = 30051         # signature, version, names, date, thumbnail
GLOBALS_OFFSET = HEADER_SIZE + 4
VANILLA_GLOBALS = 696       # data/vault13.gam of the 1.02 patch
VANILLA_PARTY = 26          # data/party.txt of the 1.02 patch
PERK_COUNT = 119
AI_PACKET_INTS = 45
DUDE_PID = 0x1000000

# Save sections between the AI packets and the world map: stats (5 ints),
# items (none), traits (2 ints), automap (1 int), preferences (80 bytes),
# character editor (5 bytes).
AFTER_AI = 20 + 0 + 8 + 4 + 80 + 5
# Between the world map and the party members: pip-boy (none), movies seen
# (17 bytes), skill uses (18 skills x 3 uses x 4 bytes).
AFTER_WORLDMAP = 0 + 17 + 216

AREA_ATTACK_MODES = ["always", "sometimes", "be_sure", "be_careful", "be_absolutely_sure"]
RUN_AWAY_MODES = ["none", "coward", "finger_hurts", "bleeding", "not_feeling_good", "tourniquet", "never"]
BEST_WEAPONS = ["no_pref", "melee", "melee_over_ranged", "ranged_over_melee", "ranged", "unarmed",
                "unarmed_over_thrown", "random"]
DISTANCES = ["stay_close", "charge", "snipe", "on_your_own", "stay"]
ATTACK_WHO = ["whomever_attacking_me", "strongest", "weakest", "whomever", "closest"]
CHEM_USES = ["clean", "stims_when_hurt_little", "stims_when_hurt_lots", "sometimes", "anytime", "always"]
DISPOSITIONS = ["none", "custom", "coward", "defensive", "aggressive", "berserk"]
# blind, crippled (all limbs), crippled_legs, crippled_arms
HURT_FLAGS = {"blind": 0x40, "crippled": 0x3C, "crippled_legs": 0x0C, "crippled_arms": 0x30}
AI_INTS = ["packet_num", "max_dist", "min_to_hit", "min_hp", "aggression", "hurt_too_much",
           "secondary_freq", "called_freq", "font", "color", "outline_color", "chance",
           "run_start", "run_end", "move_start", "move_end", "attack_start", "attack_end",
           "miss_start", "miss_end"]
HIT_LOCATIONS = ["head", "left_arm", "right_arm", "torso", "right_leg", "left_leg", "eyes", "groin"]


class Skip(Exception):
    pass


# --- game files -----------------------------------------------------------

def dat_read(dat: Path, entry: str) -> bytes | None:
    """One file from a Fallout 2 .dat archive, or None if it is not there."""
    with dat.open("rb") as f:
        f.seek(-8, 2)
        tree_size, data_size = struct.unpack("<II", f.read(8))
        f.seek(data_size - tree_size - 8)
        tree = f.read(tree_size)
        count, = struct.unpack_from("<I", tree, 0)
        pos = 4
        want = entry.lower()
        for _ in range(count):
            name_size, = struct.unpack_from("<I", tree, pos)
            name = tree[pos + 4:pos + 4 + name_size].decode("latin1")
            pos += 4 + name_size
            compressed, _real_size, packed_size, offset = struct.unpack_from("<BIII", tree, pos)
            pos += 13
            if name.lower() == want:
                f.seek(offset)
                data = f.read(packed_size)
                return zlib.decompress(data) if compressed else data
    return None


def ungzip(data: bytes) -> bytes:
    return gzip.decompress(data) if data[:2] == b"\x1f\x8b" else data


def find_loose(base: Path, entry: str) -> Path | None:
    """A file under base, matching names regardless of case."""
    path = base
    for part in entry.split("\\"):
        if not path.is_dir():
            return None
        match = [p for p in path.iterdir() if p.name.lower() == part.lower()]
        if not match:
            return None
        path = match[0]
    return path if path.is_file() else None


class Game:
    def __init__(self, folder: Path):
        self.folder = folder
        dats = []
        order = folder / "mods" / "mods_order.txt"
        if order.is_file():
            for line in order.read_text(errors="replace").splitlines():
                line = line.strip()
                if line and not line.startswith((";", "#")):
                    dats.append(folder / "mods" / line)
            dats.reverse()
        self.dats = [d for d in dats + [folder / "patch000.dat", folder / "master.dat"] if d.is_file()]

    def read(self, entry: str) -> tuple[bytes, str] | None:
        """A game file (`data\\party.txt`) as the engine finds it, or None."""
        loose = find_loose(self.folder / "data", entry)
        if loose is not None:
            return ungzip(loose.read_bytes()), str(loose)
        for dat in self.dats:
            try:
                data = dat_read(dat, entry)
            except (OSError, struct.error, zlib.error):
                continue
            if data is not None:
                return data, str(dat)
        return None


def ini_sections(text: str) -> dict[str, dict[str, str]]:
    sections: dict[str, dict[str, str]] = {}
    current = None
    for line in text.splitlines():
        line = line.split(";")[0].strip()
        if line.startswith("[") and "]" in line:
            current = sections.setdefault(line[1:line.index("]")].strip(), {})
        elif "=" in line and current is not None:
            key, value = line.split("=", 1)
            current[key.strip()] = value.strip()
    return sections


def atoi(text: str) -> int:
    m = re.match(r"\s*([-+]?\d+)", text)
    return int(m.group(1)) if m else 0


def match(value: str | None, keys: list[str]) -> int:
    """The engine's list match: the index, or -1."""
    if value is None:
        return -1
    found = -1
    for index, key in enumerate(keys):
        if value.lower() == key:
            found = index
    return found


def ai_packet(section: dict[str, str]) -> tuple[list[int], int]:
    """An AI packet as the engine saves it, from its data/ai.txt section,
    and its disposition."""
    values = []
    for key in AI_INTS:
        if key == "hurt_too_much":
            hurt = 0
            for flag in section.get(key, "").lower().split(","):
                hurt |= HURT_FLAGS.get(flag.strip(), 0)
            values.append(hurt)
        else:
            values.append(atoi(section.get(key, "0")))
    for location in HIT_LOCATIONS:
        values.append(atoi(section.get(f"hit_{location}_start", "0")))
        values.append(atoi(section.get(f"hit_{location}_end", "0")) + (1 if location == "groin" else 0))
    area = match(section.get("area_attack_mode"), AREA_ATTACK_MODES)
    run_away = match(section.get("run_away_mode"), RUN_AWAY_MODES)
    run_away = run_away - 1 if run_away >= 0 else -1
    values += [area, match(section.get("best_weapon"), BEST_WEAPONS),
               match(section.get("distance"), DISTANCES), match(section.get("attack_who"), ATTACK_WHO),
               match(section.get("chem_use"), CHEM_USES), run_away]
    desire = [-1, -1, -1]
    if "chem_primary_desire" in section:
        for index, item in enumerate(section["chem_primary_desire"].split(",")[:3]):
            desire[index] = atoi(item)
    values += desire
    disposition = match(section.get("disposition"), DISPOSITIONS)
    disposition = disposition - 1 if disposition >= 0 else -1
    return values, disposition


class Rpu:
    """What the converter needs from the game: globals, party members and
    their AI packets."""

    def __init__(self, game: Game):
        self.game = game
        found = game.read("data\\vault13.gam")
        if found is None:
            raise SystemExit(f"vault13.gam not found in {game.folder}")
        self.gam_source = found[1]
        self.globals = []
        for line in found[0].decode("latin1").splitlines():
            m = re.match(r"\s*(\w+)\s*:=\s*(-?\d+)\s*;", line.split("//")[0])
            if m:
                self.globals.append((m.group(1), int(m.group(2))))

        party = game.read("data\\party.txt")
        self.party = [int(p) for p in re.findall(r"party_member_pid\s*=\s*(\d+)",
                                                 party[0].decode("latin1"))] if party else []
        self.lists: dict[str, list[str]] = {}
        self.packets: dict[int, tuple[list[int], int]] = {}
        ai = game.read("data\\ai.txt")
        if ai is not None:
            for section in ini_sections(ai[0].decode("latin1")).values():
                if "packet_num" in section:
                    packet = ai_packet(section)
                    self.packets.setdefault(packet[0][0], packet)

    def proto_entry(self, pid: int) -> str | None:
        kind = {0: "items", 1: "critters"}.get(pid >> 24)
        index = pid & 0xFFFFFF
        if kind is None or index == 0:
            return None
        if kind not in self.lists:
            found = self.game.read(f"proto\\{kind}\\{kind}.lst")
            self.lists[kind] = found[0].decode("latin1").split() if found else []
        names = self.lists[kind]
        return f"proto\\{kind}\\{names[index - 1]}" if index <= len(names) else None

    def saved_packet(self, slot: Path, pid: int) -> tuple[list[int], int] | None:
        """The AI packet the engine saves for a party member, or None when it
        saves none for it (only packets with disposition none/custom)."""
        if pid >> 24 != 1:
            return None
        entry = self.proto_entry(pid)
        if entry is None:
            return None
        loose = find_loose(slot, entry)
        data = ungzip(loose.read_bytes()) if loose else (self.game.read(entry) or (b"",))[0]
        if len(data) < 40:
            raise Skip(f"no proto for party member {pid & 0xFFFFFF}")
        packet = self.packets.get(struct.unpack_from(">i", data, 36)[0])
        return packet if packet is not None and packet[1] == 0 else None


# --- the save -------------------------------------------------------------

def ints(data: bytes, offset: int, count: int) -> tuple[int, ...]:
    return struct.unpack_from(f">{count}i", data, offset)


def globals_in_save(data: bytes, limit: int) -> int | None:
    """Number of globals the save holds: the map list (a count, then
    13-byte .SAV names) follows them."""
    for n in range(1, limit + 1):
        end = GLOBALS_OFFSET + n * 4
        if end + 18 > len(data):
            break
        count, = struct.unpack_from(">i", data, end)
        if 0 < count < 1000 and re.match(rb"[A-Za-z0-9_]{1,8}\.SAV\0", data[end + 4:end + 18]):
            return n
    return None


def worldmap_end(data: bytes, start: int) -> int:
    """End of the world map section (see wmWorldMap_load)."""
    pos = start + 11 * 4
    cities, = ints(data, pos, 1)
    pos += 4
    if not 0 < cities < 1000:
        raise Skip("world map not where expected")
    for _ in range(cities):
        entrances, = ints(data, pos + 16, 1)
        if not 0 <= entrances < 100:
            raise Skip("world map not where expected")
        pos += 20 + entrances * 4
    tiles, _horizontal = ints(data, pos, 2)
    pos += 8 + tiles * 6 * 7 * 4
    counters, = ints(data, pos, 1)
    if not 0 <= counters < 10000:
        raise Skip("world map not where expected")
    return pos + 4 + counters * 12


def convert_globals(data: bytes, rpu: Rpu) -> bytes:
    end = GLOBALS_OFFSET + VANILLA_GLOBALS * 4
    old = list(ints(data, GLOBALS_OFFSET, VANILLA_GLOBALS))
    new = old + [value for _, value in rpu.globals[VANILLA_GLOBALS:]]
    names = [name for name, _ in rpu.globals]
    if names[74] == "GVAR_TORR_BRAHMIN_KILLED" and names[202] != "GVAR_TORR_BRAHMIN_KILLED":
        new[74] = old[202]
        new[202] = rpu.globals[202][1]
        new[29] = rpu.globals[29][1]    # unused in 1.02, Broken Hills in RPU
    # The globals are saved twice: before the map list and right after it.
    old_block = data[GLOBALS_OFFSET:end]
    second = data.find(old_block, end)
    if second == -1:
        raise Skip("second copy of the global variables not found")
    new_block = struct.pack(f">{len(new)}i", *new)
    return (data[:GLOBALS_OFFSET] + new_block + data[end:second]
            + new_block + data[second + len(old_block):])


def convert_party(data: bytes, slot: Path, rpu: Rpu) -> bytes:
    old_party = rpu.party[:VANILLA_PARTY]
    added = rpu.party[VANILLA_PARTY:]
    old_packets = [p for p in (rpu.saved_packet(slot, pid) for pid in old_party) if p]
    new_packets = [p for p in (rpu.saved_packet(slot, pid) for pid in added) if p]

    # Perks (a rank for every perk of every party member), combat state, then
    # the AI packets: found where the packet numbers follow the perks.
    search_from = GLOBALS_OFFSET + 2 * len(rpu.globals) * 4
    perks_size = VANILLA_PARTY * PERK_COUNT * 4
    perks = None
    for pos in range(search_from, len(data) - perks_size - 8):    # map names vary in length
        block = ints(data, pos + perks_size + 4, 1)[0] if old_packets else None
        if old_packets and block != old_packets[0][0][0]:
            continue
        ranks = ints(data, pos, VANILLA_PARTY * PERK_COUNT)
        if min(ranks) < 0 or max(ranks) > 20:
            continue
        ai = pos + perks_size + 4
        if all(ints(data, ai + i * AI_PACKET_INTS * 4, 1)[0] == p[0][0] for i, p in enumerate(old_packets)):
            perks = pos
            break
    if perks is None:
        raise Skip("perks and AI packets not found")
    combat_state, = ints(data, perks + perks_size, 1)
    if combat_state & 1:
        raise Skip("saved during combat; save outside combat and convert that")
    ai_end = perks + perks_size + 4 + len(old_packets) * AI_PACKET_INTS * 4

    # Party members: count, items, their object ids, then level-up info
    # (3 ints) for every party member but the player.
    party = worldmap_end(data, ai_end + AFTER_AI) + AFTER_WORLDMAP
    members, items = ints(data, party, 2)
    # items is the next id for party members' items, from 20000 up
    if not 1 <= members <= 50 or items < 0:
        raise Skip("party members not where expected")
    levels_end = party + 8 + (members - 1) * 4 + (VANILLA_PARTY - 1) * 12
    if levels_end > len(data):
        raise Skip("party members not where expected")

    perk_insert = perks + perks_size
    new_perks = bytes(len(added) * PERK_COUNT * 4)
    new_ai = b"".join(struct.pack(f">{AI_PACKET_INTS}i", *p[0]) for p in new_packets)
    new_levels = bytes(len(added) * 12)
    return (data[:perk_insert] + new_perks + data[perk_insert:ai_end] + new_ai
            + data[ai_end:levels_end] + new_levels + data[levels_end:])


def add_party_protos(slot: Path, rpu: Rpu) -> list[str]:
    """Loading a save copies the proto of every party member from the slot:
    give the ones RPU adds the game's own."""
    added = []
    for pid in rpu.party:
        entry = rpu.proto_entry(pid)
        if entry is None or pid == DUDE_PID or find_loose(slot, entry) is not None:
            continue
        found = rpu.game.read(entry)
        if found is None:
            continue
        target = slot / Path(*entry.split("\\"))
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(found[0])
        added.append(target.name)
    return added


def convert(slot: Path, rpu: Rpu) -> str:
    path = slot / "SAVE.DAT"
    if not path.is_file():
        return "no SAVE.DAT"
    data = path.read_bytes()
    if data[:2] == b"\x1f\x8b":
        return "compressed SAVE.DAT, skipped"
    have = globals_in_save(data, len(rpu.globals))
    if have is None:
        return "global variables not found, skipped"
    if have == len(rpu.globals):
        return f"already matches this game ({have} globals), nothing to do"
    if have != VANILLA_GLOBALS:
        return f"{have} globals: not a 1.02 save, skipped"
    if len(rpu.party) < VANILLA_PARTY:
        return "data/party.txt has fewer party members than 1.02, skipped"
    try:
        data = convert_globals(data, rpu)
        if len(rpu.party) > VANILLA_PARTY:
            data = convert_party(data, slot, rpu)
    except Skip as e:
        return f"{e}, skipped"
    backup = slot / "SAVE.DAT.vanilla"
    if not backup.exists():
        shutil.copyfile(path, backup)
    path.write_bytes(data)
    result = f"converted: {have} -> {len(rpu.globals)} globals"
    if len(rpu.party) > VANILLA_PARTY:
        result += f", {VANILLA_PARTY} -> {len(rpu.party)} party members"
    protos = add_party_protos(slot, rpu)
    if protos:
        result += f" (protos {' '.join(protos)})"
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("game", type=Path, help="the game folder (with master.dat)")
    parser.add_argument("slots", nargs="*", help="slot folders to convert (default: all)")
    parser.add_argument("--restore", action="store_true",
                        help="copy slots back from backup/rpu/data/SAVEGAME first")
    args = parser.parse_args()

    folder = args.game.resolve()
    saves = folder / "data" / "SAVEGAME"
    if args.restore:
        backup = folder / "backup" / "rpu" / "data" / "SAVEGAME"
        if not backup.is_dir():
            sys.exit(f"no saves in {backup}")
        saves.mkdir(parents=True, exist_ok=True)
        for slot in sorted(backup.glob("SLOT*")):
            if (saves / slot.name).exists():
                print(f"{slot.name}: already in data/SAVEGAME, not restored")
            else:
                shutil.copytree(slot, saves / slot.name)
                print(f"{slot.name}: restored")

    rpu = Rpu(Game(folder))
    print(f"vault13.gam: {len(rpu.globals)} globals ({rpu.gam_source}); "
          f"party.txt: {len(rpu.party)} party members")
    slots = [saves / s for s in args.slots] if args.slots else sorted(saves.glob("SLOT*"))
    for slot in slots:
        print(f"{slot.name}: {convert(slot, rpu)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
