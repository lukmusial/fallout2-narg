## Development

Layout (after the Restoration Project's):

| Path | What |
|---|---|
| `engine/` | the engine: Fallout 2 CE (from c30f1efc, upstream history kept) with HD; Stimpak is a submodule at `engine/external/stimpak` ([fallout2-stimpak](https://github.com/lukmusial/fallout2-stimpak)) |
| `release/` | files that go into the archive as they are: installers, `narg.cfg`, `rpu-save-convert.py` |
| `components/` | optional parts: `settings/*.cfg` (installer `--with`), `hd_packs/` (HD art packs, local builds only) |
| `docs/` | documentation, copied into `narg/docs/` |
| `extra/package.sh` | packs `narg_v<VERSION>_<platform>.zip` |
| `.github/workflows/build.yml` | builds the engine on macOS, Linux and Windows and packs the archives |

### Building

```sh
git clone --recurse-submodules https://github.com/lukmusial/fallout2-narg.git
cd fallout2-narg
cmake -S engine -B engine/out/build/macos-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build engine/out/build/macos-release --target fallout2-ce ce-dat-tool
```

### Tests

The engine's tests (`engine/tools/dev/run-tests.sh`) compare against golden screenshots in `engine/testdata/golden/` and load reference saves from `engine/testdata/SAVEGAME/`. Both show the game's art and are not in this repository, nor are the screenshots under `engine/docs/screens/` that the engine docs mention, nor the upscale unit test's fixtures (`engine/tools/hd/make_upscale_fixtures.py` makes them; without them `hd_upscale_test` skips). Make your own from your copy of the game before running those tests.

### HD art packs (optional)

The releases have none: the engine upscales at run time (`engine/src/hd_upscale.cc`, `hd_scene.cc`; `engine/tools/hd/README.md`, "No packs"). Packs are made locally by `engine/tools/hd` from your own game (see `engine/tools/hd/README.md`): extract, enhance (scripted and AI), `install.py`, `ground_variants.py`, `palm_repaint.py`. Then record the legacy frames each image was made from:

```sh
engine/tools/hd/legacy_art.py hashes <run dir>/data/hd/ai <run dir>/data/hd/scripted
```

### Packing

```sh
extra/package.sh macos engine/out/build/macos-release
```

With a third argument, `<run dir>/data/hd`, the archive also gets your HD art packs (for your own use only, see [HD art packs](../components/hd_packs/README.md)).

### Checking a content mod

```sh
engine/tools/hd/legacy_art.py compare <run dir>/data/hd/ai --base <vanilla game> --mod <modded game> --md report.md
```

lists the art the mod replaces (where the HD art would be stale; the engine then draws the mod's art) and adds.

### Versioning

`VERSION` holds the release version (semantic versioning); `docs/changelog.md` has a section per version. Tag `v<VERSION>` to make the workflow attach the archives to a release.
