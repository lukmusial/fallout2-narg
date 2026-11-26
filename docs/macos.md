## Installation on macOS

Needs macOS 11 or later (Apple Silicon or Intel) and the game's data: the folder with `master.dat`, `critter.dat` and `data/` of a Windows installation (GOG, Steam) or extracted from the GOG offline installer with [innoextract](https://constexpr.org/innoextract/) (`brew install innoextract; innoextract setup_fallout_2_*.exe -d Fallout2`). Lower-case file names, as the engine expects.

1. Optional: install the [Restoration Project](rpu.md) first.
1. Extract `narg_v*_macos.zip` into the game folder. It adds `Fallout 2 Narg Edition.app`, `narg/` and the installer; nothing of the game is overwritten.
1. Double-click `narg-install.command` (or run `./narg-install.command [options]` in Terminal; options in the [README](../README.md#options)). It sets up `fallout2.cfg` for HD (screen size detected), installs `f2_res.dat` when the game has none, and saves what it changes to `backup/narg/`.
1. Start `Fallout 2 Narg Edition.app`. The first time, macOS may ask to confirm opening an app from the internet: right-click it, **Open**.

The app reads the game from the folder it is in. Settings: `fallout2.cfg` in the same folder (`[graphics]`, `[screen]`).
