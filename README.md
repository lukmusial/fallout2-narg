## Fallout 2: Narg Edition

[**Download**](#download)
| [**Installation**](#installation)
| [**Additional mods**](#additional-mods)
| [**Report issues**](#bug-reports)

**Fallout 2: Narg Edition** is Fallout 2 in high definition on a modern engine:

- the [Fallout 2 Community Edition](https://github.com/fallout2-ce/fallout2-ce) engine (CE), which replaces the game's executable, with
- **HD rendering**: true colour at twice the resolution, HD art for the interface, floors, roofs, walls, scenery, items and inventory, 2x text, coloured lights and soft shadows, a light from every gunshot, trees swaying in the wind, smooth scrolling and map zoom;
- **[Stimpak](https://github.com/lukmusial/fallout2-stimpak)**, a savegame editor on the main menu (character, companions, quests, locations, travel).

It installs on vanilla Fallout 2 and on top of the [Restoration Project](https://github.com/BGforgeNet/Fallout2_Restoration_Project) (RPU). Characters (critters) keep their original art.

> **The HD art is made from the game's own art.** Its images are upscaled from Fallout 2's art files, so you need your own copy of Fallout 2 ([GOG](https://www.gog.com/game/fallout_2), [Steam](https://store.steampowered.com/app/38410), [Epic](https://store.epicgames.com/p/fallout-2)) to play, and the archives are not meant to be redistributed without the game.

## Download

Releases are archives per platform: `narg_v<version>_macos.zip`, `narg_v<version>_linux.zip`, `narg_v<version>_windows.zip` (see [the changelog](docs/changelog.md)). Each holds:

| File | What it is |
|---|---|
| `Fallout 2 Narg Edition.app` (macOS), `narg/linux/fallout2-ce`, `narg/windows/fallout2-ce.exe` | the engine |
| `mods/narg_hd.dat` | the HD art: AI-upscaled pack, plus the scripted images the default `mixed` pack uses |
| `mods/narg_hd_scripted.dat` | optional: the whole scripted pack (`--pack scripted`) |
| `mods/narg_hd_rpu.dat` | separate archive `narg_v<version>_rpu.zip`: HD art for the Restoration Project's own images, loaded only on RPU |
| `narg/rpu-save-convert.py` | converts saves from the unmodded game for the Restoration Project ([RPU notes](docs/rpu.md)) |
| `mods_order.txt` | the mods the installer adds to `mods/mods_order.txt` |
| `narg/narg.cfg` | the settings the installer puts into `fallout2.cfg` |
| `narg/settings/*.cfg` | optional settings components (see below) |
| `narg/f2_res.dat` | hi-res interface art, installed when the game has none |
| `narg-install.sh` / `.command` / `.bat` | the installer |
| `narg/docs/` | this documentation |

## Installation

- Narg Edition installs on **vanilla Fallout 2** (GOG, Steam, CD) or on **Fallout 2 with RPU** (install RPU first, then Narg Edition).
- Saved games keep working; with RPU, start a new game after installing RPU as usual.
- Everything the installer replaces or changes is copied to `backup/narg/` first.

Per platform: [macOS](docs/macos.md) | [Linux](docs/linux.md) | [Windows](docs/windows.md) | [on top of RPU](docs/rpu.md).

In short: extract the archive into the game folder, run `narg-install.command` (macOS), `narg-install.sh` (Linux) or `narg-install.bat` (Windows), then start the game with `Fallout 2 Narg Edition.app` / `fallout2-ce` / `fallout2-ce.exe`.

### Options

| Option | Effect |
|---|---|
| `--pack mixed` (default) | AI-upscaled art, scripted (deterministic) upscale where the AI does poorly |
| `--pack ai` / `--pack scripted` | one pack only (`scripted` needs `mods/narg_hd_scripted.dat`) |
| `--with no_wind` | trees and bushes stand still |
| `--with no_shot_light` | no light from gunshots and explosions |
| `--with no_lights` | no coloured lights, glow or soft shadows (for slow machines) |
| `--with classic` | the original 256-colour look; engine, Stimpak and fixes stay |
| `--with windowed` | a window instead of full screen |
| `--resolution 2560x1440` | screen size, instead of the detected one |

All of them are settings in `fallout2.cfg` (`[graphics]`, `[screen]`) that you can change later. On Windows the options are written `-Pack ai -With no_wind,windowed -Resolution 2560x1440`.

### Compatibility with content mods

HD images are found by the art file they were made from. When a mod replaces a picture (RPU repaints the world map, adds real tiles where the game had placeholders, changes the dialogue frame, some items and the hi-res main menu), the engine compares the game's own picture with the one the HD image was made from (`hd/<pack>/legacy.txt`, `[graphics] hd_verify=1`) and draws the mod's picture, doubled, wherever they differ. New art from a mod is also drawn doubled. See [RPU notes](docs/rpu.md).

## Additional mods

### Included

- [Fallout 2 Community Edition](https://github.com/fallout2-ce/fallout2-ce) engine at c30f1efc
- [Stimpak](https://github.com/lukmusial/fallout2-stimpak) savegame editor
- HD rendering and HD art (this project)

### Recommended

- [Restoration Project](https://github.com/BGforgeNet/Fallout2_Restoration_Project) (tested with v2.4.34)
- the sfall mods CE supports, e.g. [FO2tweaks](https://github.com/BGforgeNet/FO2tweaks), [Inventory Filter](https://github.com/rotators/InventoryFilter)

## Bug reports

Describe the problem, attach a savegame and a screenshot (in HD, F12 saves a full-resolution PNG), and say whether RPU is installed.

## Additional info

- [Changelog](docs/changelog.md)
- [Known issues](docs/known.md)
- [Credits and licences](docs/credits.md)
- [Development: building the engine and the archives](docs/development.md)
- [Tests of the 1.0.0 release](docs/testing.md)
