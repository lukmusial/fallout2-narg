## Tests of a release

### 1.1.0

`narg_v1.1.0_macos.zip` (7.7 MB, no art; sha256 `7638a9ac…ed3f0`), Release
engine, extracted into a fresh copy of vanilla Fallout 2 and into a copy of
Fallout 2 with the Restoration Project v2.4.34 that had the art packs of an
earlier build installed (saves converted with `rpu-save-convert.py`), installed
with `narg-install.sh --resolution 1920x1080` and run hidden with the
configuration as installed (`tools/dev/compat_run.py --app "Fallout 2 Narg
Edition.app"`): `tests/compat/edition.txt` (main menu, new game) and a save
(slot 3) loaded, then the Den (`gotomap denbus1`, cold disk cache) and the
world map.

| Check | Vanilla | RPU |
|---|---|---|
| `narg_hd*.dat` in `mods/mods_order.txt` after install | none | removed (RPU's 24 entries kept) |
| `hd_pack`, disk cache | empty (engine upscale), on | empty, on |
| main menu, new game, save loaded, Den, world map | PASS | PASS |
| pictures upscaled while drawing after a map load | 0 | 0 |
| Den map load with preparation, cold cache | 776 ms | 1014 ms |
| `data/hd/cache` after the run | 469 MB | 499 MB |
| errors in `debug.log` other than scripts | none | RPU's known ones (opcode 8202, city count of an old save) |

Engine tests: 5/5 unit (upscale fixtures made from the local game), 98/98
HD, 12/12 Stimpak.

### 1.0.0

Release 1.0.0 (`narg_v1.0.0_macos.zip`, sha256 `e5befaef…1e065`, engine
`9fead64a`) was installed with `narg-install.command` (run with the stock
macOS tools) into two copies of a local Fallout 2: a vanilla one (with saves)
and one with the Restoration Project v2.4.34. Each was then run hidden
(`--dev-script`, engine repository `tools/dev/compat_run.py --app "Fallout 2
Narg Edition.app"`, scripts `tests/compat/`) with the configuration exactly
as installed. No `data/hd` folder: all HD art came from `mods/narg_hd.dat`.

| Check | Vanilla | RPU |
|---|---|---|
| install, re-install over it (backups kept, no duplicate `mods_order.txt` entry) | PASS | PASS (RPU's `mods\f2_res.dat` kept) |
| main menu in HD | PASS | PASS (RPU's own menu picture, HD buttons) |
| new game | PASS | PASS (temple) |
| HD art frames checked / rejected (new game) | 533 / 0 | 568 / 10 (art RPU replaced) |
| Stimpak hub, PLAY | PASS | PASS |
| a town in HD (`gotomap`) | PASS (New Reno) | PASS (Abbey, New Reno) |

Screens: `docs/screens/edition_installed.png` (top vanilla, bottom RPU: main
menu, Stimpak, town).
