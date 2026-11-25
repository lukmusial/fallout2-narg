## Tests of a release

### 1.1.0

`narg_v1.1.0_macos.zip` (sha256 `8b90552f…53a4`) and the RPU add-on
`narg_v1.1.0_rpu.zip` (sha256 `57e233b0…5fd0`), engine `e5f7a29f`, both
extracted into fresh copies of vanilla Fallout 2 and of Fallout 2 with the
Restoration Project v2.4.34, installed with `narg-install.sh` and run hidden
as for 1.0.0 (scripts `tests/compat/edition.txt`, `hd_rpu_pack.txt`).

| Check | Vanilla | RPU |
|---|---|---|
| `mods/mods_order.txt` after install and re-install | `narg_hd.dat` (add-on present but not loaded) | `narg_hd.dat`, `narg_hd_rpu.dat`, once each |
| HD art frames checked / rejected (new game) | 533 / 0 | 575 / 0 |
| pack of 22 probed frames (RPU's changed and new art, original art) | `ai` / `scripted`; RPU's own art not found | `rpu` for all of RPU's art, `ai` for the original |
| heap errors in `debug.log` | none | none |

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
