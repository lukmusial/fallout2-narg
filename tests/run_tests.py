#!/usr/bin/env python3
"""Integration tests that drive the real game with the --dev-script harness.

Run through tools/dev/run-tests.sh (which builds and prepares the run directory).

  run_tests.py [--update-golden] [-k NAME]

Each test launches the game, follows a script from tests/scripts/, then checks
state dumps (exactly, on the fields that are deterministic) and screenshots
(main menu exactly, live game views with a tolerance) against testdata/golden/.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
RUN_DIR = Path(os.environ.get("RUN_DIR", ROOT / "out/build/macos"))
EXE = RUN_DIR / "Fallout II Community Edition.app/Contents/MacOS/Fallout II Community Edition"
SCRIPTS = ROOT / "tests/scripts"
GOLDEN = ROOT / "testdata/golden"
OUT = RUN_DIR / "devout/tests"
SLOT_COUNT = 10
GAME_TIMEOUT = 120

# Live map views animate (NPCs walk, lights cycle), so only a gross mismatch fails.
LIVE_VIEW_MAX_DIFF = 0.25


class TestFailure(Exception):
    pass


def reset_saves():
    subprocess.run([str(ROOT / "tools/dev/reset-saves.sh")], check=True, stdout=subprocess.DEVNULL)


def run_game(name, script, extra_args=()):
    out_dir = OUT / name
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)
    args = [str(EXE), f"--dev-script={SCRIPTS / script}", f"--dev-out={out_dir}", *extra_args]
    started = time.time()
    try:
        result = subprocess.run(args, cwd=RUN_DIR, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                timeout=GAME_TIMEOUT, text=True)
    except subprocess.TimeoutExpired:
        raise TestFailure(f"game did not finish within {GAME_TIMEOUT}s")
    if result.returncode != 0:
        log = RUN_DIR / "debug.log"
        tail = log.read_text(errors="replace").splitlines()[-5:] if log.exists() else []
        raise TestFailure(f"game exited with {result.returncode}: {result.stderr.strip()} | " + " / ".join(tail))
    return out_dir, time.time() - started


def load_png(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def compare_image(actual_path, golden_name, update, max_diff=0.0, ignore=()):
    """`ignore`: (x0, y0, x1, y1) rectangles excluded from the comparison."""
    golden_path = GOLDEN / golden_name
    if update or not golden_path.exists():
        golden_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(actual_path, golden_path)
        return "golden written"
    actual = load_png(actual_path)
    golden = load_png(golden_path)
    if actual.shape != golden.shape:
        raise TestFailure(f"{golden_name}: size {actual.shape} != golden {golden.shape}")
    differs = np.any(actual != golden, axis=2)
    for x0, y0, x1, y1 in ignore:
        differs[y0:y1, x0:x1] = False
    changed = differs.mean()
    if changed > max_diff:
        raise TestFailure(f"{golden_name}: {changed:.1%} of pixels differ (limit {max_diff:.0%})")
    return f"{changed:.1%} pixels differ"


def stable_state(state):
    """Fields of a state dump that do not depend on timing."""
    def critter(c):
        return {k: c[k] for k in ("pid", "name", "base_stats", "bonus_stats", "skills", "perks")}

    player = critter(state["player"])
    for key in ("pc_stats", "tagged_skills", "traits"):
        player[key] = state["player"][key]
    world = {k: state["world"][k] for k in ("area", "map", "map_name", "elevation", "x", "y")}
    return {
        "player": player,
        "party": sorted((critter(m) for m in state["party"]), key=lambda c: c["pid"]),
        "world": world,
        "gvars": state["gvars"],
    }


def compare_state(actual_path, golden_name, update):
    golden_path = GOLDEN / golden_name
    actual = stable_state(json.loads(actual_path.read_text()))
    if update or not golden_path.exists():
        golden_path.parent.mkdir(parents=True, exist_ok=True)
        golden_path.write_text(json.dumps(actual, indent=1) + "\n")
        return "golden written"
    golden = json.loads(golden_path.read_text())
    if actual != golden:
        diffs = [k for k in golden if golden.get(k) != actual.get(k)]
        raise TestFailure(f"{golden_name}: state differs in {diffs}")
    return "state matches"


# --- tests -----------------------------------------------------------------

def test_main_menu(update):
    out, seconds = run_game("main_menu", "smoke_menu.txt")
    # Bottom-right footer shows the build hash and date, which change every build.
    return compare_image(out / "mainmenu.png", "main_menu.png", update, ignore=[(400, 430, 640, 480)])


def make_slot_test(slot):
    def test(update):
        reset_saves()
        out, seconds = run_game(f"slot{slot:02d}", "load_slot.txt", [f"--dev-load-game={slot}"])
        notes = [compare_state(out / "state.json", f"slot{slot:02d}_state.json", update)]
        notes.append(compare_image(out / "game.png", f"slot{slot:02d}_game.png", update, LIVE_VIEW_MAX_DIFF))
        return ", ".join(notes) + f" ({seconds:.0f}s)"
    test.__name__ = f"test_load_slot{slot:02d}"
    return test


def slot_description(slot):
    header = (RUN_DIR / f"data/SAVEGAME/SLOT{slot:02d}/SAVE.DAT").read_bytes()[:0x5B]
    return header[0x3D:0x5B].split(b"\0")[0].decode("latin-1")


def test_stimpack_save_unchanged(update):
    """Stimpack: open slot 3, Save As without edits; the new slot must equal slot 3."""
    reset_saves()
    out, seconds = run_game("stimpack_save_as", "stimpack_save_as.txt")
    notes = [compare_image(out / "hub.png", "stimpack_hub.png", update, LIVE_VIEW_MAX_DIFF)]
    if slot_description(11) != "stimpack test":
        raise TestFailure(f"slot 11 description is {slot_description(11)!r}")
    out11, _ = run_game("stimpack_load11", "load_slot.txt", ["--dev-load-game=11"])
    # Same golden as slot 3: the re-save must not change any state.
    notes.append(compare_state(out11 / "state.json", "slot03_state.json", False))
    return ", ".join(notes) + f" ({seconds:.0f}s)"


def test_character_screens(update):
    """The normal level-up and creation screens must look as before Stimpack."""
    reset_saves()
    out, _ = run_game("char_levelup", "char_screens.txt", ["--dev-load-game=1"])
    notes = [compare_image(out / "levelup.png", "character_levelup.png", update)]
    out, _ = run_game("char_creation", "creation_screen.txt")
    notes.append(compare_image(out / "creation.png", "character_creation.png", update))
    return ", ".join(notes)


def test_stimpack_edit_player(update):
    """Stimpack: ST 7->10, Small Guns +10 clicks, one perk, level 12 on slot 1, saved as slot 11."""
    reset_saves()
    out, seconds = run_game("stimpack_edit_player", "stimpack_edit_player.txt")
    notes = [compare_image(out / "editor_final.png", "stimpack_editor_final.png", update, 0.01)]
    if slot_description(11) != "stimpack edit":
        raise TestFailure(f"slot 11 description is {slot_description(11)!r}")
    out11, _ = run_game("stimpack_edit_load11", "load_slot.txt", ["--dev-load-game=11"])
    notes.append(compare_state(out11 / "state.json", "stimpack_edit_player_state.json", update))

    # Everything except the edited player values must still match slot 1.
    original = json.loads((GOLDEN / "slot01_state.json").read_text())
    edited = stable_state(json.loads((out11 / "state.json").read_text()))
    for key in ("party", "world", "gvars"):
        if original[key] != edited[key]:
            raise TestFailure(f"Stimpack changed {key}")
    for key in ("name", "tagged_skills", "traits"):
        if original["player"][key] != edited["player"][key]:
            raise TestFailure(f"Stimpack changed player {key}")
    return ", ".join(notes) + f" ({seconds:.0f}s)"


def test_stimpack_cancel(update):
    """Stimpack: Cancel in the player editor reverts stats and perks."""
    reset_saves()
    run_game("stimpack_cancel", "stimpack_cancel.txt")
    out11, _ = run_game("stimpack_cancel_load11", "load_slot.txt", ["--dev-load-game=11"])
    return compare_state(out11 / "state.json", "slot01_state.json", False)


TESTS = [test_main_menu] + [make_slot_test(slot) for slot in range(1, SLOT_COUNT + 1)] + [
    test_stimpack_save_unchanged,
    test_character_screens,
    test_stimpack_edit_player,
    test_stimpack_cancel,
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--update-golden", action="store_true", help="overwrite golden files with this run's output")
    parser.add_argument("-k", dest="pattern", help="run only tests whose name contains this text")
    args = parser.parse_args()

    if not EXE.exists():
        sys.exit(f"game not built: {EXE}")

    selected = [t for t in TESTS if not args.pattern or args.pattern in t.__name__]
    failures = 0
    for test in selected:
        try:
            note = test(args.update_golden)
            print(f"PASS {test.__name__}: {note}", flush=True)
        except TestFailure as error:
            failures += 1
            print(f"FAIL {test.__name__}: {error}", flush=True)
    reset_saves()
    print(f"{len(selected) - failures}/{len(selected)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
