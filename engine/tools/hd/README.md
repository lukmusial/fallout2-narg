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

Floors and roofs (Phase 2.7): every floor and roof tile of the maps of the
save games' areas (Arroyo `ar*`, Klamath `kla*`, Den `den*`; 941 tiles):

```sh
cd tools/hd
.venv/bin/python extract.py --areas     # per-map counts, adds the tiles to the manifest
NAMES=$(.venv/bin/python -c "import json;print(' '.join(a['key'] for a in json.load(open('work/manifest.json'))['assets'] if a['type']=='tiles'))")
.venv/bin/python enhance_scripted.py $NAMES   # ~18 s
.venv/bin/python enhance_ai.py $NAMES         # ~32 s
.venv/bin/python install.py
.venv/bin/python floors_sheet.py        # runs the game hidden -> docs/screens/phase2.7/
```

Walls, scenery and items (Phase 2.8): every wall, scenery and item art
placed on those maps and on the reference saves' copies of them (1261 FRMs:
685 walls, 503 scenery, 73 items; 52 of them multi-frame, 338 frames).
`mapobjects.py` reads the maps' object lists the way the engine does
(scripts, objects, per-subtype data from the protos). Multi-frame art
(doors, fire pits, containers) is extracted per frame and direction
(`png/art/<type>/<name>/d<D>_f<F>.png`) and `install.py` packs the enhanced
frames into `art/<type>/<name>.fhd`:

```sh
cd tools/hd
.venv/bin/python extract.py --areas --objects
.venv/bin/python enhance_scripted.py    # everything: ~1 min
.venv/bin/python enhance_ai.py          # everything: ~3 min
.venv/bin/python install.py
.venv/bin/python objects_sheet.py       # runs the game hidden -> docs/screens/phase2.8/
.venv/bin/python pack_review.py --sheet ../../docs/screens/phase2.8/mixed_candidates.png
```

Select a pack with `[graphics] hd=1` and `[graphics] hd_pack=scripted` (or
`ai`, or a mixed pack with a `manifest.txt`, see `src/hd_art.h`).

`extract.py --extra art/scenery/car2.frm` adds assets. The game folder
defaults to `~/Games/Fallout 2`; override it with `FALLOUT2_DIR`. Critters
come in Phase 2.9.

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
editor (with its dialogs and the perk dialog) and the Stimpak hub draw the
pack's HD image right after each legacy blit of the same art
(`hdArtDrawImage`, `hdArtDrawImageRegion`, button HD art via
`buttonSetHdArt` / `hdArtAttachButtonArt`). An HD pixel is only drawn where
the window buffer still equals the legacy art, so text and numbers drawn into
or over the art (button labels, stats) stay legacy whatever the draw order.
The Stimpak editor's background is the same composite in HD as in 8 bits:
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

## Floors and roofs in the engine (Phase 2.7)

With `hd=1` and a pack, `src/tile.cc` draws each floor and roof tile into the
iso window's HD layer right after the legacy draw (`hdLayerDrawLit`): the
pack image, or the legacy tile expanded 2x when the pack has none, lit in RGB
from the same intensities the legacy draw uses (per pixel for floors with
varying light, the ambient level for roofs). The legacy tables quantise light
to 128 steps and then to the nearest palette colour; in RGB the light falls
off smoothly. Roofs are drawn in HD only where the buffer holds the plain lit
roof colour, so the blended cut-out around the player stays legacy.

Objects (scenery, walls, critters, items) stay legacy this phase: every object
draw marks its non-transparent pixels legacy in the layer, so they win even
where an object pixel has the same palette index as the floor under it.
Scrolling moves the HD layer with the window buffer (`hdLayerScroll` next to
the memmove in `mapScroll`); every redraw of a rectangle, the hi-res stencil
and the black screen of a map change reset it there first.

Observations (see `docs/screens/phase2.7/`):

- AI gives cleaner ground (desert, dirt, concrete) and much better roofs
  (Klamath straw, Arroyo tents); the scripted enhancer turns the straw and
  tent fabric into "worm" patterns.
- Seams between tiles are not visible with either pack at 1:1.
- Smooth lighting is the biggest visible change in dark maps (toxic caves,
  rat caves, the Den at night): the legacy darkening maps to the palette's few
  dark entries, which are reddish and noisy; RGB darkening keeps the tile's
  own hue (bluish-grey brick in the toxic caves). Until objects get RGB
  lighting (Phase 2.8) the legacy-lit walls and scenery keep the reddish cast
  next to the neutral floors.

Performance (Release, 3440x1440 at scale 2, `run_tests.py --perf N` with
`RUN_DIR=out/build/macos-release`; slots 1 Klamath day, 8 Den at night with
lights everywhere, 6 rat caves; machine under other load):

| | legacy (hd=0) | hd=1 scripted | hd=1 ai |
|---|---|---|---|
| idle, work per frame | 0.7-1.3 ms | 1.1-2.1 ms | 1.1-2.1 ms |
| scroll step (32x24 px) | 0.3-0.5 ms | 2.6-5.0 ms (max 7.7) | 2.4-4.8 ms (max 7.6) |
| whole-map redraw, median | 1.3-1.9 ms | 5.1-8.8 ms | 5.1-9.5 ms |
| of that, HD tile drawing | - | 2.3-5.3 ms | 2.3-5.6 ms |

Whole-map redraws happen on map and elevation changes and when combat starts;
ordinary frames redraw only the rectangles that changed.

## Walls, scenery and items in the engine (Phase 2.8)

`_obj_render_object` (`src/object.cc`) draws walls, scenery and items into
the iso window's HD layer right after their legacy draw: the pack image (or
the legacy frame expanded 2x), lit in RGB from the same intensity as the
legacy draw, so walls in dark places now get the same neutral colour as the
2.7 floors instead of the palette's reddish dark entries. Critters, misc and
interface objects stay legacy (critters: Phase 2.9).

