#!/bin/bash
# Build the game, prepare the run directory and run the integration tests.
# Arguments are passed to tests/run_tests.py (e.g. -k slot01, --update-golden).

set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_DIR/out/build/macos}"

if [[ ! -x "$REPO_DIR/.venv/bin/python" ]]; then
    python3 -m venv "$REPO_DIR/.venv"
    "$REPO_DIR/.venv/bin/pip" install -q -r "$REPO_DIR/tools/dev/requirements.txt"
fi

if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
    cmake -S "$REPO_DIR" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Debug
fi
cmake --build "$BUILD_DIR" --target fallout2-ce ce-dat-tool ce-frm2png | grep -E "error|FAILED" || true

if [[ ! -e "$BUILD_DIR/master.dat" ]]; then
    "$REPO_DIR/tools/dev/setup-run-dir.sh"
fi

"$REPO_DIR/.venv/bin/python" "$REPO_DIR/tests/run_tests.py" "$@"
