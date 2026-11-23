#ifndef FALLOUT_HD_LIGHTS_H_
#define FALLOUT_HD_LIGHTS_H_

#include <stdint.h>

#include <vector>

namespace fallout {

// HD lighting (Phase 2.11, `[graphics] hd_lights=1`): coloured light, glow and
// soft shadows applied in RGB when the presenter writes the map window's
// pixels into the 2x framebuffer. Nothing is drawn into the 8-bit buffers or
// the HD layers, so game logic, hit-testing and screenshots of the 8-bit
// frame are unaffected, and with hd_lights=0 the presenter takes exactly the
// old path.
//
// The effect lives in a screen-space field (`HdLightField`) at a fraction of
// the logical resolution, rebuilt by the map side (hd_lights_map.cc) from the
// light-emitting objects, the colour-cycling (emissive) pixels on screen and
// the critters, and interpolated bilinearly per logical pixel, so falloffs
// are smooth instead of stepping per hex. Per output pixel:
//
//   out = clamp(in * (1 + mul) * (1 - shade) + add)
//
// `mul` (per channel) is coloured light, `add` additive glow (bloom) and
// `shade` soft shadow, applied only to HD pixels (floors, walls, scenery), so
// critters are never darkened by their own shadow.
//
// This file is engine-independent (no SDL, no game state) and unit-tested.

// Logical pixels per light cell (mul, add) and per shadow cell (shade).
constexpr int kHdLightCell = 4;
constexpr int kHdShadeCell = 2;

// Bits of the presenter's per-pixel HD mask.
constexpr unsigned char kHdMaskShows = 1; // the HD block shows
constexpr unsigned char kHdMaskLit = 2; // a map window pixel: lighting applies

// Effect groups (`hdlights` harness command, settings).
enum : unsigned {
    kHdLightColour = 1,
    kHdLightGlow = 2,
    kHdLightShadows = 4,
    // Phase 2.13: lights carried by walking critters follow the drawn sprite
    // (see hdLightFieldAddMovedLight).
    kHdLightFollow = 8,
    kHdLightAll = 15,
};

// A rectangle of cells, right and bottom exclusive (empty when x0 >= x1).
struct HdLightBox {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;

