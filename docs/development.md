## Development

Layout (after the Restoration Project's):

| Path | What |
|---|---|
| `engine/` | the engine: Fallout 2 CE (from c30f1efc, upstream history kept) with HD; Stimpak is a submodule at `engine/external/stimpak` ([fallout2-stimpak](https://github.com/lukmusial/fallout2-stimpak)) |
| `release/` | files that go into the archive as they are: installers, `mods_order.txt`, `narg.cfg` |
| `components/` | optional parts: `settings/*.cfg` (installer `--with`), `hd_scripted/` (the `narg_hd_scripted.dat` pack) |
| `docs/` | documentation, copied into `narg/docs/` |
| `extra/package.sh` | packs `narg_v<VERSION>_<platform>.zip` |
| `.github/workflows/build.yml` | builds the engine on macOS, Linux and Windows and packs the archives |

### Building

```sh
git clone --recurse-submodules https://github.com/lukmusial/fallout2-nerg.git
cd fallout2-nerg
cmake -S engine -B engine/out/build/macos-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build engine/out/build/macos-release --target fallout2-ce ce-dat-tool
```

### Tests

The engine's tests (`engine/tools/dev/run-tests.sh`) compare against golden screenshots in `engine/testdata/golden/` and load reference saves from `engine/testdata/SAVEGAME/`. Both show the game's art and are not in this repository, nor are the screenshots under `engine/docs/screens/` that the engine docs mention. Make your own goldens and saves from your copy of the game before running the screenshot tests.

### HD art

The packs are made locally by `engine/tools/hd` from your own game (see `engine/tools/hd/README.md`): extract, enhance (scripted and AI), `install.py`, `ground_variants.py`, `palm_repaint.py`. Then record the legacy frames each image was made from:

```sh
engine/tools/hd/legacy_art.py hashes <run dir>/data/hd/ai <run dir>/data/hd/scripted
```

### Packing

```sh
extra/package.sh macos engine/out/build/macos-release <run dir>/data/hd
```

### Checking a content mod

```sh
engine/tools/hd/legacy_art.py compare <run dir>/data/hd/ai --base <vanilla game> --mod <modded game> --md report.md
```

lists the art the mod replaces (where the HD art would be stale; the engine then draws the mod's art) and adds.

### Versioning

`VERSION` holds the release version (semantic versioning); `docs/changelog.md` has a section per version. Tag `v<VERSION>` to make the workflow attach the archives to a release.
