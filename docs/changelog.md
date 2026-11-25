## Changelog

- [Version 1.0.0](#version-100)

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
