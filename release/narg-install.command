#!/bin/bash
# Fallout 2: Narg Edition installer for macOS: double-click it in Finder (or
# run it from a terminal with the options of narg-install.sh, which it runs).
cd -- "$(dirname "$0")" || exit 1
exec /bin/bash ./narg-install.sh "$@"