- Translucent objects (glass, steam, energy, red, wall), the cut-out
  ("egg") in walls and roofs around the player and paletted outlines are
  blended in RGB: the layer's colours under the draw are captured before the
  legacy blend (`hdLayerCapture`), then `hdLayerDrawBlended` /
  `hdLayerTintChanged` apply the legacy formula (grey level of the object
  -> share of the tint; mask / 128 -> share of the wall) without the
  palette's quantisation. The cut-out therefore shows the HD floor and
  scenery behind the wall. Solid and colour-cycling outlines stay legacy.
- One-direction art is looked up as direction 0; multi-frame art comes from
  the `.fhd` container per frame and direction.
- `hdcheck` counts wall/scenery/item pixels and their HD share, critter and
  other object pixels (must stay legacy) and cut-out draws.

Pipeline fix found on the way: `hdlib.fill_holes` padded the colour sums with
"edge" but the counts with zeros, so holes next to opaque pixels on the image
border were filled with values that grew without bound (up to ~900000).
Sprites whose art touches the frame edge came out of the AI as colour noise.
It also touched 150 floor/roof tiles (the padded canvas edge) and 5 interface
images; both packs were regenerated (AI tiles changed by 0.6 levels on
average, up to 15 on some `arrf*`/`jrt*` roof edges; scripted tiles did not
change).

Observations (see `docs/screens/phase2.8/`):

- AI keeps stone, wood, fabric and metal textures (Arroyo statue and tents,
  Klamath brick, mine carts) where scripted smears them; for most objects AI
  is the better pack.
- Where AI does poorly (`pack_review.py`, sheet `mixed_candidates.png`):
  the dithered scrap-metal walls `jds*`/`jdd*` get invented vertical streaks
  (the failure seen on the corrugated roofs), the craps tables `crptbl*`
  and the `beer` item drift in colour (blue/white outlines that are not in
  the art). These are proposed for the mixed manifest, not applied.
- Chain-link fences score worst on the edge metric but look the same as
  scripted in the game; no change proposed.

Performance (Release, 3440x1440 at scale 2, `run_tests.py --perf N`; slots 1
Klamath day, 8 Den at night, 6 rat caves):

| | legacy (hd=0) | hd=1 scripted | hd=1 ai | hd=1 mixed |
|---|---|---|---|---|
| idle, work per frame | 1.1-2.3 ms | 1.1-1.4 ms | 1.0-1.2 ms | 1.2-2.1 ms |
| scroll step | 0.3-1.7 ms | 2.7-4.0 ms (max 5.1) | 2.9-4.9 ms (max 9.4) | 2.9-4.4 ms (max 6.5) |
| whole-map redraw, median | 1.0-2.9 ms | 8.3-12.3 ms | 7.8-9.9 ms | 8.1-11.2 ms |
| of that, HD tiles / objects | - | 3.1-6.5 / 1.3-2.5 ms | 2.9-4.9 / 1.2-2.1 ms | 2.9-4.9 / 1.2-2.4 ms |

## Inventory (Phase 2.10)

Every inventory item image (`art/inven`, all 366 FRMs) and the art of the
inventory, loot, use-item-on, barter/trade (also the expanded `barter_e`/
`trade_e`, `invbox2.png`, `loot2.png`, `partyslot.png` from `ce.dat`) and
move-items windows, their scroll/take-all buttons and the interface bar's
item button (`sattkbup`, `sattkbdn`, `saturbup`): 395 images.

```sh
cd tools/hd
.venv/bin/python extract.py --areas --objects --inven
NAMES=$(.venv/bin/python -c "
import json, extract as e
w = {p.rsplit('.', 1)[0] + '.png' for p in e.INVENTORY_SCREENS + e.CE_DAT_FRM_INVEN + e.CE_DAT_PNG_INVEN}
print(' '.join(a['key'] for a in json.load(open('work/manifest.json'))['assets'] if a['type'] == 'inven' or a['key'] in w))")
.venv/bin/python enhance_scripted.py $NAMES   # 395 images: 12.6 s
.venv/bin/python enhance_ai.py $NAMES         # 30.4 s
.venv/bin/python install.py
.venv/bin/python inventory_sheet.py     # runs the game hidden -> docs/screens/phase2.10_inventory/
.venv/bin/python pack_review.py --inven # work/pack_review_inven.json
```

In the engine:

- Items: `hdArtRender` (`src/hd_art.h`) replaces `artRender` in the
  inventory, loot, trade tables, party slots and move-items window. The
  legacy item is drawn as before (nearest-scaled when it does not fit its
  slot); the pack image is resampled (bilinear) to the drawn rectangle and
  blended over what the layer showed there, captured before the legacy draw
  (`hdLayerDrawOver`), so the HD silhouette replaces the legacy one.
  Quantity labels drawn over items stay legacy.
- Window backgrounds: HD after each full and partial background restore
  (`inventoryHdRestore`, which first drops whatever the layer held there).
  The trade window's panes are cut from the dialogue's barter window, so
  their HD pixels are copied along (`windowHdCopyRect`); the barter/trade
  panel gets its HD art once it has slid in.
- Buttons made from art ids (`buttonCreateWithFrm`, `buttonSetDisabledFrm`)
  get HD art automatically, including the disabled images.
- Interface bar: the item button's HD images are composed from the pack's
  button art and the item (`interfaceBarUpdateItemButtonHd`, darkened and
  shifted on the pressed image like the legacy overlay); the action text
  and AP numbers stay legacy.
- Not HD: the dragged item (it is the mouse cursor), the "ALL" button of the
  move-items window (its label is drawn into the cached art) and critters
  in the body views (Phase 2.9 critters).
- Harness: `loot [n]` opens the loot window on party member n as the steal
  skill does; `talk [n]` starts a dialogue (the trade window is `b`).

Observations (see `docs/screens/phase2.10_inventory/`):

