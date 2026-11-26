## Changelog

- [Version 1.1.0](#version-110)
- [Version 1.0.0](#version-100)

### Version 1.1.0

#### HD without distributed art

- The archives hold no art. The engine upscales every picture of the game itself, from your own copy (and from a content mod's art, such as the Restoration Project's), with a C++ version of the deterministic upscale the scripted pack was made with: palette-aware de-dithering, edge-directed 2x interpolation, gradient restoration, sharpening; 16-bit colour (`hd_upscale_precision=16`) with de-banding where the picture is smooth. SIMD (NEON, SSE2) on all CPU cores.
- Characters (critters) are upscaled too (`hd_upscale_critters=1`).
- Each map is prepared while it loads: floor and roof tiles (upscaled with their real neighbours on the map, so no seams), walls, scenery, items and every character's standing, walking and running animations, nearest to the view first. Nothing is upscaled while you play.
- A loading screen with a progress bar shows while a map is prepared (`hd_loading_screen`); the game's splash pictures are upscaled.
- The world map, town maps, help and death screens, other elevations and the rest of the characters' animations are prepared in idle time.
- Disk cache `data/hd/cache` (`hd_upscale_cache=1`, at most `hd_upscale_cache_mb=2048`, oldest removed first): a second visit to a town loads in 0.1-0.3 s instead of 0.2-0.8 s.
- New settings component `--with pixel_doubled` (`hd_upscale=double`): the original art doubled, no upscaling.
- HD art packs are optional and built locally ([HD art packs](../components/hd_packs/README.md)); a pack, when named in `hd_pack`, still wins for the pictures it was made from. Ground variants need a pack.

#### Installer

- No `mods/*.dat` to add: without `--pack` the installer takes `narg_hd*.dat` out of `mods/mods_order.txt` (the files stay) and sets `hd_pack=` (empty) and the cache settings.

#### Tools

- `narg/rpu-save-convert.py`: saves from the unmodded game load on the Restoration Project (new global variables and party members added; `--restore` brings back the saves RPU's installer moved to `backup/`). See [RPU notes](rpu.md).

#### Engine

- Fixed: "Heap Error: Attempt to unlock a previously unlocked block" in `debug.log` after skipping the intro movie with HD lights on.
- Builds on Linux and Windows (CI).

### Version 1.0.0

First release.

#### Engine

- Fallout 2 Community Edition at c30f1efc, with:
  - HD rendering (`[graphics] hd=1`, `[screen] scale=2`): true colour at twice the logical resolution; HD art for the main menu, load/save, character editor, dialogue, Pip-Boy, world map, inventory, interface bar, floors, roofs, walls, scenery and items; 2x text; coloured lights, glow and soft shadows; a short light from every ranged attack and a flash at explosions; trees, bushes and grass swaying in the wind; smooth map scrolling and 0.5x-2x map zoom.
  - HD art is only used while the game's own picture is the one it was made from (`[graphics] hd_verify`, `hd/<pack>/legacy.txt`): art replaced by a mod such as the Restoration Project is drawn from the mod.
  - Stimpak 1.0.2, the savegame editor (main menu, **STIMPAK**).

#### HD art

- `mods/narg_hd.dat`: the AI-upscaled pack (`ai`, about 7650 images and `.fhd` multi-frame files) plus the 141 scripted images the `mixed` pack takes for small arrows, digits, lettered buttons, corrugated and striped roofs and a few objects; each pack's `legacy.txt` lists the 8231 source frames.
- `mods/narg_hd_scripted.dat` (optional): the scripted pack.
- Ground variants for desert, asphalt, pavement and grass.
- Characters keep their original art.

#### Installer

- `narg-install.sh` / `.command` / `.bat`: for vanilla Fallout 2 and for the Restoration Project; backs up what it changes to `backup/narg/`; detects the screen size; options `--pack`, `--with <component>`, `--resolution`.
