## Development

Layout (after the Restoration Project's):

| Path | What |
|---|---|
| `engine/` | the engine: Fallout 2 CE (from c30f1efc, upstream history kept) with HD; Stimpak is a submodule at `engine/external/stimpak` ([fallout2-stimpak](https://github.com/lukmusial/fallout2-stimpak)) |
| `release/` | files that go into the archive as they are: installers, `narg.cfg`, `rpu-save-convert.py` |
| `components/` | optional parts: `settings/*.cfg` (installer `--with`) |
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

### HD

The engine upscales the game's pictures at run time: `engine/src/hd_upscale.cc` (the upscale), `engine/src/hd_scene.cc` (scene preparation, loading screen), `engine/src/hd_art.cc` (lookup, caches); notes and timings in `engine/tools/hd/README.md`.

### Packing

```sh
extra/package.sh macos engine/out/build/macos-release
```

### Versioning

`VERSION` holds the release version (semantic versioning); `docs/changelog.md` has a section per version. Tag `v<VERSION>` to make the workflow attach the archives to a release.
