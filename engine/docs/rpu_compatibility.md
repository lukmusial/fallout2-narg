# Fallout 2 Restoration Project (RPU) and this engine

Tested 2026-11-24/25 with RPU **v2.4.34** (`rpu_v2.4.34.zip`, sha256
`b8f8a9c9…5e`) installed into a copy of the local game
(`~/dev/rpu-test/game`; the installed game was only read) and three builds:

| Build | What |
|---|---|
| **narg** | this tree (branch `narg`): CE c30f1efc + HD + Stimpak, Release |
| **stimpak** | `~/dev/stimpak` `tools/build.sh`: CE c30f1efc + Stimpak, no HD, Release |
| **ce** | upstream CE c30f1efc, plain CMake Release build (built; not run: it has no hidden `--dev-script` mode, so it could only be launched visibly) |

All runs were hidden (`--dev-script`), with `tools/dev/compat_run.py` and the
scripts in `tests/compat/`, from run directories made by
`tools/dev/setup-game-run-dir.sh` (APFS clones of the game folder).

## 1. How RPU is packaged and released

Repository (github.com/BGforgeNet/Fallout2_Restoration_Project):

| Path | Content |
|---|---|
| `data/` | the mod's game files (text, maps, protos, art, compiled scripts go to `data/scripts`) |
| `scripts_src/` | SSL sources, compiled by `extra/buildall.sh` (sfall `compile.exe` under wine, `gcc -E` as preprocessor) |
| `components/<name>/` | optional parts, each packed to `mods/rpu_<name>.dat` (alternative explosions, enhanced world map, extended flamer, improved Mysterious Stranger, rifle and wakizashi animations) |
| `translations/<lang>/` | text and art per language, packed to `rpu_<lang>.dat` (`rpu_v*_translations.zip`), maintained through BGforge Hive (Weblate) with `.po` files |
| `release/` | files shipped as they are: `fallout2.exe`, hi-res patch (`f2_res.dll`, `f2_res.ini`, `f2_res_config.exe`, d3dx9 DLLs), `mods/{rpu,upu,elevators}.ini`, `mods_order.txt`, `rpu-install.{bat,sh,command}` |
| `extra/` | build: `env.sh` (versions, names), `full.sh` = `prepare.sh` (download sfall modders pack, `compile.exe`, headers) + `buildall.sh` + `package.sh`; `package/*.sh` pulls the other mods' latest GitHub releases (Hero Appearance, Party Orders, NPC Armor, HQ music, Cassidy head, BGforge f2_res) and sfall; `inno/` the Inno Setup installer |
| `docs/` | `changelog.md` (per version, "Version 34 (AKA 2.3.34, 2.4.34)", plus all UPU fixes), `linux.md` (manual install), `update.md`, `known.md`, `credits.md`, `translations.md`, killap's RP readme/changelog |
| `.github/workflows/` | `build.yml` (ShellCheck, `BGforgeNet/fallout-tests` script checks, wine + `full.sh`, artifacts on branches, `softprops/action-gh-release` + torrents on `v*` tags), `poify.yml`/`scanmsg.yml`/`json-snapshots.yml` (translation and data housekeeping) |

Release (`package.sh`): `data/` (without music) is packed with `dat2.exe` into
`mods/rpu.dat`; components into `mods/rpu_<name>.dat`; sfall's `ddraw.dll`,
`sfall.dat` and `ddraw.ini` are downloaded and RPU's settings merged into
`ddraw.ini` with crudini (`VersionString=FALLOUT II 1.02d RP <version>`, mods
folder for ini files, hero appearance, extra save slots, ...). Then:

- `rpu_v<ver>.zip`: the whole `release/` tree (manual install, Linux/macOS),
  867 MB: `mods/` (rpu.dat 130 MB, npc_armor.dat 83 MB, animation and
  component dats, `f2_res.dat`, ini files), `appearance/*.dat` (Hero
  Appearance), `data/sound/music` (HQ music), sfall files, `fallout2.exe`,
  `mods_order.txt`, the install scripts;
- `rpu_v<ver>.exe`: Inno Setup installer with components (language,
  optional dats, ini choices), built in docker;
- `rpu_v<ver>_translations.zip`.

Versioning: tags `v2.4.<n>`; untagged builds are `2.4.git<sha>`; the version
is written into `patchinf.msg` and `ddraw.ini`. `mods_order.txt` lists every
dat the full install could have (translations, optional low-fps fixes,
fo2tweaks, InventoryFilter); missing ones are skipped by sfall (and by CE).

