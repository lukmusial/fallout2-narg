#!/usr/bin/env python3
"""Phase 2.13 proposal: smooth light circle, smooth panning, map zoom.

Runs the game unattended (hidden window, no audio, --dev-script) at
3440x1440 (scale 2, logical 1720x720) with the `mixed` pack, HD text and HD
lights, and writes to docs/screens/phase2.13_motion/:

  light_strip.png        the player walking at night (Den, slot 8): every
                         6th presented frame, hex by hex (before) and
                         following the sprite (after), with the sprite's
                         feet (green) and the measured light centre (yellow)
  light_track.png        light centre vs sprite position, frame by frame
  pan_strip.png          8 consecutive frames of a held scroll, original
                         steps (before) and smooth panning (after)
  pan_track.png          how far the map moved in each presented frame
  zoom_den_050.png ...   full 3440x1440 frames at zoom 0.5, 1 and 2, the Den
  zoom_arroyo_*.png      at night and Arroyo by day
  keys.png               the key bindings and settings
  perf.json              timings per frame at 3440x1440 (Release build)

  motion_sheet.py [--out DIR] [--reuse]

Needs a build (HD_BUILD_DIR, default out/build/macos; use the Release build
for the timings: HD_BUILD_DIR=out/build/macos-release), its run directory
(tools/dev/setup-run-dir.sh) and the packs.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

import hdlib

RUN_DIR = hdlib.BUILD_DIR
EXE = RUN_DIR / "Fallout II Community Edition.app/Contents/MacOS/Fallout II Community Edition"
SCRIPTS = hdlib.REPO_ROOT / "tests/scripts"
WORK = RUN_DIR / "devout/phase2.13_motion"
BASE = ["[screen]resolution_x=3440", "[screen]resolution_y=1440", "[screen]scale=2", "[graphics]hd=1",
        "[graphics]hd_pack=mixed", "[graphics]hd_text=1", "[graphics]hd_lights=1"]
VIEW = ["[graphics]smooth_scroll=1", "[graphics]zoom=1"]
BG = (34, 34, 40)
PAD = 8
MAX_BYTES = 8 * 1024 * 1024

sys.path.insert(0, str(hdlib.REPO_ROOT / "tests"))


def font(size: int = 18):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # Pillow < 10.1
        return ImageFont.load_default()


def run(name: str, script: str, slot: int, args: list[str], reuse: bool) -> Path:
    out = WORK / name
    if reuse and (out / "done").exists():
        return out
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    subprocess.run([str(hdlib.REPO_ROOT / "tools/dev/reset-saves.sh")], check=True, stdout=subprocess.DEVNULL,
                   env={"RUN_DIR": str(RUN_DIR), "PATH": "/usr/bin:/bin"})
    subprocess.run([str(EXE), f"--dev-script={SCRIPTS / script}", f"--dev-out={out}", f"--dev-load-game={slot}", *BASE, *args],
                   cwd=RUN_DIR, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=240)
    (out / "done").write_text("")
    print(f"ran {name}")
    return out


def save(image: Image.Image, path: Path) -> None:
    image.save(path, optimize=True)
    size = path.stat().st_size
    if size > MAX_BYTES:
        raise SystemExit(f"{path} is {size / 1e6:.1f} MB (limit 8 MB)")
    print(f"wrote {path} ({size / 1e6:.2f} MB)")


def strip(title: str, rows: list[tuple[str, list[Image.Image], list[str]]]) -> Image.Image:
    f = font(20)
    small = font(16)
    cell_w = rows[0][1][0].width
    cell_h = rows[0][1][0].height
    width = PAD + max(len(cells) for _, cells, _ in rows) * (cell_w + PAD)
    height = 40 + len(rows) * (cell_h + 52)
    canvas = Image.new("RGB", (width, height), BG)
    draw = ImageDraw.Draw(canvas)
    draw.text((PAD, 10), title, fill=(235, 235, 235), font=f)
    y = 40
    for label, cells, captions in rows:
        draw.text((PAD, y + 4), label, fill=(230, 210, 140), font=f)
        for index, (cell, caption) in enumerate(zip(cells, captions)):
            x = PAD + index * (cell_w + PAD)
            draw.text((x, y + 30), caption, fill=(190, 190, 190), font=small)
            canvas.paste(cell, (x, y + 50))
        y += cell_h + 52
    return canvas


def chart(title: str, series: list[tuple[str, list[float], list[float], tuple[int, int, int]]], xlabel: str, ylabel: str,
          size=(1400, 520), steps=False) -> Image.Image:
    w, h = size
    left, right, top, bottom = 90, 30, 50, 60
    canvas = Image.new("RGB", size, BG)
    draw = ImageDraw.Draw(canvas)
    f = font(18)
    draw.text((left, 12), title, fill=(235, 235, 235), font=f)
    xs = [x for _, sx, _, _ in series for x in sx]
    ys = [y for _, _, sy, _ in series for y in sy]
    x0, x1 = min(xs), max(xs)
    y0, y1 = min(ys), max(ys)
    if y1 - y0 < 1:
        y1 = y0 + 1
    span = y1 - y0
    y0 -= span * 0.05
    y1 += span * 0.05

    def to(x, y):
        return (left + (x - x0) / max(x1 - x0, 1e-6) * (w - left - right), h - bottom - (y - y0) / (y1 - y0) * (h - top - bottom))

    draw.rectangle((left, top, w - right, h - bottom), outline=(110, 110, 110))
    for k in range(6):
        yv = y0 + (y1 - y0) * k / 5
        _, py = to(x0, yv)
        draw.line((left, py, w - right, py), fill=(55, 55, 62))
        draw.text((8, py - 9), f"{yv:.0f}", fill=(170, 170, 170), font=font(14))
    draw.text((left, h - 30), xlabel, fill=(170, 170, 170), font=font(15))
    draw.text((8, top - 30), ylabel, fill=(170, 170, 170), font=font(15))
    for index, (label, sx, sy, colour) in enumerate(series):
        points = [to(x, y) for x, y in zip(sx, sy)]
        if steps:
            stepped = []
            for (ax, ay), (bx, by) in zip(points, points[1:]):
                stepped += [(ax, ay), (bx, ay)]
            stepped.append(points[-1])
            points = stepped
        draw.line(points, fill=colour, width=3)
        for x, y in [to(x, y) for x, y in zip(sx, sy)]:
            draw.ellipse((x - 3, y - 3, x + 3, y + 3), fill=colour)
        draw.text((w - right - 520, top + 8 + index * 24), "-- " + label, fill=colour, font=font(16))
    return canvas


def light_sheet(out: Path, reuse: bool) -> None:
    from run_tests import light_track  # the same measure as the test

    runs = {label: run(f"light_{label}", "view_light_walk.txt", 8, extra, reuse)
            for label, extra in (("before", ["[graphics]hd_lights_follow=0"]), ("after", []))}
    tracks = {label: light_track(path) for label, path in runs.items()}
    rows = []
    for label, title in (("before", "before: the light jumps hex by hex"), ("after", "after: the light follows the sprite")):
        meta = json.loads((runs[label] / "walk.json").read_text())
        left, top, width, height = meta["crop"]
        cells, captions = [], []
        for index in range(6, 66, 6):
            image = Image.open(runs[label] / f"walk_{index:02d}.png").convert("RGB")
            draw = ImageDraw.Draw(image)
            feet, centre, frame = tracks[label][index]
            fx, fy = frame["dude"]["feet_screen"]
            px, py = fx * 2 - left, fy * 2 - top
            draw.line((px, py - 8, px, py + 30), fill=(60, 230, 60), width=5)
            cx = centre * 2
            draw.line((cx - 18, py + 60, cx + 18, py + 60), fill=(255, 220, 40), width=5)
            draw.line((cx, py + 42, cx, py + 78), fill=(255, 220, 40), width=5)
            cells.append(image.resize((width * 9 // 20, height * 9 // 20), Image.LANCZOS))
            captions.append(f"frame {index}  sprite x {feet:.0f}  light {centre:.0f}")
        rows.append((title, cells, captions))
    save(strip("Light circle at night (Den, 3440x1440): every 6th presented frame while walking east; green = the sprite's "
               "feet, yellow = centre of the light on the floor", rows), out / "light_strip.png")

    series = []
    for label, colour in (("before", (230, 110, 90)), ("after", (110, 200, 120))):
        t0 = tracks[label][0][2]["ticks"]
        series.append((f"{label}: light centre", [r[2]["ticks"] - t0 for r in tracks[label]], [r[1] for r in tracks[label]], colour))
    t0 = tracks["after"][0][2]["ticks"]
    series.append(("sprite feet (after run)", [r[2]["ticks"] - t0 for r in tracks["after"]], [r[0] for r in tracks["after"]],
                   (120, 160, 240)))
    save(chart("Light centre on the floor vs. time while the player walks (logical x)", series, "ms", "x (logical px)"),
         out / "light_track.png")


def pan_sheet(out: Path, reuse: bool) -> None:
    runs = {label: run(f"pan_{label}", "view_pan.txt", 8, extra, reuse)
            for label, extra in (("before", []), ("after", ["[graphics]smooth_scroll=1"]))}
    rows = []
    series = []
    for label, title, colour in (("before", "before: steps of a tile (32 px) every other frame", (230, 110, 90)),
                                 ("after", "after: smooth, every frame", (110, 200, 120))):
        frames = json.loads((runs[label] / "pan.json").read_text())["frames"]
        xs = [f["dude"]["feet_screen"][0] for f in frames]
        ticks = [f["ticks"] for f in frames]
        start = next(i for i in range(1, len(xs)) if xs[i] != xs[0]) - 1
        cells, captions = [], []
        for index in range(start, start + 8):
            image = Image.open(runs[label] / f"pan_{index:02d}.png").convert("RGB")
            cells.append(image.resize((image.width * 2 // 5, image.height * 2 // 5), Image.LANCZOS))
            moved = xs[index] - xs[index - 1] if index > 0 else 0
            captions.append(f"+{ticks[index] - ticks[start]} ms  moved {abs(moved):.1f} px")
        rows.append((title, cells, captions))
        moves = [abs(b - a) for a, b in zip(xs, xs[1:])]
        series.append((f"{label}", [t - ticks[0] for t in ticks[1:]], moves, colour))
    save(strip("Held scroll (Den, 3440x1440): 8 consecutive presented frames; the map moves left", rows), out / "pan_strip.png")
    save(chart("Map movement per presented frame during a held scroll (logical px)", series, "ms", "px per frame"),
         out / "pan_track.png")


def zoom_sheet(out: Path, reuse: bool) -> None:
    for name, slot in (("den", 8), ("arroyo", 3)):
        path = run(f"zoom_{name}", "view_zoom.txt", slot, VIEW, reuse)
        for zoom in ("050", "100", "200"):
            image = Image.open(path / f"zoom{zoom}.png").convert("RGB")
            save(image, out / f"zoom_{name}_{zoom}.png")


def keys_card(out: Path) -> None:
    lines = [
        "Phase 2.13 - keys and settings",
        "",
        "Zoom the map view (with [graphics] zoom=1):",
        "   mouse wheel over the map     zoom in / out (0.5x, 0.75x, 1x, 1.25x, 1.5x, 2x)",
        "   Ctrl + =   (Ctrl + Shift + =)    zoom in",
        "   Ctrl + -                          zoom out",
        "   Ctrl + 0                          back to 1x",
        "   (+ / - stay brightness; the horizontal wheel still scrolls)",
        "",
        "Scrolling is unchanged: arrow keys, mouse at the screen edge, Home centres on the player.",
        "",
        "fallout2.cfg [graphics]:",
        "   hd_lights_follow=1   the light a walking critter carries follows the sprite (needs hd_lights=1)",
        "   smooth_scroll=1      smooth panning, every frame",
        "   scroll_speed=100     panning speed in % of the original",
        "   zoom=1               map zoom (the zoom lasts for the session, saves are not touched)",
    ]
    f = font(24)
    canvas = Image.new("RGB", (1500, 40 + 34 * len(lines)), BG)
    draw = ImageDraw.Draw(canvas)
    for index, line in enumerate(lines):
        draw.text((24, 20 + index * 34), line, fill=(235, 235, 200) if index == 0 else (215, 215, 215), font=f)
    save(canvas, out / "keys.png")


def perf(out: Path, reuse: bool) -> None:
    path = run("perf", "view_perf.txt", 8, VIEW, reuse)
    result = {}
    for phase in ("idle1", "scroll1", "scroll05", "scroll2", "zoomanim", "walk"):
        d = json.loads((path / f"{phase}.json").read_text())
        v = json.loads((path / f"{phase}_view.json").read_text())
        n = max(d["presents"], 1)
        result[phase] = {
            "presents_per_s": round(d["presents"] * 1000 / d["elapsed_ms"], 1),
            "present_ms": round(d["present_avg_ms"], 2),
            "lights_ms_per_frame": round(d["light_total_ms"] / n, 2),
            "map_view_ms_per_frame": round(v["present_total_ms"] / n, 2),
            "resample_ms_per_frame": round(v["resample_total_ms"] / n, 2),
            "scroll_step_ms": round(d["scroll_total_ms"] / max(d["scrolls"], 1), 2),
            "work_ms_per_frame": round(d["present_avg_ms"] + (v["present_total_ms"] + d["scroll_total_ms"]
                                                              + d["map_refresh_total_ms"]) / n, 2),
        }
    (out / "perf.json").write_text(json.dumps(result, indent=1) + "\n")
    for phase, values in result.items():
        print(f"  {phase:9s} {values}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=hdlib.REPO_ROOT / "docs/screens/phase2.13_motion")
    parser.add_argument("--reuse", action="store_true")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    WORK.mkdir(parents=True, exist_ok=True)
    light_sheet(args.out, args.reuse)
    pan_sheet(args.out, args.reuse)
    zoom_sheet(args.out, args.reuse)
    keys_card(args.out)
    perf(args.out, args.reuse)


if __name__ == "__main__":
    main()
