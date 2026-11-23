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
cmake --build "$BUILD_DIR" --target fallout2-ce ce-dat-tool ce-frm2png hd_layer_test | grep -E "error|FAILED" || true

# Unit tests (no game data).
ctest --test-dir "$BUILD_DIR" --output-on-failure | tail -n 3

if [[ ! -e "$BUILD_DIR/master.dat" ]]; then
    "$REPO_DIR/tools/dev/setup-run-dir.sh"
fi

# Placeholder HD art for the hd=1 tests (written into the run directory once).
"$REPO_DIR/.venv/bin/python" "$REPO_DIR/tools/dev/make_test_hd_art.py"

"$REPO_DIR/.venv/bin/python" "$REPO_DIR/tests/run_tests.py" "$@"
