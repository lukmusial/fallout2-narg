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
import threading
from concurrent.futures import ThreadPoolExecutor
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


class TestSkipped(Exception):
    pass


# --- parallel workers ------------------------------------------------------------
# Tests run concurrently, each in its own worker directory: an APFS clone of the
# app next to symlinks to the game data and HD packs, with its own saves and
# outputs (the game always runs from the folder that contains the .app).

APP_NAME = "Fallout II Community Edition.app"
_worker = threading.local()


def run_dir():
    return getattr(_worker, "run_dir", RUN_DIR)


def prepare_worker(index):
    worker = RUN_DIR / f"workers/w{index}"
    if worker.exists():
        shutil.rmtree(worker)
    (worker / "data/hd").mkdir(parents=True)
    # APFS clone: instant and takes no space until written.
    subprocess.run(["cp", "-cR", str(RUN_DIR / APP_NAME), str(worker / APP_NAME)], check=True)
    for name in ("master.dat", "critter.dat", "patch000.dat", "f2_res.dat", "sound", "ce.dat", "mods"):
        if (RUN_DIR / name).exists():
            (worker / name).symlink_to((RUN_DIR / name).resolve())
    for pack in (RUN_DIR / "data/hd").glob("*"):
        (worker / "data/hd" / pack.name).symlink_to(pack.resolve())
    shutil.copyfile(RUN_DIR / "fallout2.cfg", worker / "fallout2.cfg")
    return worker


def reset_saves():
    data = run_dir() / "data"
    for name in ("SAVEGAME", "maps", "MAPS", "proto"):
        shutil.rmtree(data / name, ignore_errors=True)
    (data / "worldmap.dat").unlink(missing_ok=True)
    shutil.copytree(ROOT / "testdata/SAVEGAME", data / "SAVEGAME")


def run_game(name, script, extra_args=(), fast=True):
    """Runs the game hidden with a script. `fast` makes palette fades instant;
    tests that measure fades pass fast=False."""
    directory = run_dir()
    out_dir = directory / "devout/tests" / name
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)
    exe = directory / APP_NAME / "Contents/MacOS/Fallout II Community Edition"
    args = [str(exe), f"--dev-script={SCRIPTS / script}", f"--dev-out={out_dir}", *(["--dev-fast"] if fast else []), *extra_args]
    started = time.time()
    try:
        result = subprocess.run(args, cwd=directory, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                timeout=GAME_TIMEOUT, text=True)
    except subprocess.TimeoutExpired:
        raise TestFailure(f"game did not finish within {GAME_TIMEOUT}s")
    if result.returncode != 0:
        log = directory / "debug.log"
        tail = log.read_text(errors="replace").splitlines()[-5:] if log.exists() else []
        raise TestFailure(f"game exited with {result.returncode}: {result.stderr.strip()} | " + " / ".join(tail))
    return out_dir, time.time() - started