    bool empty() const { return x0 >= x1 || y0 >= y1; }
    // Grows the box to contain `other`.
    void add(const HdLightBox& other);
};

struct HdLightField {
    // Logical size covered (the screen).
    int width = 0;
    int height = 0;
    int cellsX = 0;
    int cellsY = 0;
    int shadeX = 0;
    int shadeY = 0;
    // Per light cell, 3 channels (R, G, B): extra light factor (0 = none)
    // and additive glow in 8-bit levels.
    std::vector<float> mul;
    std::vector<float> add;
    // Per shadow cell: darkening 0..1.
    std::vector<float> shade;
    // Per cell: the 2x2 cells a bilinear sample starting here reads are not
    // all neutral (see hdLightFieldFinish).
    std::vector<unsigned char> lightActive;
    std::vector<unsigned char> shadeActive;
    // Cells that may be non-neutral (everything outside is neutral), so
    // clearing, finishing and comparing skip the rest.
    HdLightBox lightBox;
    HdLightBox shadeBox;
};

void hdLightFieldInit(HdLightField* field, int width, int height);
void hdLightFieldClear(HdLightField* field);
// Computes the active flags; call after building, before applying.
void hdLightFieldFinish(HdLightField* field);

// Applies the field to the 2x2 output block of logical pixel (x, y); `dest`
// points at the block's top-left 0x00RRGGBB pixel, `pitch` in pixels.
// `shaded`: the pixel is an HD pixel (shadows apply).
void hdLightApplyBlock(const HdLightField& field, int x, int y, bool shaded, uint32_t* dest, int pitch);

// Applies the field to logical pixels [left, left + width) of row `y`:
// `maskRow` is the presenter's mask for those pixels (only kHdMaskLit pixels
// change; kHdMaskShows ones also get shadows) and `dest` the 2x output at
// pixel `left` (two rows, `pitch` apart). The light is interpolated per
// logical pixel and applied to every output pixel in RGB.
void hdLightApplyRow(const HdLightField& field, int y, int left, int width, const unsigned char* maskRow, uint32_t* dest, int pitch);

// A logical rectangle.
struct HdLightRect {
    int left;
    int top;
    int width;
    int height;
};

// Rectangles (in blocks of 16 light cells) where `a` and `b` differ by more
// than a small epsilon, grown by the bilinear support. Both fields must have
// the same size.
void hdLightFieldDiff(const HdLightField& a, const HdLightField& b, std::vector<HdLightRect>* rects);

// Emissive (glowing) pixels: for every light cell, the average over its
// logical pixels of weight[index] * presented colour (0..1 per channel),
// counting only pixels whose `mask` has kHdMaskLit. `grid` gets 3 floats per
// light cell of a field of `width` x `height` logical pixels; `box` the
// cells that are not 0.
void hdLightEmissive(const unsigned char* index, const unsigned char* mask, int pitch, int width, int height,
    const unsigned char* palette8, const float* weight, std::vector<float>* grid, HdLightBox* box);

// Separable box blur of a 3-channel grid, `passes` times (3 passes are close
// to a Gaussian); radii in cells. The sum is preserved away from the edges.
// Only `box` (the non-zero cells) and what the blur spreads into is
// processed; `box` grows accordingly.
void hdLightBlur(std::vector<float>* grid, int cellsX, int cellsY, int radiusX, int radiusY, int passes, HdLightBox* box);

// field.mul (or field.add when `additive`) += grid * gain within `box`.
void hdLightFieldAddGrid(HdLightField* field, const std::vector<float>& grid, const HdLightBox& box, float gain, bool additive);

// A light's reach as smooth weights over light cells: the values given at
// sample points (logical coordinates, e.g. hex centres with the engine's
// per-hex light values normalised to 0..1, and 0 at the hexes the light does
// not reach) interpolated with a tent kernel of half-size kernelX x kernelY
// (normalised convolution), then raised to `exponent` for a softer falloff.
struct HdLightSample {
    float x;
    float y;
    float value;
};

struct HdLightFootprint {
    // Cell rectangle (light cells) and its weights.
    int cellX = 0;
    int cellY = 0;
    int cellsX = 0;
    int cellsY = 0;
    std::vector<float> weight;
};

void hdLightFootprintBuild(const std::vector<HdLightSample>& samples, float kernelX, float kernelY, float exponent,
    HdLightFootprint* footprint);

// field.mul += footprint * colour * strength (clipped to the field).
void hdLightFieldAddFootprint(HdLightField* field, const HdLightFootprint& footprint, const float* colour, float strength);

// A soft elliptical shadow centred at (cx, cy) (logical), semi-axis
// (axisX, axisY) and a perpendicular semi-axis of length `minor`;
// darkness at the centre `darkness`, smooth to 0 at the edge. Shadows
// combine as 1 - (1 - a)(1 - b).
void hdLightFieldAddShadow(HdLightField* field, float cx, float cy, float axisX, float axisY, float minor, float darkness);

// A light carried by a walking critter (Phase 2.13). The engine lights hexes:
// its light sits on the critter's hex while the sprite is drawn up to a hex
// away from it during walk frames, so the lit circle would jump hex by hex.
// Per hex (logical position of its centre) `total` is the engine's summed
// light (lightGetTrueTileIntensity, before clamping) and `own` this light's
// part of it; include the ring of unlit hexes around the light (own 0).
// The field gets the factor that turns the light the floors were drawn with,
//   clamp(total, ambient, maximum),
// into the light with this light moved by (dx, dy) to the drawn sprite,
//   clamp(total - own + own(p - (dx, dy)), ambient, maximum),
// as extra light (mul, negative where it darkens); values are interpolated
// between hex centres with a tent kernel of half-size kernelX x kernelY.
// Nothing is added when (dx, dy) is (0, 0).
struct HdMovedLightHex {
    float x;
    float y;
    float total;
    float own;
};

void hdLightFieldAddMovedLight(HdLightField* field, const std::vector<HdMovedLightHex>& hexes, float dx, float dy,
    float ambient, float maximum, float kernelX, float kernelY);

// The light (engine scale) at logical point (x, y) interpolated from `hexes`
// as hdLightFieldAddMovedLight does, with the own part moved by (dx, dy):
// clamp(total - own + own(p - d), ambient, maximum); 0 where no hex is near.
float hdMovedLightAt(const std::vector<HdMovedLightHex>& hexes, float x, float y, float dx, float dy,
    float ambient, float maximum, float kernelX, float kernelY);

} // namespace fallout

#endif /* FALLOUT_HD_LIGHTS_H_ */