Install (manual): extract into a **vanilla** game, run `rpu-install.sh` /
`.command`: abort on UP/UPU or a case-sensitive file system with upper-case
names, move `patch000.dat`, `f2_res.dat`, `data/*` (except saves, `*.txt`,
`*.gcd`, `sound/music`) and old appearance folders to `backup/rpu`, install
`mods_order.txt`, fix the music paths in `fallout2.cfg`, delete the
installers. A new game is required. The game is then run as
`WINEDLLOVERRIDES='ddraw.dll=n,b' wine fallout2.exe`.

**macOS finding:** with the stock macOS `grep` (BSD grep 2.6.0) the pattern
`^savegame$\|\.txt$\|\.gcd$\|^sound$` does not match `SAVEGAME`, so
`rpu-install.command` also moves **`data/SAVEGAME`** into `backup/rpu/data/`
(the saves are not lost, but they disappear from the game).

## 2. Results

| Check | narg | stimpak | Notes |
|---|---|---|---|
| RPU loads (mods_order, dats) | PASS | PASS | rpu.dat, party_orders, npc_armor, enhanced world map, animations, Cassidy, Mysterious Stranger loaded; 14 entries of `mods_order.txt` skipped (not shipped in the zip: translations, low-fps fixes, fo2tweaks, InventoryFilter). `mods/f2_res.dat` loaded through `f2_res.ini` `f2_res_dat` (imported by CE). `sfall.dat` and `appearance/*.dat` are not loaded by CE. |
| Main menu, hd=0 | PASS | PASS | RPU's hi-res menu (Enclave armour) at RPU's 1024x768, STIMPAK button, `ddraw.ini` version string "FALLOUT II 1.02d RP 2.4.34" |
| Main menu, hd=1 | PASS (with fix) | n/a | without the fix our pack's CE main menu (palm) covered RPU's picture; see section 3 |
| New game (RPU temple `ARTEMPLE.MAP`) | PASS | PASS | same dump in both builds |
| `--scan-unimplemented` | same report | same report | missing in CE: `set_fake_perk` (`epac3.int`), `fs_seek`/`fs_read_short`/`fs_write_short` (`gl_k_goris_derobing.int`, `gl_k_walking_speed.int`); sfall's own `gl_partycontrol.int` (sfall.dat, not loaded by CE) uses `set_dude_obj`, `HOOK_SETGLOBALVAR`, `read_byte` |
| Script errors at new game | `Undefined opcode 8202` x2 | same | the two `gl_k_*` global scripts stop at `fs_seek`: the optional low-fps walking speed / Goris de-robing fixes do not run under CE |
| Heap error after the intro movie | `Heap Error: Attempt to unlock a previously unlocked block` | none | **pre-existing HD bug**, also on vanilla with hd=1, not with hd=0 (aborting `elder.mve` at new game; probably the character selector's HD background lock). No visible effect. Not fixed here. |
| Towns via `gotomap` (HD) | PASS 14/17 | n/a | reached: Abbey, EPA, Slaver Camp, Umbra Tribe, Village (all RPU-only), Klamath, Den, Vault City, New Reno, Arroyo, Redding, San Francisco, Navarro, Gecko. Modoc, NCR, Broken Hills: the party stayed in Arroyo after the jump there (a harness/sequence limit, not an RPU error). Script errors there (`obj is NULL` in door/animation scripts) come from jumping into maps. |
| Loading the town saves (9 slots) | PASS 9/9 | PASS 9/9 | same map and the same script-error count per save in both builds |
| Stimpak on RPU (hub, PLAY) | PASS | PASS | |
| hd=1 vs hd=0 | PASS | n/a | with hd=1 the 8-bit composition is RPU's (HD never changes legacy pixels); RPU's art differs from the pack in 254 paths (section 3) |
| RPU's hi-res settings | see below | | |

### Hi-res settings (`f2_res.ini`, `ddraw.ini`, `fallout2.cfg`)

- CE imports `f2_res.ini` once, only while `fallout2.cfg` has no `[screen]
  resolution_x`: RPU's file gives `resolution 1024x768`, `windowed=0`, no
  scale (`SCALE_2X=0`), `f2_res_dat=mods\f2_res.dat`, `iface_bar_width=800`,
  side art 2.
- HD needs `[screen] scale=2` and at least 1280x960 (logical 640x480;
  otherwise `_GNW95_init_mode_ex` falls back to scale 1 and `hd=1` has no
  2x framebuffer). With RPU's values HD is effectively off. An installer
  must write `[screen]` (scale 2, the display size) and, because that stops
  the import, carry `f2_res_dat=mods\f2_res.dat` over itself (Narg Edition's
  installer does both).
- `ddraw.ini` is read by CE for its sfall settings (version string, extra
  save slots, `IniConfigFolder=mods`); RPU's `[Main] HiResMode=0` etc. do not
  matter to CE. Our `[graphics]` keys are only in `fallout2.cfg`; RPU's
  vanilla-style `fallout2.cfg` has none, so HD is off until they are added.
- RPU's `mods/f2_res.dat` (BGforge f2_res) differs from CE's
  `files/f2_res.dat`, among the art our packs cover, only in `HR_MAINMENU.FRM`; the 800-px interface bar
  comes from `ce.dat` either way (it is above f2_res in the chain).

## 3. HD art vs RPU's art

`tools/hd/legacy_art.py compare` resolves every art path of the packs
through the engine's chain in both games (vanilla: master < critter <
f2_res < ce.dat < patch000 < data/; RPU: master < critter < mods\f2_res <
ce.dat < mods in `mods_order.txt` < data/) and compares the frames.
Full list: `docs/screens/rpu_compat/rpu_art_overrides.md`.