- AI is the better pack for items: metal, wood, glass, leather and labels
  stay crisp, where scripted softens them; the window backgrounds look like
  2.6's screens (AI keeps the riveted metal).
- Where AI does poorly (`mixed_candidates.png`): the small scroll arrows
  (`invup*`, `invdn*`, `di_up*`, `di_down*`, `di_upds.png`, `di_dnds.png`)
  get blue streaks or smeared heads, the failure seen on the 2.6 arrows; the
  super tool kit (`suprtool`, colour drift 14.6 vs 7.6) turns its speckled
  red paint into wood grain. These are proposed for the mixed manifest, not
  applied. `brain1` (AI deepens the grooves) and `eastregg` (edge metric
  flagged, bands look the same) were checked and are not proposed.

## Other static screens (Phase 2.14)

Every other interface image (`art/intrface`: master.dat, patch000.dat's help
screen, f2_res.dat's `HR_*`, ce.dat's `actmenu`, `hr_iface_800`,
`iface_apbar_e`, `opbase_help.png`; 445 FRMs/PNGs) and all 173 skilldex card
illustrations (`art/skilldex`). The endgame slides (`eg_*`) and the intro and
ending sequences (`seq*`, `arroyo01`, `dp`) are left out. The death scene and
the help screen have their own palettes (`death.pal`, `helpscrn.pal`) and are
extracted through them; multi-frame art (world map dial `wmdial`, car
`wmcarmve`, `endanim`, `wait`, `wait2`) becomes `.fhd` like objects. 594 new
assets (24.3 Mpx): scripted 9 min, AI 9 min (machine shared with other work).

```sh
cd tools/hd
.venv/bin/python extract.py --areas --objects --inven --interface
.venv/bin/python enhance_scripted.py $(cat work/keys_2.14.txt)   # the new keys
.venv/bin/python enhance_ai.py $(cat work/keys_2.14.txt)
.venv/bin/python install.py
.venv/bin/python static_sheet.py        # runs the game hidden -> docs/screens/phase2.14/
```

In the engine (HD drawn right after each legacy blit, as in Phase 2.6;
`hdArtDrawImageOffset` takes a blit's own offsets, `hdArtDrawFrameRegion` art
the code blits from cached frame data):

- World map (`worldmap.cc`): frame, terrain tiles (`wrldmp*`, drawn per
  visible part of each 350x300 tile, so scrolling stays correct), town
  circles (blended over the HD terrain in RGB with the legacy formula, tint
  green), markers, tabs and town labels, day/night dial, car, globe, date,
  buttons. Fog: unknown land stays black (HD cleared); known but unvisited
  land keeps its HD terrain darkened in RGB by the same factor as the legacy
  table (`intensityColorTable[..][75]`, `windowHdShadeRect`), so it looks like
  the legacy fog without its palette banding. Town maps (`twnmap*` etc.) and
  their entrance hotspots.
- Pip-Boy (background, its restores, logo, alarm and red buttons, date) and
  the automap (background, buttons). The map lines and dots stay legacy.
- Options, pause, preferences (background, switches, knobs and sliders
  where they move), help (`helpscrn.pal`).
- Character selector (background, the premade characters' portraits,
  buttons) and dialogue: the background (`alltlk`/`HR_ALLTLK`), the talk
  panel once it has slid in, the reply and option windows (HD copied with
  their legacy pixels), review window, party member control and custom
  windows. Talking heads stay legacy.
- Skilldex cards in the character editor and the perk dialog (with the
  background restores around them), the skilldex window (background, values,
  buttons), elevators (background, panel, gauge, buttons) and the death
  scene (scaled with it when `death_screen_size` stretches it).
- Not in HD yet: the interface bar background (`iface`, `hr_iface_800`) and
  its buttons other than the item button, the combat/action menus
  (`actmenu`, called-shot screen `called`), endgame slides and movies.

Mixed pack: AI for everything except the digit and month strips
(`numbers`, `months`, `bignum`): the AI smears the thin red and yellow
digits (`mixed_choice.png`). The other small art (markers, knobs, red
buttons, town labels) looks the same in both packs and stays AI.

Tests (`tests/run_tests.py`): `test_hd_static_ai` / `_mixed` open each screen
with hd=1 and compare the 8-bit composition with an hd=0 run (identical inside
the window) and require HD detail on the art; `test_worldmap_scroll` also
checks the terrain's HD detail and colour against the legacy frame after
every scroll step (fog included), the frame and the town map. Harness: `die`
kills the player (death scene).

## Interface bar (Phase 2.16)

The art was already in the packs (Phase 2.14 `--interface`). In the engine
(`src/interface.cc`):

- The bar's static background is the bar art (`iface`, or `HR_IFACE_800`
  when `iface_bar_width=800` fits the screen) with the extended AP bar art
  (`iface_apbar_e`) blitted over it. Its HD version is composed the same way
  once, with the legacy composite it stands for, and redrawn wherever legacy
  code restores part of the background (AP bar, alternate ammo meter).
- Buttons (INV, options, skilldex, MAP/CHA/PIP, change hands, end turn and
  end combat), the AP bulbs, the HP/AC digits, the end-turn panel's slide
  animation (`endanim`, per frame) and its green/red lights, and the hi-res
  side panels (`HR_IFACELFT*`/`HR_IFACERHT*`, the same part stretched the
  same way) are HD. The item slot was already HD (Phase 2.10); the message
  monitor's background is part of the bar art, its text stays legacy or HD
  text.
- The called-shot screen (`called`) background and its cancel button. The
  critter picture stays legacy (critter art is not in the packs).
- Not HD: the action menu (`actmenu`, `actpick` and the action icons; it is
  drawn as a map object through the mouse's 3D cursor, so it would need
  object-rendering changes), the indicator boxes (sneak, level, addiction;
  cached copies with labels drawn in) and the called-shot hit chances.