def load_png(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


# The main menu footer prints the build hash and build date, which change with
# every commit; that strip is excluded from the comparison (logical 640x480 coords).
MAIN_MENU_MASK = [(300, 446, 640, 472)]


# HD text (Phase 2.9) is on by default with hd=1; the earlier HD tests check
# art and compositing against their own goldens with it off.
HD_ARGS = ["[graphics]hd=1", "[graphics]hd_text=0"]
HD_TEXT_ARGS = ["[graphics]hd=1", "[graphics]hd_text=1"]
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
    out, seconds = run_game("main_menu_hd_art", "menu_fades.txt", HD_ART_ARGS, fast=False)
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
    # (Same bounds as the HD screens tests: the step captured varies with load.)
    ratio = load_png(out / "fade_out_50.png").mean() / menu.mean()
    if not 0.3 < ratio < 0.7:
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
    reference = np.asarray(Image.open(run_dir() / "data/hd/test/art/intrface/mainmenu.png").convert("RGBA"))
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


# --- Phase 2.6: interface screens with the real HD packs --------------------
#
# The packs come from tools/hd (enhance_scripted.py, enhance_ai.py, then
# install.py copies them to run_dir()/data/hd/<pack>/); they are derived from the
# game's art and never committed, so these tests are skipped without them.

HD_SCREENS = ("menu", "load", "hub", "editor", "editor_slider", "perks", "creation")
# Screens with a golden per pack (full frame at 1280x960).
HD_SCREEN_GOLDENS = ("menu", "load", "hub", "editor")
# Rectangles (logical 640x480) of static art that must show HD detail.
HD_ART_REGIONS = {
    "menu": [(300, 40, 620, 400)],            # power armour background
    "load": [(330, 30, 620, 200), (420, 400, 560, 470)],  # frame, vault-boy plate
    "hub": [(238, 76, 401, 112)],            # panel header plate
    "editor": [(10, 30, 180, 270), (340, 260, 630, 300)],  # SPECIAL plates, skill points plate
    "editor_slider": [(10, 30, 180, 270)],
    "perks": [(40, 300, 290, 340), (305, 140, 600, 320)],  # Done/Rank plate, card parchment
    "creation": [(10, 30, 180, 270)],
}
# Legacy text colours (8-bit presented RGB) and where to look for them: these
# pixels must stay exactly the legacy (pixel-doubled) colour over HD art.
TEXT_COLOURS = [(186, 158, 40), (60, 251, 0), (255, 255, 125)]
HD_TEXT_REGIONS = {
    "menu": [(60, 15, 200, 300)],             # button labels
    "load": [(40, 20, 300, 440), (390, 250, 560, 320)],  # title, slot list, info box
    "hub": [(238, 76, 401, 404)],
    "editor": [(180, 30, 340, 300), (370, 20, 640, 240)],  # derived stats, skills
    "creation": [(180, 30, 340, 300)],
    "perks": [(70, 165, 280, 300)],           # perk list
}
HD_GOLDEN_MAX_DIFF = 0.01  # fraction of pixels differing by more than 12 levels


def pack_installed(pack):
    return (run_dir() / f"data/hd/{pack}/art/intrface/mainmenu_sp.png").exists()


def blocks_detail(image, region):
    left, top, right, bottom = region
    return nonuniform_blocks(image[top * 2:bottom * 2, left * 2:right * 2])


def legacy_text_kept(fb, legacy, region):
    """Fraction of legacy text pixels in a region that the HD frame shows
    unchanged (pixel-doubled), and how many there are."""
    left, top, right, bottom = region
    ref = legacy[top:bottom, left:right]
    mask = np.zeros(ref.shape[:2], dtype=bool)
    for colour in TEXT_COLOURS:
        mask |= np.all(ref == np.array(colour), axis=2)
    if not mask.any():
        return 1.0, 0
    hd = fb[top * 2:bottom * 2, left * 2:right * 2].reshape(bottom - top, 2, right - left, 2, 3)
    same = np.all(hd == ref[:, None, :, None, :], axis=(1, 3, 4))
    return float(same[mask].mean()), int(mask.sum())


def compare_hd_golden(actual_path, golden_name, update, mask=()):
    golden_path = GOLDEN / golden_name
    if update or not golden_path.exists():
        golden_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(actual_path, golden_path)
        return "golden written"
    actual = load_png(actual_path)
    golden = load_png(golden_path)
    if actual.shape != golden.shape:
        raise TestFailure(f"{golden_name}: size {actual.shape} != golden {golden.shape}")
    differs = np.abs(actual - golden).max(axis=2) > 12
    for left, top, right, bottom in mask:
        differs[top * 2:bottom * 2, left * 2:right * 2] = False
    changed = differs.mean()
    if changed > HD_GOLDEN_MAX_DIFF:
        raise TestFailure(f"{golden_name}: {changed:.2%} of pixels differ (limit {HD_GOLDEN_MAX_DIFF:.0%})")
    return f"{changed:.2%}"


def screenshots_in_run_dir():
    return set(run_dir().glob("scr*.png")) | set(run_dir().glob("scr*.bmp"))


_hd_screens_legacy_out = None


def hd_screens_legacy_reference():
    """The same script with hd=0: the plain 640x480 frames (run once per session)."""
    global _hd_screens_legacy_out
    if _hd_screens_legacy_out is None:
        before = screenshots_in_run_dir()
        reset_saves()
        _hd_screens_legacy_out, _ = run_game("hd_screens_legacy", "hd_screens.txt", fast=False)
        for path in screenshots_in_run_dir() - before:
            path.unlink()
    return _hd_screens_legacy_out


def make_hd_screens_test(pack):
    def test(update):
        """Main menu, load screen, Stimpak hub and editor, perk dialog, creation
        screen with a real HD pack: HD detail on the art, legacy text intact,
        fade, named-PNG lookup, F12 full-resolution screenshot, goldens."""
        if not pack_installed(pack):
            raise TestSkipped(f"pack '{pack}' not installed (tools/hd: enhance_*.py, install.py)")
        legacy_out = hd_screens_legacy_reference()
        before = screenshots_in_run_dir()
        reset_saves()
        out, seconds = run_game(f"hd_screens_{pack}", "hd_screens.txt", [*HD_ARGS, f"[graphics]hd_pack={pack}"], fast=False)
        new_shots = sorted(screenshots_in_run_dir() - before)
        try:
            notes = []
            for screen in HD_SCREENS:
                fb = load_png(out / f"{screen}.png")
                legacy = load_png(legacy_out / f"{screen}.png")
                if fb.shape != (960, 1280, 3) or legacy.shape != (480, 640, 3):
                    raise TestFailure(f"{screen}: unexpected sizes {fb.shape} / {legacy.shape}")
                for region in HD_ART_REGIONS[screen]:
                    detail = blocks_detail(fb, region)
                    if detail < 0.3:
                        raise TestFailure(f"{screen}: no HD detail in {region} ({detail:.0%} of 2x2 blocks)")
                for region in HD_TEXT_REGIONS.get(screen, ()):
                    kept, count = legacy_text_kept(fb, legacy, region)
                    if count < 50 or kept < 0.98:
                        raise TestFailure(f"{screen}: legacy text in {region}: {kept:.1%} of {count} pixels kept")
            notes.append(f"{len(HD_SCREENS)} screens HD + text ok")

            # Fades modulate the HD pixels (the step captured can vary when the machine is busy).
            ratio = load_png(out / "fade_out_50.png").mean() / load_png(out / "menu.png").mean()
            if not 0.3 < ratio < 0.7:
                raise TestFailure(f"fade-out frame is {ratio:.2f} of full brightness, expected ~0.5")
            notes.append(f"fade {ratio:.2f}")

            probe = json.loads((out / "probe_named.json").read_text())
            if not probe["found"] or probe["from_legacy"] or (probe["width"], probe["height"]) != (1280, 960):
                raise TestFailure(f"named PNG lookup: {probe}")
            reference = np.asarray(Image.open(run_dir() / f"data/hd/{pack}/art/intrface/mainmenu_sp.png").convert("RGBA"))
            if not np.array_equal(np.asarray(Image.open(out / "probe_named.png").convert("RGBA")), reference):
                raise TestFailure("named PNG lookup: image differs from the pack PNG")

            if len(new_shots) != 1 or new_shots[0].suffix != ".png":
                raise TestFailure(f"F12 wrote {[p.name for p in new_shots]}, expected one PNG")
            shot = load_png(new_shots[0])
            if shot.shape != (960, 1280, 3) or nonuniform_blocks(shot) < 0.5:
                raise TestFailure(f"F12 screenshot is {shot.shape}, detail {nonuniform_blocks(shot):.0%}")
            notes.append("named PNG + F12 ok")

            goldens = [compare_hd_golden(out / f"{screen}.png", f"hd_{pack}_{screen}.png", update,
                                         MAIN_MENU_MASK if screen == "menu" else ())
                       for screen in HD_SCREEN_GOLDENS]
            notes.append("goldens " + "/".join(goldens))
        finally:
            for path in new_shots:
                path.unlink()
        return ", ".join(notes) + f" ({seconds:.0f}s)"
    test.__name__ = f"test_hd_screens_{pack}"
    return test


# --- Phase 2.7/2.8: the map with the real HD packs ---------------------------
#
# tests/scripts/hd_map.txt loads a save with hd=1 and a pack, captures the
# frame, then runs `hdcheck` (tileHdCheck in src/tile.cc) before and after
# scrolling: the iso window's HD layer is compared with a fresh redraw of the
# same view (stale pixels) and with a redraw without objects. Floors, roofs,
# walls, scenery and items are HD; critters and other objects stay legacy.

HD_FLOOR_MIN_TILE_HD = 0.97     # floor/roof pixels (no object drawn there) showing HD
HD_MIN_SCENERY_HD = 0.95        # wall/scenery/item pixels showing HD (not: colour cycling)
HD_MIN_SCENERY_DETAIL = 0.25    # of those, 2x2 output blocks with HD detail
HD_FLOOR_MAX_COLOUR_DIFF = 12.0  # mean abs difference, HD frame box-downsampled vs the legacy golden
HD_FLOOR_GOLDEN_MAX_DIFF = 0.05  # live views animate: fraction of pixels off by > 12 levels
HD_FLOOR_MAX_STALE = 16          # isolated pixels on scroll-strip edges, see hd_check
HD_MAP_PACKS = ("scripted", "ai", "mixed")


def map_pack_installed(pack):
    """Area tiles and objects (one-image and multi-frame art) in the pack;
    `mixed` takes them from both."""
    packs = ("scripted", "ai") if pack == "mixed" else (pack,)
    return all((RUN_DIR / f"data/hd/{p}/art/{art}").exists() for p in packs
               for art in ("tiles/edg5000.png", "walls/adb1026.png", "scenery/door1.fhd"))


def hd_check(out, name):
    check = json.loads((out / f"{name}.json").read_text())
    if not check["active"]:
        raise TestFailure(f"{name}: no HD layer on the map")
    if check["legacy_object_pixels_hd"] != 0 or check["cycling_hd"] != 0:
        raise TestFailure(f"{name}: {check['legacy_object_pixels_hd']} critter/misc object pixels and "
                          f"{check['cycling_hd']} colour-cycling pixels show HD (see {out / name}.png)")
    # Stale = same legacy index as a fresh redraw but different HD. The
    # legacy engine's partial redraws after a scroll leave a few single
    # pixels on strip edges that differ from a fresh redraw (an overlapping
    # tile the strip's tile range misses); where that tile's pixel has the
    # same palette index only the HD side shows it. Missed scrolls or
    # invalidations show up as whole strips or rectangles instead, so only
    # a handful of isolated pixels is accepted.
    mask = np.asarray(Image.open(out / f"{name}.png").convert("RGB"))
    stale = np.all(mask == (255, 255, 0), axis=2)
    neighbours = sum(np.roll(np.roll(stale, dy, 0), dx, 1)
                     for dy in (-1, 0, 1) for dx in (-1, 0, 1) if dy or dx)
    clustered = int((stale & (neighbours >= 2)).sum())
    if stale.sum() > HD_FLOOR_MAX_STALE or clustered:
        raise TestFailure(f"{name}: {int(stale.sum())} stale HD pixels ({clustered} clustered) "
                          f"vs a fresh redraw (see {out / name}.png)")
    share = check["tile_pixels_hd"] / max(check["tile_pixels"], 1)
    if share < HD_FLOOR_MIN_TILE_HD:
        raise TestFailure(f"{name}: only {share:.1%} of floor/roof pixels show HD")
    scenery = check["scenery_pixels_hd"] / max(check["scenery_pixels"], 1)
    if check["scenery_pixels"] > 1000 and scenery < HD_MIN_SCENERY_HD:
        raise TestFailure(f"{name}: only {scenery:.1%} of wall/scenery/item pixels show HD")
    return check, share, scenery


def scenery_detail(fb, out, name):
    """Fraction of the 2x2 output blocks of HD wall/scenery/item pixels
    (green in the hdcheck mask) that are not one flat colour."""
    mask = np.all(np.asarray(Image.open(out / f"{name}.png").convert("RGB")) == (0, 200, 0), axis=2)
    h, w = mask.shape
    blocks = fb[:h * 2, :w * 2].reshape(h, 2, w, 2, 3)
    detail = np.abs(blocks - blocks[:, :1, :, :1, :]).sum(axis=(1, 3, 4)) > 0
    return float(detail[mask].mean()) if mask.any() else 1.0


def make_hd_map_test(pack, slot):
    def test(update):
        """One save with a real HD pack: HD floors, roofs, walls, scenery and
        items; critters and colour cycling legacy; nothing stale after
        keyboard and edge scrolling; lighting close to the legacy frame;
        state unchanged; golden with tolerance."""
        if not map_pack_installed(pack):
            raise TestSkipped(f"pack '{pack}' lacks the area art (tools/hd: extract.py --areas --objects, "
                              "enhance_*.py, install.py)")
        reset_saves()
        out, seconds = run_game(f"hd_map_{pack}_slot{slot:02d}", "hd_map.txt",
                                [f"--dev-load-game={slot}", *HD_ARGS, f"[graphics]hd_pack={pack}"])
        compare_state(out / "state.json", f"slot{slot:02d}_state.json", False)
        check, share, scenery = hd_check(out, "check")
        hd_check(out, "scrolled")
        fb = load_png(out / "game.png")
        if fb.shape != (960, 1280, 3):
            raise TestFailure(f"slot {slot}: frame is {fb.shape}")
        legacy = load_png(GOLDEN / f"slot{slot:02d}_game.png")
        small = fb.reshape(480, 2, 640, 2, 3).mean(axis=(1, 3))
        colour = float(np.abs(small - legacy).mean())
        if colour > HD_FLOOR_MAX_COLOUR_DIFF:
            raise TestFailure(f"slot {slot}: HD frame differs from the legacy view by {colour:.1f} levels on average")
        detail = scenery_detail(fb, out, "check")
        if check["scenery_pixels"] > 1000 and detail < HD_MIN_SCENERY_DETAIL:
            raise TestFailure(f"slot {slot}: only {detail:.0%} of HD wall/scenery blocks show detail")
        golden = compare_hd_golden_live(out / "game.png", f"hd_{pack}_slot{slot:02d}.png", update)
        return (f"floors/roofs {share:.0%} HD, walls/scenery/items {scenery:.0%} HD ({detail:.0%} detail), "
                f"colour diff {colour:.1f}, golden {golden} ({seconds:.0f}s)")
    test.__name__ = f"test_hd_map_{pack}_slot{slot:02d}"
    return test


# --- Phase 2.9: text at 2x ----------------------------------------------------
#
# With hd_text=1 every string is drawn a second time with 2x glyphs into the
# window's HD layer; the 8-bit buffers must not change at all. Text regions
# (logical 640x480) are checked for 2x detail: the share of the legacy text
# pixels whose 2x2 output block is not a single colour.

HD_TEXT_STYLES = ("smooth", "sharp")
HD_TEXT_SCREENS = ("menu", "load", "hub", "editor")
# Minimum share of text pixels with 2x detail. Smooth re-rasterises every
# edge; sharp (EPX) only changes diagonal steps and corners, so on the bold
# menu font few blocks change, but with hd_text=0 the share is exactly 0.
HD_TEXT_MIN_DETAIL = {"smooth": 0.25, "sharp": 0.03}


def text_pixels(legacy, region):
    """Mask of the legacy text pixels (TEXT_COLOURS) in a region."""
    left, top, right, bottom = region
    ref = legacy[top:bottom, left:right]
    mask = np.zeros(ref.shape[:2], dtype=bool)
    for colour in TEXT_COLOURS:
        mask |= np.all(ref == np.array(colour), axis=2)
    return mask


def text_detail(fb, legacy, region):
    """Share of legacy text pixels shown as a non-uniform 2x2 block, and
    how many text pixels there are."""
    left, top, right, bottom = region
    mask = text_pixels(legacy, region)
    if not mask.any():
        return 0.0, 0
    blocks = fb[top * 2:bottom * 2, left * 2:right * 2].reshape(bottom - top, 2, right - left, 2, 3)
    uniform = np.all(blocks == blocks[:, :1, :, :1, :], axis=(1, 3, 4))
    return float((~uniform)[mask].mean()), int(mask.sum())


def make_hd_text_screens_test(style):
    def test(update):
        """Main menu, load screen, Stimpak hub and editor with 2x text over
        the scripted pack: text regions show 2x detail, the 8-bit frames are
        exactly the hd=0 frames, fades dim the text, goldens."""
        if not pack_installed("scripted"):
            raise TestSkipped("pack 'scripted' not installed (tools/hd: enhance_*.py, install.py)")
        reset_saves()
        legacy_out, _ = run_game(f"hd_text_{style}_legacy", "hd_text_screens.txt", fast=False)
        reset_saves()
        out, seconds = run_game(f"hd_text_{style}", "hd_text_screens.txt",
                                [*HD_TEXT_ARGS, f"[graphics]hd_text_style={style}", "[graphics]hd_pack=scripted"],
                                fast=False)
        notes = []
        details = []
        for screen in HD_TEXT_SCREENS:
            legacy = load_png(legacy_out / f"{screen}.png")
            fb = load_png(out / f"{screen}.png")
            if not np.array_equal(load_png(out / f"{screen}_legacy.png"), legacy):
                raise TestFailure(f"{screen}: HD text changed the 8-bit frame")
            for region in HD_TEXT_REGIONS[screen]:
                detail, count = text_detail(fb, legacy, region)
                if count < 50 or detail < HD_TEXT_MIN_DETAIL[style]:
                    raise TestFailure(f"{screen}: text in {region} not drawn at 2x "
                                      f"({detail:.0%} of {count} text pixels have 2x detail)")
                details.append(f"{detail:.0%}")
            # Off text, the frame is what the art phases produce: the legacy
            # frame box-downsampled stays close.
            small = fb.reshape(480, 2, 640, 2, 3).mean(axis=(1, 3))
            colour = float(np.abs(small - legacy).mean())
            if colour > HD_FLOOR_MAX_COLOUR_DIFF:
                raise TestFailure(f"{screen}: frame differs from the legacy frame by {colour:.1f} levels")
        notes.append("8-bit frames unchanged, text detail " + "/".join(details))

        # The fade reaches the 2x text: the label pixels dim like the rest.
        menu = load_png(out / "menu.png")
        fade = load_png(out / "fade_out_50.png")
        left, top, right, bottom = HD_TEXT_REGIONS["menu"][0]
        mask = text_pixels(load_png(legacy_out / "menu.png"), (left, top, right, bottom))
        mask2 = mask.repeat(2, axis=0).repeat(2, axis=1)
        full = menu[top * 2:bottom * 2, left * 2:right * 2][mask2].mean()
        faded = fade[top * 2:bottom * 2, left * 2:right * 2][mask2].mean()
        if not 0.3 < faded / full < 0.7:
            raise TestFailure(f"text in the fade-out frame is {faded / full:.2f} of full brightness, expected ~0.5")
        notes.append(f"text fade {faded / full:.2f}")

        stats = json.loads((out / "stats.json").read_text())
        if not stats["active"] or stats["style"] != style or stats["draws"] < 50 or stats["replays"] < 1:
            raise TestFailure(f"HD text not active as expected: {stats}")
        notes.append(f"{stats['draws']} strings + {stats['replays']} button labels in {stats['total_ms']:.0f} ms")

        goldens = [compare_hd_golden(out / f"{screen}.png", f"hd_text_{style}_{screen}.png", update,
                                     MAIN_MENU_MASK if screen == "menu" else ())
                   for screen in HD_TEXT_SCREENS]
        notes.append("goldens " + "/".join(goldens))
        return ", ".join(notes) + f" ({seconds:.0f}s)"
    test.__name__ = f"test_hd_text_screens_{style}"
    return test


# Logical 640x480 rectangles: the message monitor of the interface bar.
HD_TEXT_MONITOR = (20, 384, 200, 470)


def test_hd_text_game(update):
    """A game view with 2x text: interface bar messages and floating text
    over the player (white with a black outline) are drawn at 2x; the
    message monitor's 8-bit pixels are exactly those of an hd=0 run."""
    if not map_pack_installed("scripted"):
        raise TestSkipped("pack 'scripted' has no tiles (tools/hd: extract.py --areas, enhance_*.py, install.py)")
    reset_saves()
    legacy_out, _ = run_game("hd_text_game_legacy", "hd_text_game.txt", ["--dev-load-game=1"])
    reset_saves()
    out, seconds = run_game("hd_text_game", "hd_text_game.txt",
                            ["--dev-load-game=1", *HD_TEXT_ARGS, "[graphics]hd_text_style=smooth", "[graphics]hd_pack=scripted"])
    fb = load_png(out / "game.png")
    legacy = load_png(out / "game_legacy.png")
    if fb.shape != (960, 1280, 3):
        raise TestFailure(f"frame is {fb.shape}")

    left, top, right, bottom = HD_TEXT_MONITOR
    if not np.array_equal(legacy[top:bottom, left:right], load_png(legacy_out / "game_legacy.png")[top:bottom, left:right]):
        raise TestFailure("HD text changed the message monitor's 8-bit pixels")
    detail, count = text_detail(fb, legacy, HD_TEXT_MONITOR)
    if count < 200 or detail < HD_TEXT_MIN_DETAIL["smooth"]:
        raise TestFailure(f"message monitor not drawn at 2x ({detail:.0%} of {count} text pixels)")

    # Floating text: white legacy pixels in the map area.
    white = np.all(legacy[:380] == (252, 252, 252), axis=2) | np.all(legacy[:380] == (255, 255, 255), axis=2)
    ys, xs = np.nonzero(white)
    if len(ys) < 50:
        raise TestFailure(f"no floating text found ({len(ys)} white pixels)")
    region = (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1)
    blocks = fb[region[1] * 2:region[3] * 2, region[0] * 2:region[2] * 2]
    h, w = region[3] - region[1], region[2] - region[0]
    blocks = blocks.reshape(h, 2, w, 2, 3)
    uniform = np.all(blocks == blocks[:, :1, :, :1, :], axis=(1, 3, 4))
    float_detail = float((~uniform)[white[region[1]:region[3], region[0]:region[2]]].mean())
    if float_detail < HD_TEXT_MIN_DETAIL["smooth"]:
        raise TestFailure(f"floating text not drawn at 2x ({float_detail:.0%} of its pixels)")

    stats = json.loads((out / "stats.json").read_text())
    golden = compare_hd_golden_live(out / "game.png", "hd_text_game_slot01.png", update)
    return (f"monitor {detail:.0%} of {count}, floating text {float_detail:.0%}, "
            f"{stats['draws']} strings + {stats['replays']} replays in {stats['total_ms']:.0f} ms, golden {golden} ({seconds:.0f}s)")


def compare_hd_golden_live(actual_path, golden_name, update):
    golden_path = GOLDEN / golden_name
    if update or not golden_path.exists():
        golden_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(actual_path, golden_path)
        return "new"
    actual = load_png(actual_path)
    golden = load_png(golden_path)
    if actual.shape != golden.shape:
        raise TestFailure(f"{golden_name}: size {actual.shape} != golden {golden.shape}")
    changed = float((np.abs(actual - golden).max(axis=2) > 12).mean())
    if changed > HD_FLOOR_GOLDEN_MAX_DIFF:
        raise TestFailure(f"{golden_name}: {changed:.2%} of pixels differ (limit {HD_FLOOR_GOLDEN_MAX_DIFF:.0%})")
    return f"{changed:.1%}"


# --- Phase 2.10: inventory, loot and trade windows -----------------------------
#
# tests/scripts/hd_inventory.txt loads slot 1 (Sulik in the party), captures
# the interface bar, the inventory, the loot window on Sulik and the trade
# window of a dialogue with him. The 8-bit frames (legacyshot) must equal an
# hd=0 run's frames inside the windows (the map behind them animates).

HD_INVENTORY_PACKS = ("scripted", "ai", "mixed")
HD_INVENTORY_SCREENS = ("game", "inventory", "loot", "barter")
# Logical 640x480 rectangles: the window (8-bit frame must not change) and
# art that must show HD detail (backgrounds, items, the bar's item button).
HD_INVENTORY_WINDOW = {
    "game": [(267, 405, 455, 472)],
    "inventory": [(80, 20, 590, 370), (0, 380, 640, 480)],
    "loot": [(70, 10, 580, 370), (0, 380, 640, 480)],
    "barter": [(0, 295, 640, 480)],
}
HD_INVENTORY_ART = {
    "game": [(267, 405, 455, 472)],
    "inventory": [(80, 30, 210, 330), (250, 150, 440, 350), (250, 280, 440, 350)],
    "loot": [(225, 35, 300, 330), (340, 35, 410, 330), (80, 140, 200, 340)],
    "barter": [(100, 310, 220, 470), (460, 310, 540, 470)],
}
HD_INVENTORY_TEXT = {"inventory": [(390, 40, 550, 240)], "loot": [(230, 320, 410, 340)]}
HD_INVENTORY_MAX_CHANGED = 0.01  # the body views animate; a frame apart is ~0.5%

_hd_inventory_legacy_out = None


def inventory_pack_installed(pack):
    packs = ("scripted", "ai") if pack == "mixed" else (pack,)
    return all((RUN_DIR / f"data/hd/{p}/art/{art}").exists() for p in packs
               for art in ("inven/spear.png", "intrface/invbox.png", "intrface/trade.png", "intrface/sattkbup.png"))


def hd_inventory_legacy_reference():
    global _hd_inventory_legacy_out
    if _hd_inventory_legacy_out is None:
        reset_saves()
        _hd_inventory_legacy_out, _ = run_game("hd_inventory_legacy", "hd_inventory.txt", ["--dev-load-game=1"])
    return _hd_inventory_legacy_out


def make_hd_inventory_test(pack):
    def test(update):
        """Inventory, loot and trade windows and the interface bar's item
        button with a real HD pack: HD backgrounds, buttons and items
        (resampled into their slots), legacy text intact, the 8-bit frames
        exactly as with hd=0, goldens with tolerance."""
        if not inventory_pack_installed(pack):
            raise TestSkipped(f"pack '{pack}' lacks the inventory art (tools/hd: extract.py --areas --objects --inven, "
                              "enhance_*.py, install.py)")
        legacy_out = hd_inventory_legacy_reference()
        reset_saves()
        out, seconds = run_game(f"hd_inventory_{pack}", "hd_inventory.txt",
                                ["--dev-load-game=1", *HD_ARGS, f"[graphics]hd_pack={pack}"])
        notes = []
        for screen in HD_INVENTORY_SCREENS:
            fb = load_png(out / f"{screen}.png")
            eight = load_png(out / f"{screen}_legacy.png")
            legacy = load_png(legacy_out / f"{screen}.png")
            if fb.shape != (960, 1280, 3) or eight.shape != legacy.shape:
                raise TestFailure(f"{screen}: unexpected sizes {fb.shape} / {eight.shape} / {legacy.shape}")
            for left, top, right, bottom in HD_INVENTORY_WINDOW[screen]:
                changed = float(np.any(eight[top:bottom, left:right] != legacy[top:bottom, left:right], axis=2).mean())
                if changed > HD_INVENTORY_MAX_CHANGED:
                    raise TestFailure(f"{screen}: 8-bit frame differs from hd=0 in {(left, top, right, bottom)} "
                                      f"({changed:.2%} of pixels)")
            details = []
            for region in HD_INVENTORY_ART[screen]:
                detail = blocks_detail(fb, region)
                if detail < 0.3:
                    raise TestFailure(f"{screen}: no HD detail in {region} ({detail:.0%} of 2x2 blocks)")
                details.append(f"{detail:.0%}")
            for region in HD_INVENTORY_TEXT.get(screen, ()):
                kept, count = legacy_text_kept(fb, eight, region)
                if count < 50 or kept < 0.98:
                    raise TestFailure(f"{screen}: legacy text in {region}: {kept:.1%} of {count} pixels kept")
            golden = compare_hd_golden_live(out / f"{screen}.png", f"hd_inventory_{pack}_{screen}.png", update)
            notes.append(f"{screen} HD {'/'.join(details)} golden {golden}")
        return ", ".join(notes) + f" ({seconds:.0f}s)"
    test.__name__ = f"test_hd_inventory_{pack}"
    return test


# --- Phase 2.14: the other static screens -------------------------------------
#
# tests/scripts/hd_static_game.txt (slot 1) opens the Pip-Boy, automap,
# skilldex, options, preferences, help and a dialogue; hd_static_menu.txt the
# character selector; hd_static_death.txt the death scene (its own palette). With hd=1 and a pack the art must show HD detail and the
# 8-bit composition (legacyshot) must equal an hd=0 run's frame inside the
# screen's window (the map behind some of them animates).

HD_STATIC_PACKS = ("ai", "mixed")
# Logical 640x480 rectangles: the screen's window, and art that must show HD
# detail (not the map behind it, which is HD anyway).
HD_STATIC_WINDOW = {
    "pipboy": [(0, 0, 640, 480)],
    "automap": [(60, 0, 580, 480)],
    "skilldex": [(455, 5, 635, 370)],
    "options": [(240, 50, 400, 310)],
    "preferences": [(0, 0, 640, 480)],
    "help": [(0, 0, 640, 480)],
    "dialog": [(0, 240, 640, 480)],
    "selector": [(0, 0, 640, 480)],
    "death": [(0, 0, 640, 480)],
}
HD_STATIC_ART = {
    "pipboy": [(0, 40, 200, 330), (480, 0, 640, 60)],
    "automap": [(420, 100, 560, 300)],
    "skilldex": [(460, 20, 620, 320)],
    "options": [(245, 60, 395, 290)],
    "preferences": [(0, 0, 640, 480)],
    "help": [(320, 0, 640, 240), (0, 240, 640, 480)],
    "dialog": [(0, 240, 640, 300), (0, 0, 80, 240)],
    "selector": [(0, 0, 640, 480)],
    "death": [(0, 0, 640, 480)],
}
HD_STATIC_TEXT = {"dialog": [(120, 330, 520, 470)]}
HD_STATIC_MIN_DETAIL = 0.3

_hd_static_legacy_out = None


def static_pack_installed(pack):
    packs = ("scripted", "ai") if pack == "mixed" else (pack,)
    return all((RUN_DIR / f"data/hd/{p}/art/{art}").exists() for p in packs
               for art in ("intrface/pip.png", "intrface/prefscrn.png", "intrface/wrldmp00.png", "skilldex/strength.png"))


def hd_static_legacy_reference():
    global _hd_static_legacy_out
    if _hd_static_legacy_out is None:
        reset_saves()
        game, _ = run_game("hd_static_legacy", "hd_static_game.txt", ["--dev-load-game=1"])
        menu, _ = run_game("hd_static_menu_legacy", "hd_static_menu.txt")
        death, _ = run_game("hd_static_death_legacy", "hd_static_death.txt", ["--dev-load-game=1"])
        _hd_static_legacy_out = {"game": game, "menu": menu, "death": death}
    return _hd_static_legacy_out


def make_hd_static_test(pack):
    def test(update):
        """Pip-Boy, automap, skilldex, options, preferences, help, dialogue
        and character selector with a real HD pack: HD art, legacy text
        intact, the 8-bit frames exactly as with hd=0, goldens with tolerance."""
        if not static_pack_installed(pack):
            raise TestSkipped(f"pack '{pack}' lacks the Phase 2.14 art (tools/hd: extract.py --areas --objects "
                              "--inven --interface, enhance_*.py, install.py)")
        legacy_outs = hd_static_legacy_reference()
        reset_saves()
        args = [*HD_ARGS, f"[graphics]hd_pack={pack}"]
        outs = {}
        seconds = 0
        for run, script, extra in (("game", "hd_static_game.txt", ["--dev-load-game=1"]),
                                   ("menu", "hd_static_menu.txt", []), ("death", "hd_static_death.txt", ["--dev-load-game=1"])):
            outs[run], took = run_game(f"hd_static_{run}_{pack}", script, [*extra, *args])
            seconds += took
        notes = []
        for screen in HD_STATIC_WINDOW:
            run = {"selector": "menu", "death": "death"}.get(screen, "game")
            out, legacy_out = outs[run], legacy_outs[run]
            fb = load_png(out / f"{screen}.png")
            eight = load_png(out / f"{screen}_legacy.png")
            legacy = load_png(legacy_out / f"{screen}.png")
            if fb.shape != (960, 1280, 3) or eight.shape != legacy.shape:
                raise TestFailure(f"{screen}: unexpected sizes {fb.shape} / {eight.shape} / {legacy.shape}")
            for left, top, right, bottom in HD_STATIC_WINDOW[screen]:
                changed = float(np.any(eight[top:bottom, left:right] != legacy[top:bottom, left:right], axis=2).mean())
                if changed > HD_INVENTORY_MAX_CHANGED:
                    raise TestFailure(f"{screen}: 8-bit frame differs from hd=0 in {(left, top, right, bottom)} "
                                      f"({changed:.2%} of pixels)")
            details = []
            for region in HD_STATIC_ART[screen]:
                detail = blocks_detail(fb, region)
                if detail < HD_STATIC_MIN_DETAIL:
                    raise TestFailure(f"{screen}: no HD detail in {region} ({detail:.0%} of 2x2 blocks)")
                details.append(f"{detail:.0%}")
            for region in HD_STATIC_TEXT.get(screen, ()):
                kept, count = legacy_text_kept(fb, eight, region)
                if count < 50 or kept < 0.98:
                    raise TestFailure(f"{screen}: legacy text in {region}: {kept:.1%} of {count} pixels kept")
            golden = compare_hd_golden_live(out / f"{screen}.png", f"hd_static_{pack}_{screen}.png", update)
            notes.append(f"{screen} {'/'.join(details)} golden {golden}")
        return ", ".join(notes) + f" ({seconds:.0f}s)"
    test.__name__ = f"test_hd_static_{pack}"
    return test


# --- Phase 2.16: the interface bar ---------------------------------------------
#
# tests/scripts/hd_bar.txt (slot 1) captures the bar, then combat mode (end
# buttons out, AP bar lit). At 640x480 logical the bar is iface.frm; at
# 3440x1440 (scale 2, 1720x720 logical) it is HR_IFACE_800 with the side
# panels. The bar's 8-bit composition must equal an hd=0 run's.

HD_BAR_PACKS = ("ai", "mixed")
HD_BAR_WIDE = ["[screen]resolution_x=3440", "[screen]resolution_y=1440", "[screen]scale=2"]
# (screen, wide): logical rectangles of the bar window and of art with HD detail.
HD_BAR_WINDOW = {False: (0, 380, 640, 480), True: (0, 620, 1720, 720)}
HD_BAR_ART = {
    False: [(0, 380, 640, 480), (580, 418, 637, 476)],  # bar, end buttons
    True: [(460, 620, 1260, 720), (0, 620, 400, 720), (1320, 620, 1720, 720)],  # bar, side panels
}
HD_BAR_TEXT = [(20, 384, 200, 470)]  # message monitor (legacy text with hd_text=0)

_hd_bar_legacy_out = None


def bar_pack_installed(pack):
    packs = ("scripted", "ai") if pack == "mixed" else (pack,)
    return all((RUN_DIR / f"data/hd/{p}/art/intrface/{art}").exists() for p in packs
               for art in ("iface.png", "hr_iface_800.png", "hr_ifacelft2.png", "endanim.fhd"))


def hd_bar_legacy_reference():
    global _hd_bar_legacy_out
    if _hd_bar_legacy_out is None:
        reset_saves()
        normal, _ = run_game("hd_bar_legacy", "hd_bar.txt", ["--dev-load-game=1"])
        reset_saves()
        wide, _ = run_game("hd_bar_wide_legacy", "hd_bar.txt", ["--dev-load-game=1", *HD_BAR_WIDE])
        _hd_bar_legacy_out = {False: normal, True: wide}
    return _hd_bar_legacy_out


def make_hd_bar_test(pack):
    def test(update):
        """Interface bar (iface, HR_IFACE_800 with side panels), its
        buttons, AP bar and end buttons in combat with a real HD pack: HD
        detail, legacy monitor text intact, the bar's 8-bit frames exactly as
        with hd=0, goldens with tolerance."""
        if not bar_pack_installed(pack):
            raise TestSkipped(f"pack '{pack}' lacks the bar art (tools/hd: extract.py ... --interface, enhance_*.py, install.py)")
        legacy_outs = hd_bar_legacy_reference()
        notes = []
        seconds = 0
        for wide in (False, True):
            reset_saves()
            out, took = run_game(f"hd_bar_{'wide_' if wide else ''}{pack}", "hd_bar.txt",
                                 ["--dev-load-game=1", *HD_ARGS, f"[graphics]hd_pack={pack}", *(HD_BAR_WIDE if wide else [])])
            seconds += took
            for screen in ("game", "combat"):
                fb = load_png(out / f"{screen}.png")
                eight = load_png(out / f"{screen}_legacy.png")
                legacy = load_png(legacy_outs[wide] / f"{screen}.png")
                if eight.shape != legacy.shape or fb.shape[:2] != (eight.shape[0] * 2, eight.shape[1] * 2):
                    raise TestFailure(f"{screen}: unexpected sizes {fb.shape} / {eight.shape} / {legacy.shape}")
                left, top, right, bottom = HD_BAR_WINDOW[wide]
                changed = float(np.any(eight[top:bottom, left:right] != legacy[top:bottom, left:right], axis=2).mean())
                if changed > HD_INVENTORY_MAX_CHANGED:
                    raise TestFailure(f"{screen}{' wide' if wide else ''}: 8-bit bar differs from hd=0 ({changed:.2%} of pixels)")
                regions = HD_BAR_ART[wide] if screen == "combat" or wide else HD_BAR_ART[wide][:1]
                details = []
                for region in regions:
                    detail = blocks_detail(fb, region)
                    if detail < 0.5:
                        raise TestFailure(f"{screen}{' wide' if wide else ''}: no HD detail in {region} ({detail:.0%})")
                    details.append(f"{detail:.0%}")
                if not wide:
                    for region in HD_BAR_TEXT:
                        kept, count = legacy_text_kept(fb, eight, region)
                        if count < 50 or kept < 0.98:
                            raise TestFailure(f"{screen}: monitor text in {region}: {kept:.1%} of {count} pixels kept")
                golden = compare_hd_golden_live(out / f"{screen}.png",
                                                f"hd_bar_{pack}_{'wide_' if wide else ''}{screen}.png", update)
                notes.append(f"{'wide ' if wide else ''}{screen} {'/'.join(details)} golden {golden}")
        return ", ".join(notes) + f" ({seconds:.0f}s)"
    test.__name__ = f"test_hd_bar_{pack}"
    return test


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
    header = (run_dir() / f"data/SAVEGAME/SLOT{slot:02d}/SAVE.DAT").read_bytes()[:0x5B]
    return header[0x3D:0x5B].split(b"\0")[0].decode("latin-1")


def test_stimpak_save_unchanged(update):
    """Stimpak: open slot 3, Save As without edits; the new slot must equal slot 3."""
    reset_saves()
    out, seconds = run_game("stimpak_save_as", "stimpak_save_as.txt")
    notes = [compare_image(out / "hub.png", "stimpak_hub.png", update, LIVE_VIEW_MAX_DIFF)]
    if slot_description(11) != "stimpak test":
        raise TestFailure(f"slot 11 description is {slot_description(11)!r}")
    out11, _ = run_game("stimpak_load11", "load_slot.txt", ["--dev-load-game=11"])
    # Same golden as slot 3: the re-save must not change any state.
    notes.append(compare_state(out11 / "state.json", "slot03_state.json", False))
    # The interface bar must be back: the hub hides it while editing.
    bar = load_png(out11 / "game.png")[-99:]
    golden_bar = load_png(GOLDEN / "slot03_game.png")[-99:]
    bar_diff = float(np.any(bar != golden_bar, axis=2).mean())
    if bar_diff > 0.2:
        raise TestFailure(f"interface bar missing after a Stimpak save ({bar_diff:.0%} of bar pixels differ)")
    return ", ".join(notes) + f" ({seconds:.0f}s)"


def test_character_screens(update):
    """The normal level-up and creation screens must look as before Stimpak."""
    reset_saves()
    out, _ = run_game("char_levelup", "char_screens.txt", ["--dev-load-game=1"])
    notes = [compare_image(out / "levelup.png", "character_levelup.png", update)]
    out, _ = run_game("char_creation", "creation_screen.txt")
    notes.append(compare_image(out / "creation.png", "character_creation.png", update))
    return ", ".join(notes)


def test_stimpak_edit_player(update):
    """Stimpak: ST 7->10, Small Guns +10 clicks, one perk, level 12 on slot 1, saved as slot 11."""
    reset_saves()
    out, seconds = run_game("stimpak_edit_player", "stimpak_edit_player.txt")
    notes = [compare_image(out / "editor_final.png", "stimpak_editor_final.png", update, 0.01)]
    if slot_description(11) != "stimpak edit":
        raise TestFailure(f"slot 11 description is {slot_description(11)!r}")
    out11, _ = run_game("stimpak_edit_load11", "load_slot.txt", ["--dev-load-game=11"])
    notes.append(compare_state(out11 / "state.json", "stimpak_edit_player_state.json", update))
    caps = json.loads((out11 / "state.json").read_text())["player"]["caps"]
    if caps != 5000:
        raise TestFailure(f"saved caps {caps}, expected 5000")

    # Everything except the edited player values must still match slot 1.
    original = json.loads((GOLDEN / "slot01_state.json").read_text())
    edited = stable_state(json.loads((out11 / "state.json").read_text()))
    for key in ("party", "world", "gvars"):
        if original[key] != edited[key]:
            raise TestFailure(f"Stimpak changed {key}")
    for key in ("name", "tagged_skills", "traits"):
        if original["player"][key] != edited["player"][key]:
            raise TestFailure(f"Stimpak changed player {key}")
    return ", ".join(notes) + f" ({seconds:.0f}s)"


def test_stimpak_cancel(update):
    """Stimpak: Cancel in the player editor reverts stats and perks."""
    reset_saves()
    run_game("stimpak_cancel", "stimpak_cancel.txt")
    out11, _ = run_game("stimpak_cancel_load11", "load_slot.txt", ["--dev-load-game=11"])
    return compare_state(out11 / "state.json", "slot01_state.json", False)



# HD mode (no HD art yet) on a sample of saves: must match the doubled legacy frame.
HD_SLOTS = (1, 5, 9)


def test_menu_fades(update):
    """The presenter's framebuffer matches the window composition, and fades run
    at 8-bit precision (within the 6-bit quantisation of the old presenter)."""
    out, seconds = run_game("menu_fades", "menu_fades.txt", fast=False)
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


def test_stimpak_edit_companion(update):
    """Stimpak: level Sulik (slot 1) up once, then edit ST, Melee, HP; saved as slot 11."""
    reset_saves()
    out, seconds = run_game("stimpak_edit_companion", "stimpak_edit_companion.txt")
    notes = [compare_image(out / "companion_final.png", "stimpak_companion_final.png", update, 0.01)]
    out11, _ = run_game("stimpak_companion_load11", "load_slot.txt", ["--dev-load-game=11"])
    state = json.loads((out11 / "state.json").read_text())
    sulik = state["party"][0]
    if (sulik["name"], sulik["level"], sulik["hp"], sulik["base_stats"][0], sulik["stats"][7]) != ("Sulik", 1, 70, 8, 90):
        raise TestFailure(f"companion not saved as edited: {sulik['name']} level {sulik['level']} hp {sulik['hp']} "
                          f"ST {sulik['base_stats'][0]} max hp {sulik['stats'][7]}")
    notes.append(compare_state(out11 / "state.json", "stimpak_edit_companion_state.json", update))
    original = json.loads((GOLDEN / "slot01_state.json").read_text())
    edited = stable_state(state)
    for key in ("player", "world", "gvars"):
        if original[key] != edited[key]:
            raise TestFailure(f"companion edit changed {key}")
    return ", ".join(notes) + f" ({seconds:.0f}s)"


def test_stimpak_lists(update):
    """Stimpak on slot 1: Goris joins, Sulik leaves, a quest becomes ACTIVE,
    the Den becomes VISITED and the party travels there; saved as slot 11."""
    reset_saves()
    out, seconds = run_game("stimpak_lists", "stimpak_lists.txt")
    notes = [compare_image(out / "location_den.png", "stimpak_location.png", update, LIVE_VIEW_MAX_DIFF)]
    out11, _ = run_game("stimpak_lists_load11", "load_slot.txt", ["--dev-load-game=11"])
    state = json.loads((out11 / "state.json").read_text())
    party = [member["name"] for member in state["party"]]
    if party != ["Goris"]:
        raise TestFailure(f"party is {party}, expected only Goris")
    if (state["world"]["map_name"], state["world"]["area"]) != ("DENBUS1.SAV", 1):
        raise TestFailure(f"saved in {state['world']}, expected the Den")
    # Entering the Den runs its map scripts, which set their own variables too;
    # the whole result is deterministic, so it is compared with a golden.
    notes.append(compare_state(out11 / "state.json", "stimpak_lists_state.json", update))
    original = json.loads((GOLDEN / "slot01_state.json").read_text())
    if stable_state(state)["player"] != original["player"]:
        raise TestFailure("the player changed")
    return ", ".join(notes) + f" ({seconds:.0f}s)"


def test_hd_mixed_pack(update):
    """The shipped `mixed` manifest (ce.dat hd/mixed/manifest.txt) picks the
    scripted arrows and AI art for everything else."""
    for pack in ("ai", "scripted"):
        if not (run_dir() / f"data/hd/{pack}").exists():
            return f"SKIP (pack {pack} not installed; run tools/hd/install.py)"
    out, seconds = run_game("hd_mixed_probe", "hd_mixed_probe.txt", [*HD_ARGS, "[graphics]hd_pack=mixed"])
    def source(name, art, folder="intrface"):
        if json.loads((out / f"{name}.json").read_text())["from_legacy"]:
            return "legacy"
        probed = np.asarray(Image.open(out / f"{name}.png").convert("RGBA"))
        for pack in ("ai", "scripted"):
            path = run_dir() / f"data/hd/{pack}/art/{folder}/{art}.png"
            if path.exists() and np.array_equal(probed, np.asarray(Image.open(path).convert("RGBA"))):
                return pack
        return "unknown"
    arrow, menu, roof = source("arrow", "uparwoff"), source("menu", "mainmenu"), source("roof", "ruf1000", "tiles")
    wall = source("wall", "jds1000", "walls")
    if (arrow, menu, roof, wall) != ("scripted", "ai", "scripted", "scripted"):
        raise TestFailure(f"mixed pack resolved the arrow from {arrow}, the menu from {menu}, "
                          f"the metal roof from {roof}, the scrap wall from {wall}")
    # Phase 2.14: digit strips from scripted, world map terrain from ai (when
    # the packs have them).
    if (run_dir() / "data/hd/ai/art/intrface/wrldmp00.png").exists():
        numbers, terrain = source("numbers", "numbers"), source("terrain", "wrldmp00")
        if (numbers, terrain) != ("scripted", "ai"):
            raise TestFailure(f"mixed pack resolved the digits from {numbers}, the terrain from {terrain}")
    return f"arrow, digits and metal roof from scripted, menu and terrain from ai ({seconds:.0f}s)"


def test_stimpak_perk_scan(update):
    """Every perk's card in the Stimpak perk editor can be shown (no crash)."""
    reset_saves()
    out, seconds = run_game("stimpak_perk_scan", "stimpak_perk_scan.txt")
    return f"all perk cards shown ({seconds:.0f}s)"


def test_english_interface_bar(update):
    """At 800+ logical pixels the 800-wide interface bar comes from ce.dat
    (English labels), not the Russian one in f2_res.dat."""
    reset_saves()
    out, seconds = run_game("iface800", "load_slot.txt", ["--dev-load-game=1", "[screen]resolution_x=1600",
                                                          "[screen]resolution_y=960", "[ui]iface_bar_width=800"])
    game = load_png(out / "game.png")
    bar = game[-99:, (game.shape[1] - 800) // 2:(game.shape[1] + 800) // 2]
    # Live content (messages, item, numbers) covers much of the bar, so compare
    # how well it matches the English art against the Russian one.
    def match(name):
        art = np.asarray(Image.open(GOLDEN / name).convert("RGB"), dtype=np.int16)
        return float(np.all(np.abs(bar - art) <= 8, axis=2).mean())
    english, russian = match("hr_iface_800_en.png"), match("hr_iface_800_ru.png")
    if english < 0.25 or english < 2 * russian:
        raise TestFailure(f"bar matches English art {english:.0%}, Russian art {russian:.0%}")
    return f"English {english:.0%} vs Russian {russian:.0%} ({seconds:.0f}s)"


def test_load_save_with_hidden_bar(update):
    """Slot 12 was saved by an early Stimpak build with the interface bar
    flagged hidden; loading it must still show the bar."""
    reset_saves()
    out, seconds = run_game("hidden_bar_save", "load_slot.txt", ["--dev-load-game=12"])
    bar = load_png(out / "game.png")[-99:]
    if bar.mean() < 10:
        raise TestFailure("interface bar not shown after loading slot 12")
    return f"bar shown ({seconds:.0f}s)"


# --- Phase 2.11: HD lights ----------------------------------------------------
#
# tests/scripts/hd_lights.txt loads the Den at night (slot 8) with
# hd_lights=1 and captures the view with lights off, coloured light only,
# + glow and + shadows, the light field (`hdlights field`), the emitters
# (`hdlights dump`) and eight frames for the fire flicker.

HD_LIGHTS_SLOT = 8
HD_LIGHTS_TIME_GVARS = {104}
HD_LIGHTS_ARGS = [*HD_ARGS, "[graphics]hd_pack=mixed", "[graphics]hd_lights=1"]
# Where the field is neutral, frames with and without lights may differ only
# where the scene animates (critters, colour cycling): share of pixels off by
# more than a few levels.
HD_LIGHTS_MAX_FAR_CHANGE = 0.03
# Near a fire (within 2 hexes): the lit frame is brighter and warmer.
HD_LIGHTS_MIN_WARMTH = 6.0
# Fire flicker: consecutive frames differ near the fire, but the mean stays
# within a few levels.
HD_LIGHTS_MAX_FLICKER = 12.0


def field_neutral_mask(out, shape):
    """Logical-resolution mask of the pixels the light field leaves alone,
    from the `hdlights field` dumps (light cells 4 px, shadow cells 2 px)."""
    h, w = shape
    neutral = np.ones((h, w), dtype=bool)
    for name, cell, scale in (("mul", 4, 255.0), ("add", 4, 1.0), ("shade", 2, 255.0)):
        grid = np.asarray(Image.open(out / f"field_{name}.png").convert("RGB")).max(axis=2) > 0
        # A pixel reads the cells around it: grow by one cell.
        grown = grid.copy()
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                grown |= np.roll(np.roll(grid, dy, 0), dx, 1)
        neutral &= ~grown.repeat(cell, 0).repeat(cell, 1)[:h, :w]
    return neutral


WM_VIEW = (22, 21, 472, 465)        # logical rectangle of the world map's view
WM_MIN_TERRAIN_DETAIL = 0.5         # explored terrain: 2x2 blocks with HD detail
WM_MAX_TERRAIN_COLOUR_DIFF = 16.0   # HD terrain box-downsampled vs legacy, mean abs


def test_hd_ground_variants(update):
    """Ground variants (2.14): in Arroyo the desert ground shows per-square
    variants with hd_ground_variants=1; the 8-bit picture is identical."""
    if not (run_dir() / "data/hd/ai/art/tiles/variants/edg5000").is_dir():
        return "skipped: no ground variants in the pack (tools/hd/ground_variants.py)"
    args = ["--dev-load-game=3", "[graphics]hd=1", "[graphics]hd_pack=mixed"]
    off, _ = run_game("hd_ground_off", "hd_ground.txt", args + ["[graphics]hd_ground_variants=0"])
    on, seconds = run_game("hd_ground_on", "hd_ground.txt", args + ["[graphics]hd_ground_variants=1"])
    if not np.array_equal(load_png(off / "legacy.png"), load_png(on / "legacy.png")):
        raise TestFailure("the 8-bit picture changed with ground variants")
    a, b = load_png(off / "fb.png"), load_png(on / "fb.png")
    changed = (np.abs(a - b).sum(axis=2) > 24).mean()
    if changed < 0.05:
        raise TestFailure(f"only {changed:.1%} of the frame changed: variants not drawn")
    return f"{changed:.0%} of the frame shows variants, legacy identical ({seconds:.0f}s)"


# --- Phase 2.17: floors and roofs of every town --------------------------------
#
# The saves are in Arroyo, Klamath and the Den; Stimpak's LOCATION screen
# moves the party of slot 1 to another town (TRAVEL), then PLAY runs the game
# there without saving. Every floor and roof tile is in the packs now: floor
# pixels show pack art (2x2 output blocks with detail), not the legacy tile
# doubled (flat blocks). Variants (hd_ground_variants) change the HD frame on
# the town's ground and leave the 8-bit picture alone.

HD_TOWN_MIN_FLOOR_DETAIL = 0.9    # HD floor/roof pixels whose 2x2 block is not flat
HD_TOWN_MIN_VARIANT_CHANGE = 0.05  # of the frame, with variants on vs off
# (town, letter keys in the LOCATION list, a tile whose variants the town shows)
HD_TOWNS = [
    ("newreno", "n", "road03"),
    ("ncr", "nn", "curb01"),
    ("vaultcity", "v", None),
]

HD_TOWN_SCRIPT = """until mainmenu 20000
wait 300
key s
wait 1200
key home
key enter
until stimpak 30000
wait 500
key l
wait 800
{keys}
key +
key +
wait 300
click 460 316
wait 800
key enter
until stimpak 30000
wait 1500
key p
until game 30000
wait 2500
dump state
fbshot game
legacyshot legacy
hdcheck check
quit 0
"""


def floor_detail(fb, out, name):
    """Fraction of the 2x2 output blocks of HD floor/roof pixels (grey in the
    hdcheck mask) that are not one flat colour."""
    mask = np.all(np.asarray(Image.open(out / f"{name}.png").convert("RGB")) == (128, 128, 128), axis=2)
    h, w = mask.shape
    blocks = fb[:h * 2, :w * 2].reshape(h, 2, w, 2, 3)
    detail = np.abs(blocks - blocks[:, :1, :, :1, :]).sum(axis=(1, 3, 4)) > 0
    return float(detail[mask].mean()) if mask.any() else 0.0


def make_hd_town_test(town, keys, variant_tile):
    def test(update):
        """A town outside the save games' areas: floors and roofs from the
        pack (not doubled legacy pixels); its ground variants show in HD and
        the 8-bit picture does not change."""
        if not (RUN_DIR / "data/hd/ai/art/tiles/road03.png").exists():
            raise TestSkipped("the packs lack the other towns' tiles (tools/hd: extract.py --all-tiles, "
                              "enhance_*.py, install.py)")
        script = run_dir() / "devout/tests" / f"hd_town_{town}.txt"
        script.parent.mkdir(parents=True, exist_ok=True)
        script.write_text(HD_TOWN_SCRIPT.format(keys="\n".join(f"key {k}" for k in keys)))
        args = [*HD_ARGS, "[graphics]hd_pack=mixed"]
        reset_saves()
        off, seconds = run_game(f"hd_town_{town}_off", script, args + ["[graphics]hd_ground_variants=0"])
        state = json.loads((off / "state.json").read_text())["world"]
        if state["map_name"].lower().startswith(("ar", "kla", "den")):
            raise TestFailure(f"{town}: the party did not travel ({state['map_name']})")
        check, share, _ = hd_check(off, "check")
        detail = floor_detail(load_png(off / "game.png"), off, "check")
        if detail < HD_TOWN_MIN_FLOOR_DETAIL:
            raise TestFailure(f"{town} ({state['map_name']}): only {detail:.0%} of HD floor blocks show "
                              "detail: doubled legacy tiles, not pack art")
        result = f"{state['map_name']}: floors/roofs {share:.0%} HD, {detail:.0%} with detail"
        if variant_tile and (RUN_DIR / f"data/hd/ai/art/tiles/variants/{variant_tile}").is_dir():
            reset_saves()
            on, more = run_game(f"hd_town_{town}_on", script, args + ["[graphics]hd_ground_variants=1"])
            seconds += more
            if not np.array_equal(load_png(off / "legacy.png"), load_png(on / "legacy.png")):
                raise TestFailure(f"{town}: the 8-bit picture changed with ground variants")
            a, b = load_png(off / "game.png"), load_png(on / "game.png")
            changed = float((np.abs(a - b).sum(axis=2) > 24).mean())
            if changed < HD_TOWN_MIN_VARIANT_CHANGE:
                raise TestFailure(f"{town}: only {changed:.1%} of the frame changed: variants not drawn")
            result += f", variants change {changed:.0%} of the frame, legacy identical"
        return f"{result} ({seconds:.0f}s)"
    test.__name__ = f"test_hd_town_{town}"
    return test


def test_worldmap_scroll(update):
    """World map in HD (mixed pack, HD text): after scrolling, no HD pixels or
    HD text survive over unexplored (black) land; labels still show in HD;
    explored and fogged terrain shows HD detail in the legacy colours (fog
    darkened in RGB) at every step; the frame and the town map are HD."""
    if not static_pack_installed("mixed"):
        raise TestSkipped("packs lack the Phase 2.14 art (tools/hd: extract.py ... --interface, enhance_*.py, install.py)")
    out, seconds = run_game("worldmap_scroll", "worldmap_scroll.txt",
                            ["--dev-load-game=1", "[graphics]hd=1", "[graphics]hd_pack=mixed", "[graphics]hd_text=1"])
    notes = []
    left, top, right, bottom = WM_VIEW
    for step in range(4):
        framebuffer = load_png(out / f"s{step}.png")
        legacy1 = load_png(out / f"l{step}.png")
        legacy = legacy1.repeat(2, axis=0).repeat(2, axis=1)
        if framebuffer.shape != legacy.shape:
            raise TestFailure(f"step {step}: size {framebuffer.shape} != {legacy.shape}")
        differs = np.abs(framebuffer - legacy).sum(axis=2) > 60
        # Unexplored land: black with a black neighbourhood (the darkest
        # pixels of HD terrain under the fog are near-black in 8 bits, and
        # black legacy pixels next to live labels get the labels' 2x edges);
        # leftovers of fogged-over text are hundreds of pixels.
        black1 = legacy1.sum(axis=2) < 30
        unexplored = black1.copy()
        for dy in range(-3, 4):
            for dx in range(-3, 4):
                unexplored &= np.roll(np.roll(black1, dy, 0), dx, 1)
        unexplored = unexplored.repeat(2, axis=0).repeat(2, axis=1)
        over_black = int((differs & unexplored).sum())
        if over_black > 120:
            raise TestFailure(f"step {step}: {over_black} HD pixels over black land (leftovers)")
        # Terrain: logical pixels of the view that are not black in 8 bits.
        view = legacy1[top:bottom, left:right]
        hd = framebuffer[top * 2:bottom * 2, left * 2:right * 2]
        blocks = hd.reshape(bottom - top, 2, right - left, 2, 3)
        land = view.sum(axis=2) > 60
        detail = float((np.abs(blocks - blocks[:, :1, :, :1, :]).sum(axis=(1, 3, 4)) > 0)[land].mean())
        colour = float(np.abs(blocks.mean(axis=(1, 3)) - view)[land].mean())
        if land.mean() > 0.1 and (detail < WM_MIN_TERRAIN_DETAIL or colour > WM_MAX_TERRAIN_COLOUR_DIFF):
            raise TestFailure(f"step {step}: terrain detail {detail:.0%}, colour difference {colour:.1f}")
        notes.append(f"{over_black}/{detail:.0%}/{colour:.1f}")
    frame = blocks_detail(load_png(out / "s0.png"), (480, 60, 630, 470))
    town = blocks_detail(load_png(out / "town.png"), WM_VIEW)
    if frame < 0.5 or town < 0.5:
        raise TestFailure(f"world map frame {frame:.0%} / town map {town:.0%} of 2x2 blocks with HD detail")
    return (f"per step HD pixels over black land/terrain detail/colour diff: {', '.join(notes)}; "
            f"frame {frame:.0%}, town map {town:.0%} ({seconds:.0f}s)")


def test_hd_lights(update):
    """Den at night with hd_lights=1: warm light and glow near the fire,
    nothing changes where the light field is neutral, shadows only darken,
    the fire flickers within bounds, state unchanged."""
    if not map_pack_installed("mixed"):
        raise TestSkipped("pack 'mixed' lacks the area art")
    reset_saves()
    out, seconds = run_game("hd_lights", "hd_lights.txt", [f"--dev-load-game={HD_LIGHTS_SLOT}", *HD_LIGHTS_ARGS])
    compare_state(out / "state.json", f"slot{HD_LIGHTS_SLOT:02d}_state.json", False)
    # A few seconds of play later the state is the same, except global var
    # 104, which the Den's scripts advance with game time (also with lights off).
    before = stable_state(json.loads((out / "state.json").read_text()))
    after = stable_state(json.loads((out / "state_after.json").read_text()))
    for state in (before, after):
        state["gvars"] = [v for i, v in enumerate(state["gvars"]) if i not in HD_LIGHTS_TIME_GVARS]
    if before != after:
        raise TestFailure("state changed while the lights were on: " + str([k for k in before if before[k] != after[k]]))

    lights = json.loads((out / "lights.json").read_text())
    fires = [l for l in lights["lights"] if l["kind"] == "fire"]
    if not fires:
        raise TestFailure("no fire light found in the Den")
    if lights["darkness"] < 0.9:
        raise TestFailure(f"the Den save should be at night (darkness {lights['darkness']})")

    frames = {name: load_png(out / f"{name}.png") for name in ("off", "colour", "glow", "all")}
    h, w = frames["off"].shape[0] // 2, frames["off"].shape[1] // 2
    small = {name: frame.reshape(h, 2, w, 2, 3).mean(axis=(1, 3)) for name, frame in frames.items()}

    # Unchanged where the field is neutral (animation aside).
    neutral = field_neutral_mask(out, (h, w))
    if neutral.mean() < 0.3:
        raise TestFailure(f"the light field covers {1 - neutral.mean():.0%} of the screen")
    far = np.abs(small["all"] - small["off"]).max(axis=2)[neutral]
    far_change = float((far > 6).mean())
    if far_change > HD_LIGHTS_MAX_FAR_CHANGE:
        raise TestFailure(f"{far_change:.1%} of the pixels outside the light field changed")

    # Brighter and warmer next to a fire on screen.
    warmth = []
    for fire in fires:
        x, y = int(fire["x"]), int(fire["y"])
        if not (40 <= x < w - 40 and 40 <= y < h - 40):
            continue
        box = (slice(y - 30, y + 30), slice(x - 50, x + 50))
        off, lit = small["off"][box], small["colour"][box]
        gain = float((lit - off).mean())
        warm = float(((lit[..., 0] - lit[..., 2]) - (off[..., 0] - off[..., 2])).mean())
        if gain <= 0:
            raise TestFailure(f"fire at ({x}, {y}): not brighter with coloured light ({gain:.1f})")
        warmth.append(warm)
    if not warmth:
        raise TestFailure("no fire within the test view")
    if max(warmth) < HD_LIGHTS_MIN_WARMTH:
        raise TestFailure(f"coloured light near the fire is not warm enough ({max(warmth):.1f})")

    # Glow adds light, shadows only remove it.
    if float((small["glow"] - small["colour"]).mean()) < -0.5:
        raise TestFailure("glow darkened the frame")
    if float((small["all"] - small["glow"]).mean()) > 0.5:
        raise TestFailure("shadows brightened the frame")

    # Fire flicker: frames differ near the fire, mean bounded.
    fire = fires[0]
    x, y = int(fire["x"]), int(fire["y"])
    box = (slice(max(y - 40, 0) * 2, (y + 40) * 2), slice(max(x - 60, 0) * 2, (x + 60) * 2))
    anim = [load_png(out / f"anim{i:02d}.png")[box] for i in range(1, 9)]
    changes = [float(np.abs(a - b).mean()) for a, b in zip(anim, anim[1:])]
    means = [float(a.mean()) for a in anim]
    if max(changes) < 0.2:
        raise TestFailure("no flicker: the fire's surroundings do not change between frames")
    if max(means) - min(means) > HD_LIGHTS_MAX_FLICKER:
        raise TestFailure(f"flicker too strong: mean varies by {max(means) - min(means):.1f} levels")

    return (f"{len(lights['lights'])} lights ({len(fires)} fire), field covers {1 - neutral.mean():.0%}, "
            f"outside changed {far_change:.1%}, warmth +{max(warmth):.1f}, flicker {max(means) - min(means):.1f} "
            f"({seconds:.0f}s)")


# Phase 2.13: map view (smooth panning, zoom) and lights that follow walking
# critters, at the user's 3440x1440 (logical 1720x720). The map view is off
# by default (and in the other tests), so their goldens are unaffected.
VIEW_SCREEN = ["[screen]resolution_x=3440", "[screen]resolution_y=1440", "[screen]scale=2"]
VIEW_HD = [*VIEW_SCREEN, *HD_ARGS, "[graphics]hd_pack=mixed"]
VIEW_ON = ["[graphics]smooth_scroll=1", "[graphics]zoom=1"]
VIEW_SLOT = 8


def compare_state_after_play(out, slot):
    """The state dumped after some play equals the slot's golden, except
    global var 104, which the Den's scripts advance with game time."""
    golden = json.loads((GOLDEN / f"slot{slot:02d}_state.json").read_text())
    actual = stable_state(json.loads((out / "state.json").read_text()))
    for state in (golden, actual):
        state["gvars"] = [v for i, v in enumerate(state["gvars"]) if i not in HD_LIGHTS_TIME_GVARS]
    if json.loads(json.dumps(actual)) != golden:
        raise TestFailure("state changed: " + str([k for k in golden if golden[k] != actual.get(k)]))


def view_frames(out, name):
    return json.loads((out / f"{name}.json").read_text())["frames"]


def test_view_identity(update):
    """Map view on at zoom 1 at rest: the same frame as without it (the map
    is drawn into the surface and copied, HD pixels included)."""
    if not map_pack_installed("mixed"):
        raise TestSkipped("pack 'mixed' lacks the area art")
    frames = {}
    for label, extra in (("off", []), ("on", VIEW_ON)):
        reset_saves()
        out, seconds = run_game(f"view_identity_{label}", "load_slot.txt", ["--dev-load-game=3", *VIEW_HD, *extra])
        frames[label] = load_png(out / "game.png")
    differs = np.any(frames["off"] != frames["on"], axis=2)
    # (Arroyo's people walk about between the two runs.)
    if differs.mean() > 0.01:
        raise TestFailure(f"{differs.mean():.3%} of the pixels differ with the map view on")
    return f"{differs.mean():.4%} pixels differ ({seconds:.0f}s)"


def test_view_pan(update):
    """Held scroll, original vs smooth: the same scroll limits and end
    position; the original moves in whole 32 px steps, smooth panning in
    sub-step amounts every frame and comes to rest on the centre."""
    results = {}
    for label, extra in (("steps", []), ("smooth", ["[graphics]smooth_scroll=1"])):
        reset_saves()
        # At the test resolution (640x480 logical) the debug build presents
        # often enough to see the motion between frames.
        out, seconds = run_game(f"view_pan_{label}", "view_pan.txt", [f"--dev-load-game={VIEW_SLOT}", *HD_ARGS,
                                                                       "[graphics]hd_pack=mixed", *extra])
        xs = [f["dude"]["feet_screen"][0] for f in view_frames(out, "pan")]
        moves = [abs(b - a) for a, b in zip(xs, xs[1:]) if abs(b - a) > 0.01]
        after = json.loads((out / "after.json").read_text())
        before = json.loads((out / "before.json").read_text())
        results[label] = (moves, before, after)
        compare_state_after_play(out, VIEW_SLOT)
    steps, smooth = results["steps"][0], results["smooth"][0]
    if not steps or any(m % 32 != 0 for m in steps):
        raise TestFailure(f"original scrolling should move in 32 px steps: {steps}")
    partial = [m for m in smooth if abs(m - 32 * round(m / 32)) > 0.6]
    if len(smooth) < 10 or len(partial) < len(smooth) * 0.5:
        raise TestFailure(f"smooth panning should move by parts of a step every frame: {smooth}")
    if max(smooth) > 48:
        raise TestFailure(f"smooth panning jumped {max(smooth)} px in a frame")
    if results["steps"][2]["centre_tile"] != results["smooth"][2]["centre_tile"]:
        raise TestFailure("smooth panning ended on another centre than the original scroll: "
                          f"{results['steps'][2]['centre_tile']} vs {results['smooth'][2]['centre_tile']}")
    if results["smooth"][2]["pan_x"] != 0 or results["smooth"][2]["pan_y"] != 0:
        raise TestFailure("the view did not come to rest on the centre")
    return (f"original {len(steps)} steps of 32 px, smooth {len(smooth)} frames of {min(smooth):.1f}..{max(smooth):.1f} px, "
            f"same end centre {results['smooth'][2]['centre_tile']}")


def check_view_zoom(out, hd):
    views = {z: json.loads((out / f"v{z}.json").read_text()) for z in ("100", "200", "050", "keyreset", "reset")}
    for name, zoom in (("100", 1.0), ("200", 2.0), ("050", 0.5), ("keyreset", 1.0), ("reset", 1.0)):
        if abs(views[name]["zoom"] - zoom) > 1e-6:
            raise TestFailure(f"zoom at {name}: {views[name]['zoom']} (expected {zoom}; keys Ctrl+= / Ctrl+- / Ctrl+0)")
    dude = views["100"]["dude"]["tile"]
    maps = []
    for z in ("100", "200", "050"):
        sprite = json.loads((out / f"party{z}.json").read_text())
        hexes = json.loads((out / f"hex{z}.json").read_text())
        if sprite["critter"] is None or sprite["critter"]["tile"] != dude:
            raise TestFailure(f"zoom {z}: clicking the player's sprite picks {sprite['critter']}")
        if hexes["tile"] != dude or hexes["hex_cursor_tile"] != dude:
            raise TestFailure(f"zoom {z}: the hex under the cursor is {hexes['tile']} (cursor {hexes['hex_cursor_tile']}), "
                              f"the player stands on {dude}")
        maps.append(sprite["map"])
    if any(abs(a - b) > 1 for m in maps for a, b in zip(m, maps[0])):
        raise TestFailure(f"the same sprite point maps to different map pixels at different zooms: {maps}")
    frames = {z: load_png(out / f"zoom{z}.png") for z in ("100", "200", "050")}
    height = frames["100"].shape[0]
    bar = height * 100 // 720  # the interface bar (100 logical rows) keeps its size
    for z in ("200", "050"):
        if np.any(frames[z][-bar:] != frames["100"][-bar:], axis=2).mean() > 0.02:
            raise TestFailure(f"zoom {z}: the interface bar changed")
        if np.any(frames[z][:-bar] != frames["100"][:-bar], axis=2).mean() < 0.5:
            raise TestFailure(f"zoom {z}: the map view hardly changed")
    return maps[0]


def test_view_zoom(update):
    """Zoom 1, 2 (Ctrl+= x3) and 0.5 (Ctrl+- x2 after Ctrl+0), animated; the
    player's sprite and hex are hit at every zoom, the interface bar keeps
    its size, state unchanged."""
    if not map_pack_installed("mixed"):
        raise TestSkipped("pack 'mixed' lacks the area art")
    reset_saves()
    out, seconds = run_game("view_zoom", "view_zoom.txt", [f"--dev-load-game={VIEW_SLOT}", *VIEW_HD, *VIEW_ON])
    compare_state_after_play(out, VIEW_SLOT)
    point = check_view_zoom(out, True)
    return f"zoom 1/2/0.5 reached, sprite and hex hit at every zoom (map point {point}) ({seconds:.0f}s)"


def test_view_zoom_hd0(update):
    """The map view with hd=0 (nearest pixel): zoom and hit-testing work,
    state unchanged."""
    reset_saves()
    out, seconds = run_game("view_zoom_hd0", "view_zoom.txt", [f"--dev-load-game={VIEW_SLOT}", *VIEW_SCREEN, *VIEW_ON])
    compare_state_after_play(out, VIEW_SLOT)
    check_view_zoom(out, False)
    return f"hd=0: zoom and hit-testing ok ({seconds:.0f}s)"


def light_track(out):
    """Per frame: the player's feet x and the x centroid of the light on the
    floor around him (his sprite masked out), logical pixels."""
    meta = json.loads((out / "walk.json").read_text())
    left, top, width, height = meta["crop"]
    yy, xx = np.mgrid[0:height, 0:width]
    rows = []
    for index, frame in enumerate(meta["frames"]):
        lum = load_png(out / f"walk_{index:02d}.png").mean(axis=2)
        fx, fy = frame["dude"]["feet_screen"]
        fx, fy = fx * 2 - left, fy * 2 - top
        r = np.sqrt((xx - fx) ** 2 + ((yy - fy) * 2) ** 2)
        mask = (r > 70) & (r < 300) & ~((np.abs(xx - fx) < 60) & (yy < fy + 10) & (yy > fy - 200))
        weight = np.clip(lum - np.percentile(lum[mask], 30), 0, None) * mask
        rows.append((fx / 2, float((weight * xx).sum() / weight.sum()) / 2, frame))
    return rows


def test_view_light_follow(update):
    """Den at night, the player walks east: with hd_lights_follow the light
    on the floor tracks his sprite smoothly (follows it linearly), without
    it it jumps hex by hex; the light reported for him sits at the sprite."""
    if not map_pack_installed("mixed"):
        raise TestSkipped("pack 'mixed' lacks the area art")
    residual = {}
    for label, extra in (("hex", ["[graphics]hd_lights_follow=0"]), ("follow", [])):
        reset_saves()
        out, seconds = run_game(f"view_light_{label}", "view_light_walk.txt",
                                [f"--dev-load-game={HD_LIGHTS_SLOT}", *VIEW_HD, "[graphics]hd_lights=1", *extra])
        compare_state_after_play(out, HD_LIGHTS_SLOT)
        rows = light_track(out)
        x = np.array([r[0] for r in rows])
        c = np.array([r[1] for r in rows])
        moving = (x > x[0] + 1) & (x < x[-1] - 1)
        if moving.sum() < 6:
            raise TestFailure(f"{label}: the player hardly moved ({x[0]:.0f} -> {x[-1]:.0f})")
        slope, offset = np.polyfit(x[moving], c[moving], 1)
        residual[label] = float((c[moving] - (slope * x[moving] + offset)).std())
        if label == "follow":
            lights = [m for r in rows for m in r[2].get("moving_lights", [])]
            if not lights:
                raise TestFailure("no light was placed at a walking sprite")
            for light in lights:
                # The light moves from the hex towards the sprite (other
                # lights and the clamping to full light keep it from moving
                # all the way).
                shift = light["moved_x"] - light["centre_x"]
                if abs(light["dx"]) >= 8 and (shift * light["dx"] <= 0 or abs(shift) > abs(light["dx"]) + 4):
                    raise TestFailure(f"moved light not towards the sprite: {light}")
    if residual["follow"] > 3.0 or residual["follow"] > 0.6 * residual["hex"]:
        raise TestFailure(f"the light does not follow the sprite smoothly: residual {residual['follow']:.2f} px "
                          f"(hex by hex {residual['hex']:.2f} px)")
    return f"light vs sprite residual {residual['follow']:.2f} px (hex by hex {residual['hex']:.2f} px)"


# --- Phase 2.15: gunshot light and wind ----------------------------------------
#
# Both are off in the test run directory (setup-run-dir.sh); these tests turn
# them on. The other tests' goldens are unaffected.

SHOT_SLOT = 8
SHOT_ARGS = [*HD_ARGS, "[graphics]hd_pack=mixed", "[graphics]hd_shot_light=1", "[graphics]hd_lights=0"]


def shot_gain(image, base, shot):
    """Mean brightening of `image` over `base` ahead of the muzzle (towards
    the target) and behind it (output pixels around the muzzle)."""
    mx, my = int(shot["muzzle_x"] * 2), int(shot["muzzle_y"] * 2)
    side = 1 if shot["target_x"] >= shot["muzzle_x"] else -1
    diff = image.astype(float) - base.astype(float)
    rows = slice(my - 30, my + 30)
    ahead = diff[rows, mx + side * 20:mx + side * 160:side].mean()
    behind = diff[rows, mx - side * 160:mx - side * 20:side].mean()
    return float(ahead), float(behind)


def test_hd_shot_light(update):
    """Den at night, the player fires a pistol, then a burst: one light per
    round, starting at the muzzle (found in the shot frame's flash, at the
    weapon on the sprite) and lighting ahead of it, not behind; afterwards
    the light field is empty again."""
    if not map_pack_installed("mixed"):
        raise TestSkipped("pack 'mixed' lacks the area art")
    reset_saves()
    out, seconds = run_game("hd_shot_light", "hd_shot_light.txt", [f"--dev-load-game={SHOT_SLOT}", *SHOT_ARGS])
    single = json.loads((out / "single.json").read_text())["shots"]["fired"]
    burst = json.loads((out / "burst.json").read_text())["shots"]["fired"][len(single):]
    if len(single) != 1:
        raise TestFailure(f"single shot: {len(single)} lights fired")
    if len(burst) < 3:
        raise TestFailure(f"burst: only {len(burst)} light(s) fired")

    state = json.loads((out / "single.json").read_text())
    for shot in single + burst:
        if not shot["from_flash"]:
            raise TestFailure(f"muzzle not found from the flash: {shot}")
    # At the weapon: in front of the shooter at chest height (Den save: the
    # player stands at hex 13292, screen (320, 190) at 1280x960).
    feet_x, feet_y = 320, 190
    for shot in single + burst:
        up = feet_y - shot["muzzle_y"]
        across = (shot["muzzle_x"] - feet_x) * (1 if shot["target_x"] >= feet_x else -1)
        if not (15 <= up <= 70 and -6 <= across <= 45):
            raise TestFailure(f"muzzle not at the weapon: {up:.0f} px above the feet, {across:.0f} px ahead ({shot})")

    before, lit, after = (load_png(out / f"{name}.png") for name in ("before", "lit", "after"))
    ahead, behind = shot_gain(lit, before, single[0])
    if ahead < 8.0:
        raise TestFailure(f"too little light ahead of the muzzle: +{ahead:.1f}")
    if behind > 0.5 * ahead:
        raise TestFailure(f"behind the shooter nearly as bright as ahead: +{behind:.1f} vs +{ahead:.1f}")
    late, _ = shot_gain(after, before, single[0])
    if abs(late) > 3.0:
        raise TestFailure(f"the light did not decay: {late:.1f} levels left ahead of the muzzle")

    # Nothing left in the field once the shots are over.
    for name in ("mul", "add"):
        field = np.asarray(Image.open(out / f"after_{name}.png").convert("RGB"))
        if field.max() > 0:
            raise TestFailure(f"light field not empty after the shots ({name} max {field.max()})")
    return (f"{len(burst)} burst rounds, at the shot ahead +{ahead:.1f} / behind +{behind:.1f} levels, "
            f"after {late:.1f} ({seconds:.0f}s)")


WIND_ARGS = [*HD_ARGS, "[graphics]hd_pack=mixed", "[graphics]hd_wind=1"]
WIND_SLOTS = (3, 9)


def make_hd_wind_test(slot):
    def test(update):
        """Arroyo with hd_wind=1: whole redraws with the wind held at other
        times and off leave the 8-bit buffer unchanged; between two wind
        times the plants' tops move while their bottom 35% stays put."""
        if not map_pack_installed("mixed"):
            raise TestSkipped("pack 'mixed' lacks the area art")
        reset_saves()
        out, seconds = run_game(f"hd_wind_slot{slot:02d}", "hd_wind.txt", [f"--dev-load-game={slot}", *WIND_ARGS])
        legacy = json.loads((out / "legacy.json").read_text())
        if not legacy["wind_was_on"] or any(legacy["differing"]):
            raise TestFailure(f"8-bit buffer changed by wind redraws: {legacy}")
        a, b = load_png(out / "t0.png"), load_png(out / "t1.png")
        h, w = a.shape[0] // 2, a.shape[1] // 2
        p0 = json.loads((out / "t0.json").read_text())["wind"]["plants"]
        p1 = json.loads((out / "t1.json").read_text())["wind"]["plants"]
        if len(p0) < 3 or len(p0) != len(p1):
            raise TestFailure(f"plants on screen: {len(p0)} / {len(p1)}")
        # Pixels that may change for other reasons: where the 8-bit view
        # changed (critters walking) and other plants (grass in front of a
        # tree's base), grown by a pixel.
        l0, l1 = load_png(out / "l0.png"), load_png(out / "l1.png")
        busy = np.abs(l0 - l1).sum(axis=2) > 0
        busy = busy | np.roll(busy, 1, 0) | np.roll(busy, -1, 0) | np.roll(busy, 1, 1) | np.roll(busy, -1, 1)
        busy = busy.repeat(2, 0).repeat(2, 1)
        moved = 0
        for index, (q0, q1) in enumerate(zip(p0, p1)):
            left, top, right, bottom = q0["rect"]
            others = np.zeros(busy.shape, dtype=bool)
            for other in p0[:index] + p0[index + 1:]:
                ol, ot, orr, ob = other["rect"]
                others[max(2 * ot, 0):max(2 * (ob + 1), 0), max(2 * ol, 0):max(2 * (orr + 1), 0)] = True
            # Inside the map view (above the interface bar).
            y0, y1 = max(top, 0), min(bottom + 1, h - 100)
            x0, x1 = max(left, 0), min(right + 1, w)
            if y1 - y0 < 8 or x1 - x0 < 4:
                continue
            region = (slice(2 * y0, 2 * y1), slice(2 * x0, 2 * x1))
            diff = (np.abs(a[region] - b[region]).sum(axis=2) > 0) & ~busy[region] & ~others[region]
            # Rows from the frame's top: the bottom 35% never moves.
            row = np.arange(2 * y0, 2 * y1)[:, None]
            from_bottom = (2 * (bottom + 1) - row - 0.5) / (2 * q0["height"])
            base = diff & (from_bottom < 0.34)
            if base.sum() > 0:
                raise TestFailure(f"{q0['art']} at {q0['tile']}: {int(base.sum())} pixels moved near its base")
            if abs(q1["sway"] - q0["sway"]) >= 2 and (diff & (from_bottom > 0.7)).sum() > 0:
                moved += 1
        if moved < 2:
            raise TestFailure(f"only {moved} plant(s) moved at the top between the two wind times")
        # The ticker moves the plants by recomposing the HD layer alone: the
        # result equals a whole redraw at the same wind time.
        stats = json.loads((out / "t2.json").read_text())["wind"]["stats"]
        if stats["recomposes"] == 0:
            raise TestFailure("the ticker never recomposed a plant")
        recomposed, redrawn = load_png(out / "recomposed.png"), load_png(out / "redrawn.png")
        changed = float((np.abs(recomposed - redrawn).sum(axis=2) > 0).mean())
        if changed > 0.002:
            raise TestFailure(f"recomposed plants differ from a redraw on {changed:.2%} of the pixels")
        return (f"{len(p0)} plants on screen, {moved} swayed, recomposed = redraw ({changed:.3%} differ), "
                f"8-bit buffer unchanged ({seconds:.0f}s)")
    test.__name__ = f"test_hd_wind_slot{slot:02d}"
    return test


TESTS = ([test_main_menu, test_menu_fades, test_main_menu_hd, test_main_menu_hd_art, test_main_menu_hd_fhd,
          test_hd_art_lookup]
         + [make_slot_test(slot) for slot in range(1, SLOT_COUNT + 1)]
         + [make_slot_test(slot, hd=True) for slot in HD_SLOTS]
         + [test_stimpak_save_unchanged, test_character_screens, test_stimpak_edit_player, test_stimpak_cancel,
            test_stimpak_edit_companion, test_stimpak_lists, test_stimpak_perk_scan,
            test_english_interface_bar, test_load_save_with_hidden_bar]
         + [make_hd_screens_test(pack) for pack in ("scripted", "ai")]
         + [make_hd_map_test(pack, slot) for pack in HD_MAP_PACKS for slot in range(1, SLOT_COUNT + 1)]
         + [test_hd_mixed_pack]
         + [make_hd_text_screens_test(style) for style in HD_TEXT_STYLES]
         + [test_hd_text_game, test_hd_lights, test_worldmap_scroll, test_hd_ground_variants]
         + [make_hd_town_test(*town) for town in HD_TOWNS]
         + [make_hd_inventory_test(pack) for pack in HD_INVENTORY_PACKS]
         + [make_hd_static_test(pack) for pack in HD_STATIC_PACKS]
         + [make_hd_bar_test(pack) for pack in HD_BAR_PACKS]
         + [test_view_identity, test_view_pan, test_view_zoom, test_view_zoom_hd0, test_view_light_follow]
         + [test_hd_shot_light] + [make_hd_wind_test(slot) for slot in WIND_SLOTS])


PERF_CONFIGS = [
    ("1280x960 scale 2", ["[screen]resolution_x=1280", "[screen]resolution_y=960", "[screen]scale=2"]),
    ("3440x1440 scale 2", ["[screen]resolution_x=3440", "[screen]resolution_y=1440", "[screen]scale=2"]),
    ("3440x1440 hd=1", ["[screen]resolution_x=3440", "[screen]resolution_y=1440", "[screen]scale=2", *HD_ART_ARGS]),
    ("3440x1440 hd=1 scripted", ["[screen]resolution_x=3440", "[screen]resolution_y=1440", "[screen]scale=2", *HD_ARGS,
                                 "[graphics]hd_pack=scripted"]),
    ("3440x1440 hd=1 ai", ["[screen]resolution_x=3440", "[screen]resolution_y=1440", "[screen]scale=2", *HD_ARGS,
                           "[graphics]hd_pack=ai"]),
    ("3440x1440 hd=1 mixed", ["[screen]resolution_x=3440", "[screen]resolution_y=1440", "[screen]scale=2", *HD_ARGS,
                              "[graphics]hd_pack=mixed"]),
]


def run_perf(slot):
    """Presenter timings (not a pass/fail test): idle map and edge scrolling."""
    print(f"presenter timings, slot {slot} (avg/max ms per renderPresent; convert = index->RGB total ms)")
    for label, overrides in PERF_CONFIGS:
        reset_saves()
        out, _ = run_game("perf_" + label.replace(" ", "_").replace("=", ""), "perf_game.txt", [f"--dev-load-game={slot}", *overrides])
        for phase in ("idle", "scroll", "full"):
            stats = json.loads((out / f"{phase}.json").read_text())
            fb = "x".join(str(v) for v in stats["framebuffer"])
            frames = max(stats["presents"], 1)
            work = (stats["present_avg_ms"] * stats["presents"] + stats["map_refresh_total_ms"] + stats["scroll_total_ms"]) / frames
            print(f"  {label:24s} fb {fb:9s} {phase:6s} presents {stats['presents']:5d} "
                  f"({stats['presents'] / max(stats['elapsed_ms'], 1) * 1000:5.1f}/s)  "
                  f"present avg {stats['present_avg_ms']:6.3f} max {stats['present_max_ms']:7.3f}  "
                  f"map redraw {stats['map_refreshes']:4d} x {stats['map_refresh_total_ms'] / max(stats['map_refreshes'], 1):6.3f} "
                  f"max {stats['map_refresh_max_ms']:7.3f}  "
                  f"scroll {stats['scrolls']:3d} x {stats['scroll_total_ms'] / max(stats['scrolls'], 1):6.3f} "
                  f"max {stats['scroll_max_ms']:7.3f}  HD tiles {stats['hd_tiles_total_ms']:7.1f} objects "
                  f"{stats.get('hd_objects_total_ms', 0):7.1f} ms  work/frame {work:6.3f} ms")
        bench = json.loads((out / "bench.json").read_text())
        if bench["count"]:
            r, t, o = bench["refresh_ms"], bench["hd_tiles_ms"], bench.get("hd_objects_ms") or {"median": 0}
            print(f"  {label:24s} whole-map redraw x{bench['count']}: min {r['min']:6.2f} median {r['median']:6.2f} "
                  f"max {r['max']:6.2f} ms; HD tiles median {t['median']:6.2f} ms, HD objects median {o['median']:6.2f} ms")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--perf", type=int, metavar="SLOT", help="print presenter timings for a save slot and exit")
    parser.add_argument("--update-golden", action="store_true", help="overwrite golden files with this run's output")
    parser.add_argument("-k", dest="pattern", help="run only tests whose name contains this text")
    parser.add_argument("-j", "--jobs", type=int, default=min(6, max(1, (os.cpu_count() or 2) // 2)),
                        help="tests run at once, each in its own worker directory (default %(default)s)")
    args = parser.parse_args()

    if not EXE.exists():
        sys.exit(f"game not built: {EXE}")

    if args.perf:
        run_perf(args.perf)
        reset_saves()
        return 0

    selected = [t for t in TESTS if not args.pattern or args.pattern in t.__name__]
    jobs = max(1, min(args.jobs, len(selected)))
    started = time.time()

    # Worker directories, one per parallel job (job 0 = the run directory itself
    # when running serially).
    free_dirs = [RUN_DIR] if jobs == 1 else [prepare_worker(index) for index in range(jobs)]
    free_lock = threading.Lock()
    print_lock = threading.Lock()
    results = {}

    def run_one(test):
        with free_lock:
            directory = free_dirs.pop()
        _worker.run_dir = directory
        try:
            note = test(args.update_golden)
            outcome = ("PASS", note)
        except TestSkipped as reason:
            outcome = ("SKIP", reason)
        except TestFailure as error:
            outcome = ("FAIL", error)
        except Exception as error:  # a broken test must not stop the others
            outcome = ("FAIL", f"{type(error).__name__}: {error}")
        finally:
            with free_lock:
                free_dirs.append(directory)
        with print_lock:
            print(f"{outcome[0]} {test.__name__}: {outcome[1]}", flush=True)
        results[test.__name__] = outcome[0]

    # Longest tests first so the pool finishes together.
    order = sorted(selected, key=lambda t: 0 if any(k in t.__name__ for k in ("hd_screens", "hd_text", "hd_inventory", "hd_static", "hd_bar", "stimpak", "lists")) else 1)
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        list(pool.map(run_one, order))

    _worker.run_dir = RUN_DIR
    reset_saves()
    failures = sum(1 for r in results.values() if r == "FAIL")
    skipped = sum(1 for r in results.values() if r == "SKIP")
    print(f"{len(selected) - failures - skipped}/{len(selected)} passed" + (f", {skipped} skipped" if skipped else "")
          + f" in {time.time() - started:.0f}s with {jobs} job(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