Paths of the packs (7270 assets) that RPU resolves from another file:

| type | identical | same pixels | **changed** |
|---|---|---|---|
| intrface | 12 | 0 | **31**: world map tiles `wrldmp00-19`, `wrldspr0-2`, `wmtabs`, `wm_tribe` (enhanced world map), `alltlk` (dialogue frame), elevator panels `el_*`, `hr_mainmenu` |
| inven | 0 | 0 | **7**: flask, gizgun1, hrifle, hrifle2, phazer, sniper, zipgun |
| items | 0 | 1 | **1**: flare |
| skilldex | 0 | 0 | **7** cards (addict, drugrest, firstaid, ...) |
| scenery | 1 | 53 | **40**: cacti, rocks, signs, computers, force fields, steam, vents, ... |
| walls | 0 | 29 | **2**: shutl01/02 |
| tiles | 1 | 0 | **166**: `shut001-094` (vanilla has 42x18 placeholders, RPU real 80x36 tiles), `vrof001-042`, `bld*`, `grass01/02` |

So **254 art files** would have been drawn as the pack's picture of the
original over RPU's art (93 of them resized, so the old picture was also
stretched). In addition RPU **adds 1876 art files** of the HD types (971
tiles, 472 scenery, 357 walls, 39 inventory, 24 items, 11 interface, 2
skilldex): these have no HD image and are drawn as the legacy frame doubled
(lit in RGB like all HD fallbacks), next to HD neighbours.

### Fix (implemented)

- **Pipeline:** `tools/hd/legacy_art.py hashes <pack>...` writes
  `hd/<pack>/legacy.txt`: for every image (and every `.fhd` frame) the
  legacy art path, direction, frame and an FNV-1a 64 hash of width, height
  and palette indices of the frame it was made from (8231 frames per pack).
  Ground variants are covered by their tile's entry.
- **Engine** (`src/hd_art.cc`, `src/hd_format.{h,cc}`): `hdArtFindFrame`
  checks once per asset frame (cached) that the running game's frame, loaded
  through the same file chain (localized art first, as the art cache does),
  has the recorded hash; if not, the lookup returns no image, so every
  caller falls back to the legacy frame doubled (or, for screens, to plain
  legacy). Packs without `legacy.txt` and unlisted frames are not checked.
  `[graphics] hd_verify=1` (default) switches it; `hdstats` reports
  `legacy_checked` / `legacy_rejected`; a `debugPrint` names each rejected
  frame.
- **Tests:** unit `hd_format_test` (hash vectors shared with the Python
  tool, pitch, parser); integration `test_hd_legacy_verify` (pack
  `testlegacy` from `make_test_hd_art.py`: a matching source is used, a
  changed one rejected, `hd_verify=0` disables).

Results (hidden runs, 2560x1440, hd=1 mixed):

| Run | checked | rejected |
|---|---|---|
| vanilla: menu / new game / world map | 4 / 558 / 571 | 0 / 0 / 0 |
| RPU: menu / new game / world map | 7 / 608 / 621 | 1 / 12 / 19 |

The rejected frames are exactly the changed art the comparison predicted
(`hr_mainmenu`, `cac1000`/`cac2000` directions, `roks*`, `wrldmp*`,
`wrldspr*`, `wmtabs`). Screens: `docs/screens/rpu_compat/menu_off_on.png`
(left `hd_verify=0`: our CE palm menu over RPU's; right: RPU's own menu with
HD buttons), `worldmap_off_on.png`, `towns_rpu_hd.png`.

Cost: one extra file read and hash per asset on first use (cached); no
change on vanilla pictures.

### Possible next steps

- HD images for RPU's 254 changed and 1876 added files: run the pipeline
  (`extract.py` with the RPU game as `FALLOUT2_DIR`) and ship them as a
  separate pack/dat for RPU; `legacy.txt` then selects per frame.
- CE: implement `fs_seek`/`fs_read_short`/`fs_write_short` and
  `set_fake_perk` (upstream CE work).