Mixed pack: the lettered bar buttons (`mapup`/`mapdn`, `chaup`/`chadn`,
`pipup`/`pipdn`, `invbutup`/`invbutdn`) come from scripted: the AI outlines
and breaks their letters. TURN/CMBT look the same in both and stay AI.

`test_hd_bar_ai` / `_mixed` (`tests/scripts/hd_bar.txt`, slot 1) check the
bar and combat mode at 640x480 logical and at 3440x1440 (800-px bar, side
panels): HD detail, monitor text kept, the bar's 8-bit composition identical
to an hd=0 run. `bar_sheet.py` writes `docs/screens/phase2.16_bar/`.
## Gunshot light and wind (Phase 2.15)

Two effects of the HD renderer, each with its own switch in
`files/fallout2.cfg` `[graphics]` (on in the example config, off in the test
run directory, nothing with `hd=0`). Neither changes the 8-bit buffers, so
hit-testing, saves and the legacy goldens are unaffected.

`hd_shot_light=1` (`src/hd_shot_light.{h,cc}`): every ranged attack lights
its surroundings for 150 ms (1 / 0.6 / 0.3 / 0.1 / 0 over ~33 ms steps)
through the HD light field (`hd_lights.h`), so floors, walls, scenery and
critters are lit alike, also with `hd_lights=0`. The ranged attack registers
itself (`actions.cc`), and the light follows the fire animation as drawn:

- When: the fire animations have their muzzle flashes painted in. Per frame
  of a direction, the "flash" pixels are bright warm pixels (luminance > 170,
  red > 200) that the quietest frame of that direction does not have at the
  same place (frames aligned by their offsets). Frames whose bright count
  peaks well above the quietest frame are shots: one per round of a burst
  (SMG frames 1, 3, 5; minigun every other frame), the action frame for
  single shots.
- Where: of the flash pixels within 36 px sideways of the critter's feet (a
  flamer's stream and a rocket's trail reach further), the cluster around
  the frontmost one along the facing; its near end is the muzzle. Without
  flash pixels: the frontmost sprite pixel at weapon height (45-75%). The
  feet are found through the art's per-direction offset, which wide frames
  (flamer) use to keep the feet on the hex.
- Light: the approved mock's cone, `(cos - 0.80) / 0.20` clipped `^1.2`,
  times `1 / (1 + (d / 140)^2)`, plus `exp(-d / 28)` spill at the muzzle and
  almost nothing behind (output pixels; vertical distance counts double), a
  flash streak and core at the muzzle drawn per output pixel
  (`HdLightField::flashes`). Colour by damage type: bullets warm yellow,
  laser red, plasma green, fire orange, electrical/EMP blue. Explosions
  (rockets, grenades, dynamite) give an omni flash for 450 ms at the impact.
  Strength scales with darkness (0.45 by day, 1 at night).

`hd_wind=1` (`src/hd_wind.{h,cc}`, the wind scene in `src/object.cc`):
trees (`tree*`, `tre1000`), bushes, corn, the plant-like weeds (`weed01-04`,
`10`, `14`, `24-26`; the junk and dirt-mound weeds and the cave weeds are
left out), `maryj*` and, stiffer, cacti sway. Per output row a horizontal
shift: 0 for the bottom 35% of the sprite, growing squared to the top, at
most 1.5-5.5 output pixels by the plant's height, a ~1.1 s sway out of step
per tile under slow gusts. The baked shadow moves with its rows.

- Compositing: the plant's own draw recomposes its rectangle widened by the
  sway margin: the layer under it is captured before the legacy draw, the
  shifted HD plant is drawn over it and pixels the legacy plant covers but
  the shifted one has left get what was under it, all with the buffer's
  current index as shadow (`hdLayerDrawSwayed`).
- Moving: a ticker (18 Hz) does not redraw the map. From each plant's last
  draw the scene keeps the colours under the first plant at every pixel,
  which pixels something else was drawn between plants, and a hash of what
  the plants left in the layer. Plants whose shifted rows changed are
  repainted in tile order over that base, only in the rows that moved,
  and a pixel is written only where the layer still shows what the plants
  left (critters in front, roofs, text stay). Unknown plants (new on screen)
  get one ordinary redraw. The test checks that this equals a whole redraw
  pixel for pixel. If a tick costs much (a forest at 3440x1440 on a busy
  machine) the ticks are spaced to keep it under about a fifth of the time;
  while the map view pans or zooms the plants wait.

