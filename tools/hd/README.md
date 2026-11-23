# HD art pipeline (Phase 2.5)

Two independent enhancers turn original 8-bit FRM art into true-colour RGBA
PNGs at exactly 2x the original pixel size, for the engine's HD layer:

- `assets/hd/scripted/art/<type>/<name>.png`: deterministic, numpy only
- `assets/hd/ai/art/<type>/<name>.png`: Real-ESRGAN x2plus on PyTorch/MPS
- `assets/hd/masks/art/<type>/<name>.png`: shared 2x mask (255 = HD pixel,
  128 = colour-cycling index 229-254, 0 = transparent index 0)

Index 0 and the colour-cycling indices are alpha 0 in both packs, so the
legacy palette animation (slime, shoreline, monitors, fire) shows through.
Alpha is a crisp nearest-2x of the source silhouette, so it matches the
legacy sprite exactly. Floor tiles overlap their neighbours, so their diamond
edges must not move.

`assets/hd/`, `tools/hd/work/` (extracted art), `tools/hd/models/` (weights) and
`tools/hd/.venv/` are gitignored. Only tools, tests, docs and the comparison
sheets in `docs/screens/phase2.5/` are committed.

## Run

```sh
cmake -S . -B out/build/macos -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build out/build/macos --target ce-dat-tool ce-frm2png

uv venv --python /opt/homebrew/bin/python3.15 tools/hd/.venv   # or python3.15 -m venv
VIRTUAL_ENV=tools/hd/.venv uv pip install -r tools/hd/requirements.txt
# model weights: see MODELS.md

cd tools/hd
.venv/bin/python extract.py            # master.dat < critter.dat < patch000.dat < data/
.venv/bin/python enhance_scripted.py   # all assets (or: enhance_scripted.py mainmenu edg5000)
.venv/bin/python enhance_ai.py         # --model x2plus|x4plus|anime6b, --detail, --tile-detail
.venv/bin/python contact_sheet.py      # -> docs/screens/phase2.5/
.venv/bin/python tilemap.py            # seam metrics per map patch and pack
.venv/bin/python test_pipeline.py -v
```

Interface screens (Phase 2.6) and installing the packs for the game:

```sh
cd tools/hd
.venv/bin/python extract.py            # also SCREENS (f2_res.dat HR_* art) and ce.dat PNGs
NAMES=$(.venv/bin/python -c "import json;print(' '.join(a['key'] for a in json.load(open('work/manifest.json'))['assets'] if a['type']=='intrface'))")
.venv/bin/python enhance_scripted.py $NAMES
.venv/bin/python enhance_ai.py $NAMES
.venv/bin/python install.py            # -> out/build/macos/data/hd/{scripted,ai}/
.venv/bin/python screens_sheet.py      # runs the game hidden -> docs/screens/phase2.6/
```

Select a pack with `[graphics] hd=1` and `[graphics] hd_pack=scripted` (or
`ai`, or a mixed pack with a `manifest.txt`, see `src/hd_art.h`).

`extract.py --extra art/scenery/car2.frm` adds assets. The game folder
defaults to `~/Games/Fallout 2`; override it with `FALLOUT2_DIR`. Multi-frame
FRMs use frame 0, direction 0 for now (critters come in Phase 2.9).

## What each enhancer does

**Scripted** (`enhance_scripted.py`):
1. Palette to RGB. Transparent and cycling pixels are holes, filled from
   their neighbours so black or placeholder colours don't bleed in.
2. Palette-aware de-dither: a 3x3 bilateral filter. Its range sigma at each
   pixel is the distance from that pixel's palette colour to the nearest other
   palette colour, so dither partners merge and real edges stay.
3. Edge-directed 2x upscale with directional cubic convolution (DCCI). A
   diagonal pass runs, then an axial pass. Edge directions are measured on a
   fully de-dithered guide image with a noise floor, so leftover dither does
   not turn into diagonal "worms".
4. Gradient restoration at 2x: range-limited smoothing of about one palette
   step turns quantisation bands into ramps, followed by an edge-aware unsharp
   mask.
5. 45% of the removed dither and texture is added back as fine grain through
   a bilinear 2x.

**AI** (`enhance_ai.py`):
1. Hole-filled RGB goes into RealESRGAN_x2plus (RRDBNet, native 2x), tiled
   with overlap.
2. Colour-drift correction: the output is box-downsampled to 1x, and the
   low-pass of (source minus output) is added back at 2x.
