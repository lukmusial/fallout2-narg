## Installation on Linux

Needs SDL2 (`sudo apt install libsdl2-2.0-0`) and the game's data: the folder with `master.dat`, `critter.dat` and `data/` of a Windows installation, or extracted from the GOG offline installer with innoextract (`innoextract setup_fallout_2_*.exe -d Fallout2`). On a case-sensitive file system the game's file names must be lower case.

1. Optional: install the [Restoration Project](rpu.md) first.
1. Extract `narg_v*_linux.zip` into the game folder.
1. Run `./narg-install.sh [options]` there (options in the [README](../README.md#options)). It installs `fallout2-ce` and `ce.dat` (backing up any existing ones to `backup/narg/`), sets up `fallout2.cfg` for HD (screen size from `xrandr` when available), adds the HD art to `mods/mods_order.txt` and installs `f2_res.dat` when the game has none.
1. Start `./fallout2-ce`.