Performance (Release, 3440x1440 at scale 2, hd_lights=1, smooth_scroll and
zoom on; presents per second and work per frame from `perf`; the machine
was running another agent's test suite meanwhile, so the numbers are noisy):

| | wind off | wind on |
|---|---|---|
| Arroyo slot 3 (140 trees on screen), idle | 56.4/s, 5.3 ms | 55.5/s, 4.4 ms |
| Arroyo slot 3, scrolling | 53.6/s, 5.3 ms | 40.8/s, 10.2 ms |
| Arroyo slot 9 (5 plants), idle | 55.7/s, 2.5 ms | 56.1/s, 2.5 ms |
| Arroyo slot 9, scrolling | 57.1/s, 1.8 ms | 57.8/s, 2.4 ms |

A wind tick costs 0.5 ms with a few plants and about 12 ms (26 max) with
140 trees, most of it in showing the recomposed rows (map view compose and
present). The gunshot light alone (hd_lights=0) costs 0.5 ms per field
update; with hd_lights=1 the frames of a shot reuse the field without shots
(the Den at night: 33 presents in the 600 ms after a burst, field max 15 ms).

Harness: `shoot <weapon pid> nearest|party <n> [secondary]`, `hdfx shot|wind
on|off`, `hdfx dump`, `hdfx waitshot <n>`, `hdfx shotclock hold|run`,
`hdfx windtime <ms>|run`, `hdfx legacycheck`, `hdfx shotlight` (see
`src/hd_dev_automation.h`). Review videos in
`docs/screens/phase2.15/`.

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

## Runtime scripted upscale (Phase 2.22)

The engine upscales at runtime, with the scripted pack's algorithm
(`src/hd_upscale.{h,cc}`, a C++ port of `enhance_scripted.py`), every frame
no pack has an image of, so `hd_pack=scripted` and manifest entries naming
`scripted` work without the 293 MB pack. Lookup order per frame:

1. the first pack (manifest order) whose image was made from the frame the
   game draws (`legacy.txt`, `hd_verify`): an installed scripted pack still
   wins, its images had the tiles' and walls' real neighbours;
2. the runtime upscale (`[graphics] hd_upscale=scripted`, default), also for
   frames whose pack image was rejected (art a mod replaced);
3. `hd_upscale=double`: the callers' fallback (pixels doubled, or legacy).

Critters (no pack has them) now take the HD object path when
`hd_upscale_critters=1` (default). Settings: `hd_upscale`,
`hd_upscale_critters`, `hd_upscale_precision` (16 default, 8 = exactly the
Python), `hd_upscale_preload`, `hd_upscale_cache`, `hd_upscale_threads`.

Palette boost (`hd_upscale_precision=16`): the 6-bit palette expanded at
full precision (v * 255 / 63) into a float pipeline, a de-banding pass where
the picture is smooth (range-limited tent filter of radius 8 at 2x, ~1.6
palette steps; local activity decides where), 4x4 ordered dither to 8 bits.
The HD layer and compositor stay 8 bits per channel (`HdImage` is
0xAARRGGBB); a 10/16-bit layer would mean wider compositing buffers
throughout `hd_layer.cc`, not done here.

Pre-upscaling in background threads: the early interface art at game start
(`kEarlyInterfaceArt` in `hd_art.cc`) and every frame of the art a map uses,
queued while `_obj_preload_art_cache` holds it locked (first frames ahead of
the others). A frame not ready when first drawn is upscaled right then on
the main thread (queued) or waited for (running); large ones are split into
strips over the cores with identical results. Never drawn doubled first:
the map's image tables keep the first image they get. The CE main menu
background is made by Stimpak in memory, so it cannot be queued at start
(upscaled on first draw, ~25-45 ms).

Disk cache (`hd_upscale_cache=1`, off by default): `data/hd/cache/xx/<key>.rgba`,
raw RGBA keyed by FNV-1a of pixels, palette, flags and algorithm version;
about 70-115 MB per town.

Timings (Apple M5 Pro, busy machine, Debug build with hd_upscale.cc at -O2;
`tests/unit/hd_upscale_test.cc`, `tools/dev/runtime_upscale_perf.py`):

| | |
|---|---|
| single thread, precision 8 / 16 | ~50 / ~86 ns per output pixel |
| 8 pool threads, precision 16 | ~15 ns per output pixel |
| start: 49 interface frames (11.3 Mpx) | queued in 25-35 ms (main thread), done 183 ms; 1 thread 1041 ms; warm disk cache 25 ms |

| map (slot) | frames queued | queueing (main) | done in background | main-thread stall at first draw | no preload | 1 thread | warm cache |
|---|---|---|---|---|---|---|---|
| Arroyo (3) | 754 | 9 ms | 181 ms | 83 ms | 526 ms | 868 ms / 449 ms | 64 ms / 4 ms |
| Klamath (1) | 2150 | 20 ms | 403 ms | 137 ms | 554 ms | 2335 ms / 548 ms | 87 ms / 4 ms |
| Den (8) | 2225 | 21 ms | 423 ms | 121 ms | 386 ms | 2616 ms / 422 ms | 82 ms / 4 ms |

Frame time is unchanged (presents 1.3-3.5 ms and whole-map redraws 9-12 ms in
every configuration, within run-to-run noise). The HD art cache holds 72-115
MB after a town's frames are upscaled (offline pack: 20-26 MB; under the
256 MB `hd_cache_mb`).

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

## Ground variants (Phase 2.14)

The desert ground (`edg*` tiles: most of the floor of Arroyo, Klamath and the
Den) repeated visibly: one tile, `edg5000`, alone is a quarter of all floor
squares. `ground_variants.py` gives every `edg*` tile 8 variants with real
ground detail: an exemplar texture of cracked dry earth, generated once from a
text prompt with the local SDXL model (`work/ground/exemplar_cracked.png`, no
game art goes in), lends each variant the fine detail of a different crop,
laid over the tile's own colours (rough, rocky tiles get less). The detail
fades out over the outer ~13 px of the diamond, so every variant joins every
neighbour exactly like the original tile.

```sh
cd tools/hd
.venv/bin/python ground_variants.py            # ~10 s; --exemplar regenerates the texture
```

Variants live in `<pack>/art/tiles/variants/<name>/<k>.png`. With `[graphics]
hd_ground_variants=1` `src/tile.cc` draws, on each map square, the variant a
hash of the square picks (the same one every time); tiles without variants
are drawn as before. The 8-bit picture does not change.


## Floors and roofs of every town (Phase 2.17)

Until now only the tiles of the save games' areas were in the packs; every
other map drew its floor and roof as the legacy tile doubled. `extract.py
--all-tiles` adds every floor and roof tile of all 155 maps (master.dat <
patch000.dat; 2580 tiles: 1286 floor, 1398 roof, some both) and the 501
other tiles of `tiles.lst` that no map places (scripts may), with the same
treatment as the approved area tiles (AI and scripted, padded on the floor
lattice). 3081 tiles in all, 2138 new: scripted 51 s, AI 78 s (M5 Pro).

