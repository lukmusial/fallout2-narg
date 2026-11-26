## Narg Edition on top of the Restoration Project

Tested with RPU v2.4.34 (manual archive, `rpu-install.command`, on macOS).

1. Install RPU on vanilla Fallout 2 as its [instructions](https://github.com/BGforgeNet/Fallout2_Restoration_Project/blob/master/docs/linux.md) say.
1. Install Narg Edition as for your platform. The installer sees `mods/rpu.dat` and keeps RPU's hi-res data (`mods\f2_res.dat`, from `f2_res.ini`) and RPU's `mods/mods_order.txt`.
1. Start a new game, or bring your saves from before RPU (below).

### Saves from before RPU

RPU recommends a new game: it declares more global variables (791 instead of 696) and more party members (29 instead of 26) than the game's 1.02 patch, and an old save does not load with them ("Error reading load function #3 data" in `debug.log`). `narg/rpu-save-convert.py` (Python 3) converts saves made with the unmodded game:

```sh
python3 narg/rpu-save-convert.py . --restore
```

run from the game folder (on Windows `py narg\rpu-save-convert.py . --restore`). `--restore` first copies back the saves RPU's installer moved to `backup/rpu/data/SAVEGAME`; slots already in `data/SAVEGAME` stay as they are. Then every slot in `data/SAVEGAME` (or the slots named, e.g. `SLOT03`) gets RPU's new global variables with their starting values, and RPU's three new party members (perks, AI and level-up data, and their proto). The original is kept as `SAVE.DAT.vanilla` in the slot; slots already converted are left alone, and so are saves made during combat.

Tested with the ten reference saves (Arroyo, Klamath, the Den) on RPU v2.4.34: all load, and the player, party, quests and world state equal the same saves loaded without RPU. What stays from the old game:

- maps you have visited keep their state from the save, so RPU's changes to them (restored people, items, scenery) show only in maps you have not visited yet;
- the 12 locations RPU adds to the world map start as a new game has them (`debug.log` notes "number of cities 49 in the save file is different from the number of cities 61");
- quests RPU adds, or changes in places you have been, start as if new.

What to expect:

- The engine is CE, not RPU's `fallout2.exe` + sfall. CE loads RPU's `mods/*.dat` in RPU's order and runs its scripts, and reads `ddraw.ini` (e.g. RPU's version string on the main menu). Its sfall support is broad but not complete: `--scan-unimplemented` lists three opcodes RPU's scripts use that CE lacks (`fs_seek`/`fs_read_short`/`fs_write_short` in the optional low-fps walking-speed and Goris de-robing fixes, `set_fake_perk` in one EPA script). The two global scripts stop with "Undefined opcode 8202" in `debug.log`; the game is not affected otherwise.
- Art RPU replaces (309 files: the world map, the dialogue frame, elevator panels, several inventory items, skilldex cards, about 170 tiles, some scenery) and art it adds (1876 files: new locations' tiles, walls and scenery) is upscaled by the engine from RPU's own pictures, like the game's. Endgame slides have no HD drawing yet, on vanilla either.
- RPU's `f2_res.ini` is only imported by CE when `fallout2.cfg` has no screen settings; the installer writes them (HD needs `scale=2` and at least 1280x960), so RPU's `f2_res.ini` screen size is not used. Edit `[screen]` in `fallout2.cfg` instead.
- Note for RPU's own installer on macOS: `rpu-install.command` moves `data/SAVEGAME` to `backup/rpu/data/` with the stock macOS `grep` (its `\|` alternation with anchors does not match there). `rpu-save-convert.py --restore` copies them back (see above).
