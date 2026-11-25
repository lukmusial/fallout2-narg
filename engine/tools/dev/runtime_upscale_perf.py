#!/usr/bin/env python3
"""Timings of the runtime scripted upscale (Phase 2.22): per save slot and
configuration, the map load, the frames queued at the load and how long the
background threads took, main-thread time on runtime frames, presenter and
redraw timings, memory of the HD art cache. Hidden runs (tools/dev/hd_run.py).

  runtime_upscale_perf.py [--slots 3,1,8] [--configs name,...]
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUN = ROOT / "out/build/macos/hdrun"
HD = ["[graphics]hd=1", "[graphics]hd_text=1"]

# name: (packs linked, overrides, runs (the last one is reported; earlier
# ones fill the disk cache))
CONFIGS = {
    "offline-pack": ("ai,rpu,scripted", ["[graphics]hd_pack=scripted", "[graphics]hd_upscale=double"], 1),
    "double": ("ai,rpu", ["[graphics]hd_pack=scripted", "[graphics]hd_upscale=double"], 1),
    "runtime": ("ai,rpu", ["[graphics]hd_pack=scripted"], 1),
    "runtime-no-preload": ("ai,rpu", ["[graphics]hd_pack=scripted", "[graphics]hd_upscale_preload=0"], 1),
    "runtime-1-thread": ("ai,rpu", ["[graphics]hd_pack=scripted", "[graphics]hd_upscale_threads=1"], 1),
    "runtime-cache-cold": ("ai,rpu", ["[graphics]hd_pack=scripted", "[graphics]hd_upscale_cache=1"], 1),
    "runtime-cache-warm": ("ai,rpu", ["[graphics]hd_pack=scripted", "[graphics]hd_upscale_cache=1"], 2),
    "pack+runtime": ("ai,rpu,scripted", ["[graphics]hd_pack=scripted"], 1),
}


def run(name: str, slot: int, packs: str, overrides: list[str], keep: bool) -> Path:
    cmd = [sys.executable, str(ROOT / "tools/dev/hd_run.py"), name, "hd_upscale_perf.txt", "--slot", str(slot),
           "--packs", packs, *(["--keep-cache"] if keep else []), *HD, *overrides]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    return RUN / name / "devout"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--slots", default="3,1,8")
    ap.add_argument("--configs", default=",".join(CONFIGS))
    args = ap.parse_args()
    rows = []
    for slot in [int(s) for s in args.slots.split(",")]:
        for config in args.configs.split(","):
            packs, overrides, runs = CONFIGS[config]
            name = f"perf_{config}_{slot}"
            for index in range(runs):
                out = run(name, slot, packs, overrides, keep=index > 0)
            load = json.loads((out / "load_stats.json").read_text())
            done = json.loads((out / "map_stats.json").read_text())
            end = json.loads((out / "end_stats.json").read_text())
            idle = json.loads((out / "idle.json").read_text())
            scroll = json.loads((out / "scroll.json").read_text())
            bench = json.loads((out / "bench.json").read_text())
            log = (out / "debug.log").read_text(errors="replace")
            loads = [int(m) for m in re.findall(r"CE: map \S+ loaded in (\d+) ms", log)]
            rows.append({
                "slot": slot, "config": config,
                "map_load_ms": loads[-1] if loads else None,
                "queued": done["batch_queued"], "queue_ms": round(done["batch_queue_ms"], 1),
                "pre_done_ms": round(done["batch_done_ms"], 1),
                "main_at_load_ms": round(load["runtime_inline_ms"] + load["runtime_take_ms"], 1),
                "inline_frames_at_load": load["runtime_inline"] + load["runtime_from_pool"],
                "disk_hits": done["upscale_disk_hits"],
                "present_idle_ms": round(idle["present_avg_ms"], 3),
                "present_scroll_ms": round(scroll["present_avg_ms"], 3),
                "scroll_step_ms": round(scroll["scroll_total_ms"] / max(scroll["scrolls"], 1), 2),
                "redraw_median_ms": round(bench["refresh_ms"]["median"], 2) if bench["count"] else None,
                "cache_mb": round(end["bytes_cached"] / 1048576, 1),
                "frames": end["frames_cached"],
                "runtime_frames": end["runtime_served"],
            })
            print(json.dumps(rows[-1]), flush=True)
    keys = list(rows[0].keys())
    print("| " + " | ".join(keys) + " |")
    print("|" + "---|" * len(keys))
    for row in rows:
        print("| " + " | ".join(str(row[k]) for k in keys) + " |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