```sh
cd tools/hd
.venv/bin/python extract.py --areas --objects --inven --interface --all-tiles
.venv/bin/python enhance_scripted.py $(cat work/keys_2.17.txt)   # the new keys
.venv/bin/python enhance_ai.py $(cat work/keys_2.17.txt)
.venv/bin/python install.py
.venv/bin/python tilemap.py --maps ncr1 vctycocl newr1 ...       # seams on a patch of each map
python materials_sheet.py packs        # -> docs/screens/phase2.17/packs_*.png (scipy: the
                                       #    SDXL venv, see "Ground variants")
```

`materials_sheet.py` draws real map regions (floor or roof layer) offline
from the map grids with the `tilemap.py` layout, so any town can be checked
without running the game.

Observations (`docs/screens/phase2.17/packs_*.png`):

- AI is the better pack for most of the new art too: plank roofs (Gecko,
  Broken Hills), Vault City's concrete domes, Enclave grid plates, the
  tanker's ship floor, metal plates of the Sierra depot, carpets.
- Two roof families go to the scripted pack in the mixed manifest: Navarro's
  corrugated metal (`vrof*`), where the AI turns parts into brick and the
  tiles look patchy (the 2.7 problem with `ruf*`/`jrt*`), and New Reno's
  fine-striped roofs (`salbr*`), where it smooths the stripes away.
- Tiles that are pieces of one larger picture (the Gecko reactor `geka*`,
  the military base's rock roof `mbrf*`, Navarro and Vault City buildings,
  church roofs, ...) showed thin seams across the picture: each tile was
  padded with copies of itself. `extract.py --all-tiles` now also writes
  `work/tile_neighbours.json`: for each tile and lattice offset (up to two
  squares), the neighbour it has on at least 90% of its placements on all
  maps, if that is another tile (1957 tiles). The enhancers pad with those
  real neighbours (`hdlib.tile_context`); the seams are gone. 1439 of the new
  tiles have such neighbours (re-run: scripted 29 s, AI 47 s). The approved
  area tiles were not re-run.
- Seams (`tilemap.py`, a 6x6 patch per map with the most different tiles):
  NCR, Vault City, New Reno, San Francisco, Broken Hills, Redding, Modoc,
  Gecko, Navarro, military base, Sierra depot and Enclave are all within the
  test limits: scripted seam_excess 0.0-4.4, grad_excess 1.00-1.09; AI
  -0.2-3.0 and 0.98-1.24 (the Gecko reactor went from 1.6 to 1.0).

`test_hd_town_*` (tests/run_tests.py) moves the party of slot 1 to New Reno,
NCR and Vault City with Stimpak's TRAVEL and checks that the floor shows pack
art there (2x2 output blocks with detail), not doubled legacy pixels.

### More ground materials (Phase 2.17)

`docs/screens/phase2.17/families_*.png` sorts every floor family (most map
squares first) into the materials with variants and the floors that stay
regular (wood, brick, metal plates and grids, floor tiles, carpet, New
Reno's building floors `towr`, pictures such as the Gecko reactor, colour
cycling, caves). `ground_variants.py` has a material table: families,
exemplar, prompt and seed, scale, strength. Approved from the proposal
(`proposal_*.png`) and installed by default:

| Material | Families | Exemplar | Strength | Tiles |
|---|---|---|---|---|
| desert (2.14) | `edg`, `edgs` | cracked dry mud | 1.0 | 49 |
| asphalt (option 2) | `road` | asphalt with a crack network | 0.8, seamless | 39 |
| pavement (option 2) | `curb`, `cmt` | cracked concrete slab | 0.55 | 63 |
| grass | `grass` | short dry lawn | 0.8, seamless | 2 |

Caves were proposed and declined (almost no visible effect on the dark cave
floor); the other options (`asphalt_aggregate`, `concrete_strong`, `cave`)
stay in the table for the sheets. New in this phase:

- `project`: the exemplar is laid onto the isometric floor plane (an affine
  map with the floor lattice), so cracks and grain are foreshortened like
  the ground instead of facing the camera.
- `seamless`: for continuous surfaces (asphalt, grass) the detail does not
  fade out towards the diamond edges, which showed the tile grid on dark,
  even asphalt. It crossfades to a field that is periodic on the floor
  lattice (`lattice_field`, built from the same exemplar), so every tile
  of the material has the same detail along its edges and neighbouring
  squares join whatever variants they show. Pavement slabs keep the fade:
  their joints run along the tile edges anyway.

```sh
cd tools/hd
python ground_variants.py              # all approved materials into both packs: ~3 s
python materials_sheet.py families
python materials_sheet.py proposal asphalt asphalt_aggregate --maps newr2 depolv1
python materials_sheet.py exemplars desert asphalt concrete grass
.venv/bin/python towns_sheet.py        # runs the game hidden -> docs/screens/phase2.17/town_*.png
```

(`python` = a venv with scipy, torch and diffusers; the exemplars come from
the local SDXL base model, offline, about 23 s each; they are kept in
`work/ground/`.) Proposal variants go to `work/ground/proposal/<material>/`,
not into the packs.

`towns_sheet.py` loads save slot 1 and moves the party with the new harness
command `gotomap <map> [<elevation> <tile>]` (a map transition, as through an
exit grid) to New Reno, NCR, Vault City, Broken Hills, Redding, Modoc, Gecko,
San Francisco, Navarro, the military base and the Enclave, and shows legacy
and HD side by side (`town_<map>.png`). Walls, scenery and critters of those
towns have no pack art yet.

`test_hd_town_*` also checks each material's variants in the game: in New
Reno (asphalt, pavement) and Vault City's council area (grass, reached with
`gotomap`) the frame with all variants is compared with the same view after
`hdpack town_no_<material>`, a pack whose manifest sends that material's
variants to a pack that does not exist. Asphalt changes 33% of the New Reno
frame, pavement 4%, grass 30% of the Vault City one; the 8-bit picture is
identical with and without variants.

