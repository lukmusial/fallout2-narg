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
