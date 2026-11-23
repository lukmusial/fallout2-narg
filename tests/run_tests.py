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


HD_ARGS = ["[graphics]hd=1"]
# Placeholder HD art made by tools/dev/make_test_hd_art.py (main menu background).
HD_ART_ARGS = [*HD_ARGS, "[graphics]hd_pack=test"]


def compare_image(actual_path, golden_name, update, max_diff=0.0, mask=(), upscale=1):
    golden_path = GOLDEN / golden_name
    if upscale == 1 and (update or not golden_path.exists()):
        golden_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(actual_path, golden_path)
        return "golden written"
    actual = load_png(actual_path)
    golden = load_png(golden_path)
    if upscale != 1:
        # HD-mode screenshots are at output resolution; legacy pixels are doubled.
        golden = golden.repeat(upscale, axis=0).repeat(upscale, axis=1)
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


def test_main_menu_hd(update):
    """hd=1 without HD art: the 2x framebuffer is the legacy frame pixel-doubled."""
    out, seconds = run_game("main_menu_hd", "smoke_menu.txt", HD_ARGS)
    return compare_image(out / "mainmenu.png", "main_menu.png", False, mask=MAIN_MENU_MASK, upscale=2)


def nonuniform_blocks(image):
    """Fraction of 2x2 output blocks that are not a single colour (HD detail)."""
    h, w = image.shape[0] // 2, image.shape[1] // 2
    blocks = image[:h * 2, :w * 2].reshape(h, 2, w, 2, 3)
    return float((np.abs(blocks - blocks[:, :1, :, :1, :]).sum(axis=(1, 3, 4)) > 0).mean())


def test_main_menu_hd_art(update):
    """HD layer + compositor: menu background from a 2x PNG, legacy buttons and
    text on top, cursor composited over HD, fade-out modulates HD pixels."""
    out, seconds = run_game("main_menu_hd_art", "menu_fades.txt", HD_ART_ARGS)
    menu = load_png(out / "menu_fb.png")
    detail = nonuniform_blocks(menu)
    if detail < 0.5:
        raise TestFailure(f"HD background not shown ({detail:.0%} of 2x2 blocks have detail)")
    # Cursor rectangle at logical (320, 240): HD must survive around the cursor.
    cursor = nonuniform_blocks(menu[480:530, 640:690])
    if cursor < 0.2:
        raise TestFailure(f"cursor area lost its HD pixels ({cursor:.0%})")
    # Button label "INTRO" (logical 90..160 x 18..38) is legacy text over HD:
    # its text pixels are pixel-doubled.
    label = menu[36:76, 180:320]
    if nonuniform_blocks(label) > 0.9:
        raise TestFailure("button label shows no legacy (pixel-doubled) text")
    ratio = load_png(out / "fade_out_50.png").mean() / menu.mean()
    if not 0.4 < ratio < 0.6:
        raise TestFailure(f"fade-out frame is {ratio:.2f} of full brightness, expected ~0.5")
    note = compare_image(out / "menu_fb.png", "main_menu_hd_art.png", update, mask=MAIN_MENU_MASK)
    return f"{note}, detail {detail:.0%}, cursor {cursor:.0%}, fade {ratio:.2f} ({seconds:.0f}s)"


def test_main_menu_hd_fhd(update):
    """The menu background found as a one-frame .fhd container instead of a PNG."""
    out, seconds = run_game("main_menu_hd_fhd", "smoke_menu.txt", [*HD_ARGS, "[graphics]hd_pack=testfhd"])
    return compare_image(out / "mainmenu.png", "main_menu_hd_art.png", False, mask=MAIN_MENU_MASK) + f" ({seconds:.0f}s)"