The `test_hd_map_*` goldens show the packs' own tiles, so those tests now run
with `hd_ground_variants=0` (with the desert variants installed, slots 3, 5
and 9 differed from them by 7-11%). Comparisons of the 8-bit picture in the
town tests ignore pixels whose colour is a colour-cycling one in both frames
(New Reno's trash-can fires animate between runs).

## Walls, scenery and items of every town (Phase 2.18)

Until now only the objects of the save games' areas had pack art (Phase
2.8); everything else drew as doubled legacy pixels. `extract.py
--all-objects` (with `--objects --all-tiles`) adds every wall, scenery and
item placed on any of the 155 maps and the ground art of every item in
`items.lst` (items are carried and dropped anywhere): 3194 art files in all
(1454 walls, 1549 scenery, 191 items), 1933 new, 2559 new assets with the
frames of 100 new multi-frame FRMs (16.7 Mpx). Scripted 72 s, AI 274 s (M5
Pro). Effects (`art/misc`: explosions, projectiles) are left out.

```sh
cd tools/hd
.venv/bin/python extract.py --areas --objects --inven --interface --all-tiles --all-objects
.venv/bin/python enhance_scripted.py $(cat work/keys_2.18.txt)   # the new keys
.venv/bin/python enhance_ai.py $(cat work/keys_2.18.txt)
.venv/bin/python install.py            # keeps the ground variants in the packs
.venv/bin/python wallmap.py newr1 ncr1 vctydwtn sfchina navarro encdet mbase12 broken1 \
    --sheet ../../docs/screens/phase2.18
.venv/bin/python pack_review.py        # mixed-pack candidates
.venv/bin/python towns_sheet.py        # runs the game hidden -> docs/screens/phase2.18/town_*.png
```

Wall runs. Walls are drawn as adjacent pieces, one FRM per hex. Each piece
was enhanced alone, with transparency around it, so its edges did not
continue into the next piece (the Enclave's riveted panels showed doubled
lines at the joins). `extract.py --all-objects` now also writes
`work/wall_neighbours.json`: for every wall FRM, the walls around it (within
16 px) in its most common placement over all maps, with their offsets
(1454 pieces, 5.5 neighbours on average). The enhancers draw those
neighbours around the piece before upscaling (`hdlib.object_context`), as
the floor tiles get their neighbours. The 768 new walls were made this way
(scripted 18 s, AI 48 s); the approved area walls were not re-run.
`wallmap.py` assembles the walls of a map window the way `objectGetRect`
places them and measures seams like `tilemap.py` (the pieces against the
same enhancer run on the whole composite). With wall context:

| Map | scripted seam_excess / grad_excess | ai |
|---|---|---|
| kladwtwn, denbus1 (2.8) | 1.7-2.7 / 1.06-1.08 | 0.8-2.0 / 0.93-1.01 |
| newr1, ncr1, sfchina, broken1 | 1.3-2.2 / 1.03-1.08 | 0.5-1.8 / 0.94-1.00 |
| navarro, mbase12 | 4.2-4.3 / 1.02-1.03 | 1.8-2.9 / 0.94-1.02 |
| encdet (Enclave) | 3.3 / 1.18 | 3.9 / 1.12 (5.5 / 1.32 without context) |
| vctydwtn | 10.5 / 0.87 | 5.4 / 0.57 |

Vault City's numbers come from its chain-link fences (transparent holes the
whole-composite reference fills in), not from seams: `walls_vctydwtn.png`
shows none. No visible seams in the `walls_*.png` and `town_*.png` sheets.

Mixed pack (`pack_review.py`: 3128 art files measured, 180 flagged, looked
at by eye): the AI is fine for nearly all of them (the edge metric flags
chain-link fences and thin rails, which look the same in both packs because
alpha is exact). To the scripted pack: the round grilles `encl37`, `encl38`
and `shipsc09` (the AI wipes the grille out) and the opening containers
`ammobox1`, `ammobox4`, `walllkr1`, `walllkr3`, `safe` (the AI renders the
frames differently, so still parts shimmer while they open).

The wind (Phase 2.15) picks plants by name, so the new trees and bushes
sway too. `test_hd_town_*` also check that walls, scenery and items in New
Reno, NCR and Vault City show pack art (2x2 blocks with detail: 98-100%)
and that the 8-bit picture equals the same view with `hd=0`.

## Main menu palm (Phase 2.19)

The high-resolution main menu background (`HR_MAINMENU.FRM`) has a palm
tree whose original is a coarse 3D render: crumpled, blocky fronds that an
upscale only sharpens. `palm_repaint.py` replaces just the palm of the AI
pack's 2x image with a repaint by the local SDXL model (inpainting with a
mask of the canopy and trunk; seed and strength fixed, option 2 of
`docs/screens/phase2.19_palm/`). Run it after `install.py`:

```sh
cd tools/hd
.venv/bin/python palm_repaint.py     # ~25 s
```

## Main menu palm (Phase 2.19)

The high-resolution main menu background (`HR_MAINMENU.FRM`) has a palm
tree whose original is a coarse 3D render: crumpled, blocky fronds that an
upscale only sharpens. `palm_repaint.py` replaces just the palm of the AI
pack's 2x image with a repaint by the local SDXL model (inpainting with a
mask of the canopy and trunk; seed and strength fixed, option 2 of
`docs/screens/phase2.19_palm/`). Run it after `install.py`:

```sh
cd tools/hd
.venv/bin/python palm_repaint.py     # ~25 s
```


## Legacy sources and content mods (`legacy_art.py`)

HD images are looked up by art path, so a mod that ships another picture
under the same path (the Restoration Project repaints the world map, the
dialogue frame, the hi-res main menu, some items, and replaces about 170
tiles) would get the pack's picture of the original drawn over its own.
`install.py` therefore ends with `legacy_art.py hashes`, which writes
`data/hd/<pack>/legacy.txt`: for every image and `.fhd` frame the legacy art
path, direction, frame and an FNV-1a 64 hash of the frame's size and palette
indices, taken from the art the pack was made from (the game's chain:
master.dat < critter.dat < f2_res.dat < ce.dat < patch000.dat < data/).
`src/hd_art.cc` uses an image only while the running game's frame has that
hash (`[graphics] hd_verify=1`, the default); otherwise the frame is drawn
from the game, doubled. Ground variants follow their tile.

