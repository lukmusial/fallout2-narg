## Changelog

### Version 1.0.0

First release.

#### Engine

- [Fallout 2 Community Edition](https://github.com/fallout2-ce/fallout2-ce) at c30f1efc, for macOS, Linux and Windows.
- HD rendering (`[graphics] hd=1`, `[screen] scale=2`): true colour at twice the logical resolution, 2x text.
- In-engine 2x upscale of every picture the game draws (interface, floors, roofs, walls, scenery, items, characters, world map, town maps, splash and help screens), made from your own copy of the game and from a content mod's art, such as the Restoration Project's: palette-aware de-dithering, edge-directed interpolation, gradient restoration and sharpening, in 16-bit colour with de-banding where the picture is smooth (`hd_upscale_precision=16`); SIMD (NEON, SSE2) on all CPU cores.
- Scene preparation: while a map loads, everything it can show at once is upscaled (floor and roof tiles with their real neighbours on the map, so no seams; walls, scenery, items; every character's standing, walking and running animations), nearest to the view first, behind a loading screen with a progress bar (`hd_loading_screen`). The world map, town maps, help and death screens, other elevations and other animations are prepared in idle time. Nothing is upscaled while you play.
- Disk cache `data/hd/cache` (`hd_upscale_cache`, at most `hd_upscale_cache_mb=2048`, oldest removed first): a town takes 0.2-0.8 s to prepare the first time, 0.1-0.3 s from the cache (Apple M5 Pro).
- The 800-pixel interface bar for large screens is built from your game's own `iface.frm`, in your game's language (a mod's own bar, such as RPU's, is used as is).
- Coloured lights, glow and soft shadows; a short light from every ranged attack and a flash at explosions; trees, bushes and grass swaying in the wind; smooth map scrolling and 0.5x-2x map zoom.
- [Stimpak](https://github.com/lukmusial/fallout2-stimpak) 1.0.2, the savegame editor (main menu, **STIMPAK**).
- Runs the Restoration Project's mods, scripts and art.

#### Installer

- `narg-install.sh` / `.command` / `.bat`, for vanilla Fallout 2 and for the Restoration Project: merges the settings into `fallout2.cfg`, detects the screen size, installs `f2_res.dat` when the game has none, and backs up what it changes to `backup/narg/`.
- Options `--with <component>` (`no_wind`, `no_shot_light`, `no_lights`, `pixel_doubled`, `classic`, `windowed`) and `--resolution`.

#### Tools

- `narg/rpu-save-convert.py`: saves from the unmodded game load on the Restoration Project (new global variables and party members added; `--restore` brings back the saves RPU's installer moved to `backup/`). See [RPU notes](rpu.md).