3. 30% of the high-pass difference is re-added, or 60% for tiles. Without
   this the model erases fine regular textures, such as the 1-px stripes on
   Den floors, and treats neighbouring tiles inconsistently.

**Tiles** (both enhancers): before processing, each tile is surrounded by
copies of itself on the isometric floor lattice (+48,-12 per column, +32,+24
per row, as in `squareTileToScreenXY`). The result is cropped back.

## Interface screens in the engine (Phase 2.6)

Main menu (vanilla `mainmenu_sp.png`, hi-res `HR_MAINMENU.FRM` +
`hr_menu_bg_sp.png`, `menuup`/`menudown`), load/save screen, character
editor (with its dialogs and the perk dialog) and the Stimpack hub draw the
pack's HD image right after each legacy blit of the same art
(`hdArtDrawImage`, `hdArtDrawImageRegion`, button HD art via
`buttonSetHdArt` / `hdArtAttachButtonArt`). An HD pixel is only drawn where
the window buffer still equals the legacy art, so text and numbers drawn into
or over the art (button labels, stats) stay legacy whatever the draw order.
The Stimpack editor's background is the same composite in HD as in 8 bits:
`edtrcrte` with the level box of `edtredt`.

Named PNG art from `ce.dat` (`mainmenu_sp.png`, `opbase_sp.png`, ...) is
looked up like FRM art: `hd/<pack>/art/intrface/mainmenu_sp.png`.

Observations (see `docs/screens/phase2.6/`):

- AI keeps the painted textures (power armour, leather, metal plates, the
  parchment) and is the better choice for backgrounds and panels.
- Very small glyph-like buttons are where AI fails: the 11x14 scroll arrows
  (`uparwoff`, `dnarwoff`, ...) come out as blobs. Scripted keeps them. A
  mixed pack (`* = ai`, the arrow FRMs `= scripted`) fixes that.
- Scripted is faithful but soft; on the 3440x1440 menu its plates look
  smeared next to AI.

## Timings (Apple M5 Pro, 64 GB; per asset, warm)

| Asset | Scripted | AI (x2plus, MPS) |
|---|---|---|
| 640x480 interface screen | 1.2 s | 1.5-1.9 s |
| 26x26 button | 0.007 s | 0.05-0.13 s |
| 80x36 floor/roof tile (padded) | 0.022 s | 0.07 s (max 0.14) |
| Wall or scenery (up to 221x111) | 0.015-0.11 s | 0.05-0.22 s |
| Whole sample set (72 assets) | 6.4 s | 11.6 s + 0.2 s model load |

x4plus takes about 1.6x as long as x2plus. anime6b is the fastest but
flattens the art.

## Seam quality (`tilemap.py`, map patches)

Each 6x6 floor patch is taken from a real map grid: `arvillag`, `kladwtwn`,
`arcaves` and `denbus1`, elevation 0. The tiles are enhanced one by one, then
assembled at 2x. The result is compared with the same enhancer run on the
whole 1x patch at once, where every tile has its true neighbours.

- `seam_excess`: extra mean absolute error (0-255) on 2-px seam bands,
  compared with tile interiors.
- `grad_excess`: cross-seam gradient ratio, divided by the reference's ratio.

| Pack | seam_excess | grad_excess | Test limit |
|---|---|---|---|
| scripted | 1.1-4.4 | 1.03-1.07 | < 6.0 / < 1.12 |
| ai | 0.5-2.5 | 1.10-1.23 | < 4.0 / < 1.35 |

## Observations so far

- **Interface screens.** AI looks more natural: metal, leather and the power
  armour keep a believable texture, and numbers and lettering are crisp.
  Scripted is clean and faithful but painterly: flat areas get soft
  blotches and gradients look a bit posterised. Neither enhancer invents
  wrong shapes.
- **Floor tiles.** Closer. Scripted is perfectly consistent from tile to
  tile, so it adds no visible seams, but it looks smeared. AI gives nicer
  stone, brick and cave detail. It is less consistent across neighbouring
  tiles, though: some diamonds come out smoother than others, and very fine
  regular stripes get partly erased. The detail re-injection reduces both
  problems without removing them.
- **Sprites with dithered shadows** (car1). The shadow is a checkerboard of
  opaque and transparent pixels. Both packs keep it as isolated 2x2 dots,
  because alpha is exact by design. A true translucent shadow would need an
  engine change or alpha-aware handling for shadows.
- **AI is not bit-reproducible** (MPS float32). The scripted pack is
  bit-exact, which the tests check.