```sh
.venv/bin/python legacy_art.py hashes ../../out/build/macos/data/hd/ai ../../out/build/macos/data/hd/scripted
.venv/bin/python legacy_art.py compare ../../out/build/macos/data/hd/ai --base ~/Games/Fallout\ 2 \
    --mod ~/dev/rpu-test/game --md report.md   # what a mod replaces and adds
```

See `docs/rpu_compatibility.md` for the Restoration Project results. Hashing
both packs takes about 10 minutes, most of it `ce-dat-tool extract`.

## A pack for a mod's own art (`mod_pack.py`, Phase 2.21: the Restoration Project)

`legacy.txt` keeps the packs' images off art a mod replaced; `mod_pack.py`
makes a second pack from the mod's own art, so it gets HD images too.

```sh
cd tools/hd
export HD_MODELS_DIR=...                   # weights as in MODELS.md
.venv/bin/python mod_pack.py extract --mod ~/dev/rpu-test/game   # ~10 min
.venv/bin/python mod_pack.py enhance                             # AI x2plus, ~5 min
.venv/bin/python mod_pack.py install --mod ~/dev/rpu-test/game   # -> out/build/macos/data/hd/rpu, ~2.5 min
```

(`--pack NAME`, default `rpu`; work files in `work/<pack>/`, images in
`work/<pack>/assets/<pack>/`; `--base` is the original game, default
`FALLOUT2_DIR`.)

- **File chain.** The mod's art is read as the engine opens the installed
  game (`legacy_art.engine_layers`): master.dat < critter.dat < f2_res (the
  game's own, from `fallout2.cfg`/`f2_res.ini`) < ce.dat < patchNNN.dat <
  `mods/mods_order.txt` < data/. `mods_order.txt` is opened top to bottom
  and each archive goes in front of the chain (`src/sfall_ext.cc`), so the
  **last line wins**. That is sfall's order too (the RPU install's
  `ddraw.ini`: "loaded line by line from top to bottom", "master_patches >
  critter_patches > mods_order.txt > PatchFileXX > patchXXX.dat > sfall.dat >
  critter_dat > f2_res_patches > f2_res_dat > master_dat"), and it is what
  RPU needs: the enhanced world map (`rpu_enhanced_worldmap.dat`) comes after
  `rpu.dat` and replaces its world map tiles. CE does not open `sfall.dat`
  (sfall's scripts only).
- **What goes in.** Every FRM of the HD types (`legacy_art.HD_TYPES`;
  critters stay legacy) the mod adds, and of the art it changes only the
  frames whose size or palette indices differ from the original game's.
  Frames the mod left alone stay empty in `.fhd` files, so the pack can never
  match a frame of the original game. Intro sequences (`seq*`, `arroyo01`,
  `dp`) are left out; endgame slides are in.
- **Own palettes.** `art/intrface/<name>.frm` with an `art/intrface/<name>.pal`
  next to it (endgame slides `eg_*`, `death`, `helpscrn`) is converted
  through that palette.
- **Neighbours from the mod's maps.** Floor/roof kind, the fixed tile
  neighbours (`extract.tile_neighbours`) and the wall runs
  (`wallmap.wall_neighbours`) come from the mod's `tiles.lst`, maps and
  protos; neighbours outside the pack are converted as context only.
  `extract.read_map_grids` now reads gzipped maps too.
- **Colour cycling** (229-254) stays transparent as everywhere; multi-frame
  art is converted per frame and direction and packed into `.fhd` by
  `install.py`.
- **legacy.txt** is hashed through the mod's chain (`install.py --mod`,
  `legacy_art.py hashes --mod DIR`), so the engine uses an image only while
  the game draws exactly the mod's frame.

The engine tries the packs of a manifest line in order (`src/hd_art.h`):
`hd/mixed/manifest.txt` has `* = ai rpu`, and `scripted rpu` for the Vault
City roofs RPU redraws. On the original game the ai image of every frame
matches and is used; on RPU, where the mod's frame differs, ai's is skipped
and rpu's matches. One install serves both. Checked offline: none of the
3379 rpu frames hashes like the original game's frame (two vanilla
installs), all 3379 like RPU's.

RPU v2.4.34 (`~/dev/rpu-test/game`): 2185 FRMs (1876 new, 309 changed),
3379 images, 160 MB:

| type | FRMs new / changed | images (.png / .fhd) | size |
|---|---|---|---|
| intrface | 11 / 32 | 43 / 0 | 41 MB |
| inven | 39 / 7 | 46 / 0 | 1.0 MB |
| items | 24 / 1 | 21 / 4 (24 frames) | 0.9 MB |
| scenery | 472 / 85 | 426 / 131 (1277 frames) | 89 MB |
| skilldex | 2 / 7 | 9 / 0 | 1.1 MB |
| tiles | 971 / 166 | 1137 / 0 | 16 MB |
| walls | 357 / 11 | 361 / 7 (35 frames) | 12 MB |

The changed set is larger than the 254 of `docs/rpu_compatibility.md`, which
only counted art the vanilla packs have. RPU moves `patch000.dat` away, so
art the original game takes from it falls back to master.dat's older version
unless `rpu.dat` has its own; those frames differ too.

Times (M5 Pro): extract 575 s (mostly reading 176 maps and the walls on
them), AI 260 s of model time for 3379 images, install with hashes 153 s.
Review sheets: `docs/screens/phase2.21_rpu_pack/` (untracked).

The engine does not draw endgame slides through the HD layer yet (neither
the original game's nor RPU's), so the `eg_*` images are only used once
that is added; `death.frm` is (`src/main.cc`).
