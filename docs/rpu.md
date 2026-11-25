## Narg Edition on top of the Restoration Project

Tested with RPU v2.4.34 (manual archive, `rpu-install.command`, on macOS).

1. Install RPU on vanilla Fallout 2 as its [instructions](https://github.com/BGforgeNet/Fallout2_Restoration_Project/blob/master/docs/linux.md) say.
1. Install Narg Edition as for your platform. The installer sees `mods/rpu.dat`, keeps RPU's hi-res data (`mods\f2_res.dat`, from `f2_res.ini`), and adds `narg_hd.dat` at the end of RPU's `mods/mods_order.txt`.
1. Start a new game (RPU needs one).

What to expect:

- The engine is CE, not RPU's `fallout2.exe` + sfall. CE loads RPU's `mods/*.dat` in RPU's order and runs its scripts, and reads `ddraw.ini` (e.g. RPU's version string on the main menu). Its sfall support is broad but not complete: `--scan-unimplemented` lists three opcodes RPU's scripts use that CE lacks (`fs_seek`/`fs_read_short`/`fs_write_short` in the optional low-fps walking-speed and Goris de-robing fixes, `set_fake_perk` in one EPA script). The two global scripts stop with "Undefined opcode 8202" in `debug.log`; the game is not affected otherwise.
- Art RPU replaces (about 250 files: the world map, the dialogue frame, elevator panels, several inventory items, skilldex cards, about 170 tiles such as the Vault 13 shutters and placeholder tiles, some scenery) is drawn from RPU's picture, doubled, instead of from the HD pack. Art RPU adds (about 1900 files: new locations' tiles, walls and scenery) is drawn doubled too.
- RPU's `f2_res.ini` is only imported by CE when `fallout2.cfg` has no screen settings; the installer writes them (HD needs `scale=2` and at least 1280x960), so RPU's `f2_res.ini` screen size is not used. Edit `[screen]` in `fallout2.cfg` instead.
- Note for RPU's own installer on macOS: `rpu-install.command` moves `data/SAVEGAME` to `backup/rpu/data/` with the stock macOS `grep` (its `\|` alternation with anchors does not match there). Copy your saves back if you want them.
