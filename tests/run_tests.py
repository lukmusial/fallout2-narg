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


# The main menu footer prints the build hash and build date, which change with
# every commit; that strip is excluded from the comparison (logical 640x480 coords).
MAIN_MENU_MASK = [(300, 446, 640, 472)]


def compare_image(actual_path, golden_name, update, max_diff=0.0, mask=()):
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
    scale = actual.shape[1] / 640
    for left, top, right, bottom in mask:
        differs[int(top * scale):int(bottom * scale), int(left * scale):int(right * scale)] = False
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
    return compare_image(out / "mainmenu.png", "main_menu.png", update, mask=MAIN_MENU_MASK)


def make_slot_test(slot):
    def test(update):
        reset_saves()
        out, seconds = run_game(f"slot{slot:02d}", "load_slot.txt", [f"--dev-load-game={slot}"])
        notes = [compare_state(out / "state.json", f"slot{slot:02d}_state.json", update)]
        notes.append(compare_image(out / "game.png", f"slot{slot:02d}_game.png", update, LIVE_VIEW_MAX_DIFF))
        return ", ".join(notes) + f" ({seconds:.0f}s)"
    test.__name__ = f"test_load_slot{slot:02d}"
    return test


def test_menu_fades(update):
    """The presenter's framebuffer matches the window composition, and fades run
    at 8-bit precision (within one 6-bit step of the old presenter)."""
    out, seconds = run_game("menu_fades", "menu_fades.txt")
    for fb, shot in (("menu_fb", "menu"), ("load_fb", "load")):
        if not np.array_equal(load_png(out / f"{fb}.png"), load_png(out / f"{shot}.png")):
            raise TestFailure(f"{fb}.png differs from the window composition {shot}.png")
    for name in ("fade_out_50", "fade_in_25", "fade_in_50"):
        fade = load_png(out / f"{name}.png")
        legacy = load_png(out / f"{name}_6bit.png")
        if fade.shape != legacy.shape:
            raise TestFailure(f"{name}: size {fade.shape} != {legacy.shape}")
        worst = int(np.abs(fade - legacy).max())
        if worst > 4:
            raise TestFailure(f"{name}: differs from the 6-bit fade by {worst} levels")
        if fade.max() < 16:
            raise TestFailure(f"{name}: frame is black, fade not captured mid-way")
    return f"framebuffer matches, fades captured ({seconds:.0f}s)"


TESTS = [test_main_menu, test_menu_fades] + [make_slot_test(slot) for slot in range(1, SLOT_COUNT + 1)]


PERF_CONFIGS = [
    ("1280x960 scale 2", ["[screen]resolution_x=1280", "[screen]resolution_y=960", "[screen]scale=2"]),
    ("3440x1440 scale 2", ["[screen]resolution_x=3440", "[screen]resolution_y=1440", "[screen]scale=2"]),
]


def run_perf(slot):
    """Presenter timings (not a pass/fail test): idle map and edge scrolling."""
    print(f"presenter timings, slot {slot} (avg/max ms per renderPresent; convert = index->RGB total ms)")
    for label, overrides in PERF_CONFIGS:
        reset_saves()
        out, _ = run_game("perf_" + label.split()[0], "perf_game.txt", [f"--dev-load-game={slot}", *overrides])
        for phase in ("idle", "scroll"):
            stats = json.loads((out / f"{phase}.json").read_text())
            fb = "x".join(str(v) for v in stats["framebuffer"])
            print(f"  {label:20s} fb {fb:9s} {phase:6s} presents {stats['presents']:5d}  "
                  f"present avg {stats['present_avg_ms']:6.3f} max {stats['present_max_ms']:7.3f}  "
                  f"convert {stats['convert_total_ms']:8.1f} ms / {stats['converted_pixels'] / 1e6:6.1f} Mpx  "
                  f"palette {stats['palette_total_ms']:7.1f} ms / {stats['palette_updates']} updates")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--perf", type=int, metavar="SLOT", help="print presenter timings for a save slot and exit")
    parser.add_argument("--update-golden", action="store_true", help="overwrite golden files with this run's output")
    parser.add_argument("-k", dest="pattern", help="run only tests whose name contains this text")
    args = parser.parse_args()

    if not EXE.exists():
        sys.exit(f"game not built: {EXE}")

    if args.perf:
        run_perf(args.perf)
        reset_saves()
        return 0

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