def test_hd_art_lookup(update):
    """Pack lookup routes (PNG, .fhd, manifest), legacy 2x fallback, cache."""
    out, seconds = run_game("hd_art_lookup", "hd_art_lookup.txt", HD_ART_ARGS)
    probe = {name: json.loads((out / f"{name}.json").read_text())
             for name in ("probe_png", "probe_fallback", "probe_fhd", "probe_mixed", "probe_mixed_again",
                          "probe_mixed_missing", "probe_none")}
    png = probe["probe_png"]
    if not png["found"] or png["from_legacy"] or (png["width"], png["height"]) != (1280, 960):
        raise TestFailure(f"PNG lookup: {png}")
    reference = np.asarray(Image.open(RUN_DIR / "data/hd/test/art/intrface/mainmenu.png").convert("RGBA"))
    for name in ("probe_png", "probe_fhd", "probe_mixed", "probe_mixed_again"):
        if probe[name]["from_legacy"]:
            raise TestFailure(f"{name}: fell back to legacy ({probe[name]})")
        if not np.array_equal(np.asarray(Image.open(out / f"{name}.png").convert("RGBA")), reference):
            raise TestFailure(f"{name}: image differs from the pack PNG")
    for name in ("probe_fallback", "probe_mixed_missing", "probe_none"):
        p = probe[name]
        if not p["from_legacy"] or (p["width"], p["height"]) != (2 * p["legacy_width"], 2 * p["legacy_height"]):
            raise TestFailure(f"{name}: expected a 2x legacy fallback, got {p}")
    fallback = load_png(out / "probe_fallback.png")
    if nonuniform_blocks(fallback) != 0.0:
        raise TestFailure("legacy fallback is not a pixel-doubled image")
    stats = json.loads((out / "stats.json").read_text())
    if stats["hits"] < 1 or stats["assets_with_hd"] < 1:
        raise TestFailure(f"cache not used: {stats}")
    return f"png/fhd/manifest/fallback ok, cache {stats} ({seconds:.0f}s)"


def make_slot_test(slot, hd=False):
    def test(update):
        reset_saves()
        suffix = "_hd" if hd else ""
        out, seconds = run_game(f"slot{slot:02d}{suffix}", "load_slot.txt",
                                [f"--dev-load-game={slot}", *(HD_ARGS if hd else [])])
        notes = [compare_state(out / "state.json", f"slot{slot:02d}_state.json", update and not hd)]
        notes.append(compare_image(out / "game.png", f"slot{slot:02d}_game.png", update and not hd, LIVE_VIEW_MAX_DIFF,
                                   upscale=2 if hd else 1))
        return ", ".join(notes) + f" ({seconds:.0f}s)"
    test.__name__ = f"test_load_slot{slot:02d}{'_hd' if hd else ''}"
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



# HD mode (no HD art yet) on a sample of saves: must match the doubled legacy frame.
HD_SLOTS = (1, 5, 9)


def test_menu_fades(update):
    """The presenter's framebuffer matches the window composition, and fades run
    at 8-bit precision (within the 6-bit quantisation of the old presenter)."""
    out, seconds = run_game("menu_fades", "menu_fades.txt")
    for fb, shot in (("menu_fb", "menu"), ("load_fb", "load")):
        if not np.array_equal(load_png(out / f"{fb}.png"), load_png(out / f"{shot}.png")):
            raise TestFailure(f"{fb}.png differs from the window composition {shot}.png")
    for name in ("fade_out_50", "fade_in_25", "fade_in_50"):
        fade = load_png(out / f"{name}.png")
        legacy = load_png(out / f"{name}_6bit.png")
        if fade.shape != legacy.shape:
            raise TestFailure(f"{name}: size {fade.shape} != {legacy.shape}")
        # 6-bit steps truncate (< 4 levels at 8 bits) and lack bit replication
        # (<= 3 levels), so the two may differ by up to 6.
        worst = int(np.abs(fade - legacy).max())
        if worst > 6:
            raise TestFailure(f"{name}: differs from the 6-bit fade by {worst} levels")
        if fade.max() < 16:
            raise TestFailure(f"{name}: frame is black, fade not captured mid-way")
    return f"framebuffer matches, fades captured ({seconds:.0f}s)"


TESTS = ([test_main_menu, test_menu_fades, test_main_menu_hd, test_main_menu_hd_art, test_main_menu_hd_fhd,
          test_hd_art_lookup]
         + [make_slot_test(slot) for slot in range(1, SLOT_COUNT + 1)]
         + [make_slot_test(slot, hd=True) for slot in HD_SLOTS]
         + [test_stimpack_save_unchanged, test_character_screens, test_stimpack_edit_player, test_stimpack_cancel])


PERF_CONFIGS = [
    ("1280x960 scale 2", ["[screen]resolution_x=1280", "[screen]resolution_y=960", "[screen]scale=2"]),
    ("3440x1440 scale 2", ["[screen]resolution_x=3440", "[screen]resolution_y=1440", "[screen]scale=2"]),
    ("3440x1440 hd=1", ["[screen]resolution_x=3440", "[screen]resolution_y=1440", "[screen]scale=2", *HD_ART_ARGS]),
]


def run_perf(slot):
    """Presenter timings (not a pass/fail test): idle map and edge scrolling."""
    print(f"presenter timings, slot {slot} (avg/max ms per renderPresent; convert = index->RGB total ms)")
    for label, overrides in PERF_CONFIGS:
        reset_saves()
        out, _ = run_game("perf_" + label.replace(" ", "_").replace("=", ""), "perf_game.txt", [f"--dev-load-game={slot}", *overrides])
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
