#!/usr/bin/env python3
"""Hidden compatibility runs of a build against an installed game (a run
directory made by tools/dev/setup-game-run-dir.sh), e.g. on top of the
Fallout 2 Restoration Project.

  compat_run.py <run dir> <name> <script> [config overrides ...] [--scan] [--timeout S]

<script> is a --dev-script file, or `towns:<map>,<map>,...[@<first slot>]`:
loads save slot 1, moves the party to each map with the HD harness command
`gotomap` and takes a screenshot and a state dump there; with @<first slot>
each town is also saved, to consecutive slots from <first slot> (for builds
without `gotomap`, which then load those saves). Overrides are passed to the
game as `[section]key=value`.

Writes <run dir>/devout/compat/<name>/: the script's screenshots and dumps,
stdout.txt (the --scan-unimplemented report goes there), debug.log and
result.json (exit code, time, script errors counted in debug.log).
The game always runs hidden (--dev-script); nothing is shown or focused.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

APP = "Fallout II Community Edition.app"

SCRIPT_ERROR = re.compile(r"(error|unknown|failed|can't|cannot|not found|invalid)", re.I)


def towns_script(spec: str) -> str:
    first_slot = None
    if "@" in spec:
        spec, slot = spec.rsplit("@", 1)
        first_slot = int(slot)
    lines = ["# generated: towns " + spec, "until game 60000", "wait 2000"]
    for index, town in enumerate(m for m in spec.split(",") if m):
        lines += [f"gotomap {town}", "wait 6000", f"shot {town}", f"dump {town}"]
        if first_slot is not None:
            # Save screen: Home selects slot 1, Down moves one slot.
            lines += ["key f4", "wait 1500", "key home"]
            if first_slot + index > 1:
                lines.append(f"key down {first_slot + index - 1}")
            lines += ["key enter", "until input 10000", f"text {town}", "key enter", "wait 2500"]
    lines.append("quit 0")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("name")
    parser.add_argument("script")
    parser.add_argument("overrides", nargs="*")
    parser.add_argument("--scan", action="store_true", help="add --scan-unimplemented")
    parser.add_argument("--load", type=int, help="--dev-load-game=<slot>")
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--app", default=APP, help=f"app bundle in the run directory (default {APP})")
    args = parser.parse_args()

    run_dir = args.run_dir.resolve()
    out = run_dir / "devout" / "compat" / args.name
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    if args.script.startswith("towns:"):
        script = out / "towns_script.txt"
        script.write_text(towns_script(args.script[len("towns:"):]))
        if args.load is None:
            args.load = 1
    else:
        script = Path(args.script).resolve()

    exe = run_dir / args.app / "Contents/MacOS/Fallout II Community Edition"
    cmd = [str(exe), f"--dev-script={script}", f"--dev-out={out}", "--dev-fast", "[debug]mode=log"]
    if args.scan:
        cmd.append("--scan-unimplemented")
    if args.load is not None:
        cmd.append(f"--dev-load-game={args.load}")
    cmd += args.overrides

    log = run_dir / "debug.log"
    log.unlink(missing_ok=True)
    started = time.time()
    try:
        result = subprocess.run(cmd, cwd=run_dir, capture_output=True, text=True, errors="replace",
                                timeout=args.timeout)
        code = result.returncode
        stdout, stderr = result.stdout, result.stderr
    except subprocess.TimeoutExpired as e:
        code = -1
        stdout = (e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        stderr = "timeout"
    elapsed = time.time() - started

    (out / "stdout.txt").write_text(stdout)
    (out / "stderr.txt").write_text(stderr)
    errors = []
    if log.exists():
        shutil.copyfile(log, out / "debug.log")
        for line in log.read_text(errors="replace").splitlines():
            if SCRIPT_ERROR.search(line):
                errors.append(line.strip())
    summary = {
        "name": args.name,
        "command": cmd[1:],
        "exit": code,
        "seconds": round(elapsed, 1),
        "debug_log_error_lines": len(errors),
        "debug_log_errors_unique": sorted(set(errors))[:200],
        "shots": sorted(p.name for p in out.glob("*.png")),
    }
    (out / "result.json").write_text(json.dumps(summary, indent=2))
    print(f"{args.name}: exit {code}, {elapsed:.0f}s, {len(errors)} error lines in debug.log, "
          f"{len(summary['shots'])} shots -> {out}")
    return 0 if code == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
