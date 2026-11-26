## Tests of a release

### 1.0.0

`narg_v1.0.0_macos.zip` (7.7 MB; sha256 `f97c5937…b5d9c`), Release
engine, extracted into a fresh copy of vanilla Fallout 2 and into a copy of
Fallout 2 with the Restoration Project v2.4.34 (saves converted with
`rpu-save-convert.py`), installed with `narg-install.sh --resolution 1920x1080`
and run hidden with the configuration as installed (`tools/dev/compat_run.py
--app "Fallout 2 Narg Edition.app"`): `tests/compat/edition.txt` (main menu,
new game) and a save (slot 3) loaded, then the Den (`gotomap denbus1`, empty
disk cache) and the world map.

| Check | Vanilla | RPU |
|---|---|---|
| `mods/` after install | untouched (none) | untouched (RPU's load order kept) |
| main menu, new game, save loaded, Den, world map | PASS | PASS |
| pictures upscaled while drawing after a map load | 0 | 0 |
| Den map load with preparation, empty cache | 786 ms | 903 ms |
| `data/hd/cache` after the run | 464 MB | 499 MB |
| heap errors in `debug.log` | none | none |
| other errors in `debug.log` | none | RPU's known ones (opcode 8202, city count of an old save) |

Engine tests: 5/5 unit (upscale fixtures made from the local game), 98/98
HD, 12/12 Stimpak.
