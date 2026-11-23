#ifndef FALLOUT_HD_LAYER_H_
#define FALLOUT_HD_LAYER_H_

#include <stdint.h>

#include <vector>

namespace fallout {

// HD layers: full-resolution (2x) true-colour pixels attached to a window,
// shown in place of the window's 8-bit pixels for as long as those 8-bit
// pixels are unchanged.
//
// The game keeps drawing into its 8-bit window buffers. An HD-aware draw does
// the legacy blit first, then draws the 2x RGBA image into the window's layer
// and records, per logical pixel, the palette index the window buffer had
// at that moment (the "shadow"). When the window is composited, a logical
// pixel shows its 2x2 HD block only if the layer holds a pixel there and the
// window buffer still equals the shadow; otherwise the legacy palette colour
// (pixel-doubled) is shown. Any legacy code that later draws over an area, by
// any route, therefore wins automatically, and redrawing the original
// background re-validates the HD pixels.
//
// HD colours are stored relative to the base palette (`_cmap`). Fades and
// brightness reach them through palette modulation: each HD pixel is mapped
// with the change between the base and the presented colour of its shadow
// index (see `hdModulationBuild`). Colour-cycling indices (229..254) are never
// covered by HD pixels, so the palette animation shows through.
//
// This file is engine-independent (no SDL, no game state) so its rules can be
// unit-tested without game data; the window manager and presenter call it.

constexpr int kHdScale = 2;
constexpr int kHdCycleFirst = 229;
constexpr int kHdCycleLast = 254;

inline bool hdIsCyclingIndex(unsigned char index)
{
    return index >= kHdCycleFirst && index <= kHdCycleLast;
}

// A true-colour image, 0xAARRGGBB with straight (non-premultiplied) alpha.
struct HdImage {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;
};

// Text overlay guard bits (HdLayer::textGuard): kHdTextSelf marks a text
// pixel (the legacy text draw wrote it); bits 0..7 (kHdTextNeighbours) mark
// the neighbouring text pixels a fringe pixel's 2x colours were made from.
constexpr uint16_t kHdTextSelf = 0x100;

struct HdTextNeighbour {
    int dx;
    int dy;
};

constexpr HdTextNeighbour kHdTextNeighbours[8] = {
    { -1, -1 }, { 0, -1 }, { 1, -1 }, { -1, 0 }, { 1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 }
};

struct HdLayer {
    // Logical size (the window's size); `pixels` is kHdScale times larger in
    // each direction and holds opaque 0x00RRGGBB colours.
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;
    std::vector<unsigned char> shadow;
    std::vector<unsigned char> valid;

    // Text overlay (Phase 2.9, see hdLayerDrawText): 2x text drawn over
    // whatever the layer or the legacy buffer shows. Empty until the first
    // text draw. `textPixels` holds premultiplied 0xAARRGGBB colours (2x);
    // per logical pixel `textGuard` says what must still be in the window
    // buffer for the text to show, `textShadow` is the buffer index a text
    // pixel had after the legacy draw and `textUnder` the index before it
    // (what the text was drawn over).
    std::vector<uint32_t> textPixels;
    std::vector<uint16_t> textGuard;
    std::vector<unsigned char> textShadow;
    std::vector<unsigned char> textUnder;
    // Rows [textTop, textBottom) hold all text (empty when equal), so
    // scrolling and compositing skip the rest.
    int textTop = 0;
    int textBottom = 0;
};

void hdLayerInit(HdLayer* layer, int width, int height);
// Removes all HD pixels and text.
void hdLayerClear(HdLayer* layer);

// A 2x text draw that legacy code has just made at 1x (hd_text.cc renders
// the same string with 2x glyphs). The rectangle (x, y, width, height) is in
// layer coordinates; `mask` (1x, pitch `width`) is non-zero where the legacy
// draw wrote a pixel (glyphs, shadow, underline, outline), `pixels` (2x,
// pitch 2 * width) the premultiplied 2x colours, relative to the base
// palette like all HD colours, and `before` (pitch `width`) the window
// buffer's indices in the rectangle before the legacy draw.
struct HdTextDraw {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    const unsigned char* mask = nullptr;
    const uint32_t* pixels = nullptr;
    const unsigned char* before = nullptr;
};

// Records the text in the layer's text overlay. Text pixels (mask set)
// show their 2x block for as long as the window buffer keeps the index the
// legacy draw left there; fringe pixels (mask clear, but the 2x glyphs reach
// into them) show theirs for as long as every neighbouring text pixel they
// were made from does. Under the text, the compositor shows the layer's HD
// pixel when it is valid for the index the text was drawn over, otherwise
// that index's base palette colour. The legacy buffer stays the truth:
// once legacy code draws over the text (or erases it), its pixels show.
void hdLayerDrawText(HdLayer* layer, const unsigned char* buffer, int bufferPitch, const HdTextDraw& draw);

// Whether the text overlay shows at logical pixel (x, y) of a layer whose
// window buffer is `buffer`.
inline bool hdLayerTextShows(const HdLayer* layer, const unsigned char* buffer, int bufferPitch, int x, int y)
{
    if (y < layer->textTop || y >= layer->textBottom) {
        return false;
    }

    size_t offset = static_cast<size_t>(y) * layer->width + x;
    uint16_t guard = layer->textGuard[offset];
    if (guard == 0) {
        return false;
    }

    if ((guard & kHdTextSelf) != 0 && buffer[static_cast<size_t>(y) * bufferPitch + x] != layer->textShadow[offset]) {
        return false;
    }

    for (int bit = 0; bit < 8; bit++) {
        if ((guard & (1 << bit)) == 0) {
            continue;
        }
        int nx = x + kHdTextNeighbours[bit].dx;
        int ny = y + kHdTextNeighbours[bit].dy;
        if (nx < 0 || ny < 0 || nx >= layer->width || ny >= layer->height) {
            return false;
        }
        size_t neighbour = static_cast<size_t>(ny) * layer->width + nx;
        if ((layer->textGuard[neighbour] & kHdTextSelf) == 0
            || buffer[static_cast<size_t>(ny) * bufferPitch + nx] != layer->textShadow[neighbour]) {
            return false;
        }
    }

    return true;
}

// The 2x2 block the compositor shows at logical pixel (x, y) when the text
// overlay shows there: the text over the layer's HD pixel (when valid for the
// index under the text) or the base palette colour of that index.
void hdLayerTextBlock(const HdLayer* layer, const unsigned char* buffer, int bufferPitch, int x, int y,
    const unsigned char* basePalette8, uint32_t* block, int blockPitch);

// Composites the text overlay into the layer's own pixels wherever it shows
// (valid, shadowed by the buffer's index) and removes the overlay; used
// before effects that only know the layer's pixels (main menu darkening).
void hdLayerBakeText(HdLayer* layer, const unsigned char* buffer, int bufferPitch, const unsigned char* basePalette8);

// Draws `image` into the layer so that it covers the logical rectangle
// (x, y, width, height); the image is resampled (bilinear) when it is not
// exactly kHdScale times that size. `buffer` is the window's 8-bit buffer
// after the legacy draw of the same area; its indices become the shadow.
// Partially transparent HD pixels are blended over what the layer shows at
// that point (the layer's own pixel, or the base palette colour of the legacy
// index). Fully transparent 2x2 blocks leave the layer untouched; logical
// pixels on colour-cycling indices are left to the legacy renderer.
// `basePalette8` is the base palette at 8 bits (R, G, B per entry).
void hdLayerDrawImage(HdLayer* layer, const unsigned char* buffer, int bufferPitch,
    int x, int y, int width, int height, const HdImage& image, const unsigned char* basePalette8);

// Optional restrictions for hdLayerDrawImageEx.
struct HdDrawOptions {
    // Only logical pixels inside this rectangle (layer coordinates, right and
    // bottom exclusive) are drawn; used to draw part of an image, e.g. when a
    // screen restores a region of its background.
    int clipLeft = 0;
    int clipTop = 0;
    int clipRight = 1 << 30;
    int clipBottom = 1 << 30;

    // The legacy art the HD image was made from, at logical resolution and
    // aligned with the image rectangle (x, y). When set, a logical pixel gets
    // its HD block only where the window buffer equals the art there, so text
    // or numbers that legacy code drew into the art (or over it) stay legacy,
    // whatever order the draws happen in.
    const unsigned char* source = nullptr;
    int sourcePitch = 0;
};

void hdLayerDrawImageEx(HdLayer* layer, const unsigned char* buffer, int bufferPitch,
    int x, int y, int width, int height, const HdImage& image, const unsigned char* basePalette8,
    const HdDrawOptions& options);

// Lighting in RGB (Phase 2.7). `intensity` uses the engine's light scale:
// 0x10000 leaves the colour unchanged, less darkens it towards black
// (multiplies), more brightens it towards white, as the legacy intensity
// tables do, but without quantising to 128 steps and the palette.
inline uint32_t hdLight(uint32_t rgb, int intensity)
{
    if (intensity == 0x10000) {
        return rgb & 0xFFFFFF;
    }

    uint32_t r = (rgb >> 16) & 0xFF;
    uint32_t g = (rgb >> 8) & 0xFF;
    uint32_t b = rgb & 0xFF;
    if (intensity < 0x10000) {
        // Each channel times scale / 0x10000, rounded; red and blue share
        // one 64-bit multiply (red at bit 32, blue at bit 0).
        uint64_t scale = intensity > 0 ? static_cast<uint64_t>(intensity) : 0;
        uint64_t redBlue = ((static_cast<uint64_t>(r) << 32) | b) * scale + 0x0000800000008000ull;
        r = static_cast<uint32_t>(redBlue >> 48) & 0xFF;
        b = static_cast<uint32_t>(redBlue >> 16) & 0xFF;
        g = static_cast<uint32_t>((g * scale + 0x8000) >> 16);
    } else {
        uint32_t scale = static_cast<uint32_t>(intensity < 0x20000 ? intensity - 0x10000 : 0x10000);
        r += ((255 - r) * scale + 0x8000) >> 16;
        g += ((255 - g) * scale + 0x8000) >> 16;
        b += ((255 - b) * scale + 0x8000) >> 16;
    }
    return (r << 16) | (g << 8) | b;
}

// A lit draw of a legacy frame's HD image (floor and roof tiles): the legacy
// code has just drawn the frame, with lighting, into the window buffer.
struct HdLitDraw {
    // The legacy frame (index 0 transparent) and its HD image, which must be
    // exactly kHdScale times the frame's size.
    const unsigned char* frame = nullptr;
    int framePitch = 0;
    const HdImage* image = nullptr;

    // The drawn part: frame pixel (frameX, frameY) lands on logical layer
    // pixel (x, y); width x height logical pixels.
    int frameX = 0;
    int frameY = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    // Per-pixel intensities aligned with the drawn part (row pitch in ints),
    // or `constantIntensity` when null.
    const int* intensity = nullptr;
    int intensityPitch = 0;
    int constantIntensity = 0x10000;

    // When set, a pixel is drawn only where the window buffer equals
    // expected[frame index], the colour the plain legacy draw produces there;
    // pixels the legacy code drew differently (e.g. blended under the roof
    // cut-out) keep whatever the layer had.
    const unsigned char* expected = nullptr;
};

// For every non-transparent frame pixel of the drawn part: the HD block,
// lit, becomes the layer's pixel with the window buffer's index as shadow.
// Blocks that are fully transparent in the HD image (colour-cycling source
// pixels) and pixels whose buffer index is a colour-cycling index are made
// invalid, so the legacy pixel shows. Partially transparent HD pixels are
// blended over what the layer shows there (or the base palette colour).
void hdLayerDrawLit(HdLayer* layer, const unsigned char* buffer, int bufferPitch, const HdLitDraw& draw, const unsigned char* basePalette8);

// Blended draws (Phase 2.8): translucent objects, the cut-out ("egg") in
// walls and roofs around the player, paletted outlines. The legacy code
// blends into the window buffer, which loses what was under it, so the
// colours under the draw are captured first:
//
// For the logical rectangle (x, y, width, height), `under` receives
// (2 * width) x (2 * height) colours (row pitch 2 * width): the layer's HD
// block where it shows, else the base palette colour of the buffer index.
// Alpha is 0xFF, or 0 where there is no colour to blend with (a
// colour-cycling index, or outside the layer).
void hdLayerCapture(const HdLayer* layer, const unsigned char* buffer, int bufferPitch,
    int x, int y, int width, int height, const unsigned char* basePalette8, std::vector<uint32_t>* under);

enum class HdBlendMode {
    // Translucent objects (the legacy blend tables): the HD pixel's grey
    // level sets how much of `tint` covers what is under it; the result is
    // lit: light(lerp(under, tint, alpha)).
    Tint,
    // The cut-out: m = mask value (0..128) at the pixel; 0 draws the lit HD
    // pixel, else lerp(under, light(hd), m / 128).
    Mask,
};

struct HdBlendDraw {
    // As HdLitDraw (frame part at (frameX, frameY) lands on logical (x, y)).
    const unsigned char* frame = nullptr;
    int framePitch = 0;
    const HdImage* image = nullptr;
    int frameX = 0;
    int frameY = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int intensity = 0x10000;

    // hdLayerCapture of exactly (x, y, width, height), taken before the
    // legacy draw.
    const uint32_t* under = nullptr;

    HdBlendMode mode = HdBlendMode::Tint;

    // Tint: the tint colour and the grey weights (out of 10) of the legacy
    // grey table (_commonGrayTable 3/6/1, _glassGrayTable 1/5/4).
    uint32_t tint = 0;
    int greyR = 3;
    int greyG = 6;
    int greyB = 1;

    // Mask: aligned with (x, y).
    const unsigned char* mask = nullptr;
    int maskPitch = 0;
};

// For every non-transparent frame pixel of the drawn part the blended HD
// block becomes the layer's pixel, with the buffer's (new) index as shadow.
// Pixels with no colour to blend with, colour-cycling buffer indices and
// fully transparent HD blocks are made invalid (legacy shows).
void hdLayerDrawBlended(HdLayer* layer, const unsigned char* buffer, int bufferPitch, const HdBlendDraw& draw);

// After legacy code changed some pixels of (x, y, width, height) (e.g. an
// outline drawn around an object), `before` being a copy of the buffer
// there (row pitch `width`) and `under` its hdLayerCapture: the changed
// pixels become lerp(under, tint, alpha256 / 256) in HD (a paletted, i.e.
// translucent, outline over HD pixels), or invalid (legacy) when
// alpha256 is 0.
void hdLayerTintChanged(HdLayer* layer, const unsigned char* buffer, int bufferPitch,
    int x, int y, int width, int height, const unsigned char* before, const uint32_t* under, uint32_t tint, int alpha256);

// Marks a rectangle (logical, clipped to the layer) as having no HD pixels.
void hdLayerInvalidateRect(HdLayer* layer, int x, int y, int width, int height);

// Same, only where `mask` (aligned with the rectangle) is not 0; used where
// legacy art (objects) is drawn over HD pixels, so it stays legacy even where
// its palette index happens to equal the shadow.
void hdLayerInvalidateMasked(HdLayer* layer, int x, int y, int width, int height, const unsigned char* mask, int maskPitch);

// Moves the layer's content like a scroll of the window buffer: afterwards
// logical pixel (x, y) holds what was at (x + dx, y + dy). Pixels with no
// source are invalid.
void hdLayerScroll(HdLayer* layer, int dx, int dy);

// Whether the HD block of logical pixel (x, y) is shown for buffer value `index`.
inline bool hdLayerShows(const HdLayer* layer, int x, int y, unsigned char index)
{
    size_t offset = static_cast<size_t>(y) * layer->width + x;
    return layer->valid[offset] != 0 && layer->shadow[offset] == index;
}

// Composites a rectangle of a window into a screen-sized staging area: for
// every logical pixel, `mask` becomes 1 and the 2x2 HD block is copied when
// the layer shows there, else `mask` becomes 0. For transparent windows
// (`transparent`), buffer index 0 is see-through and leaves mask/rgb as they
// are, mirroring blitBufferToBufferTrans. `layer` may be null (no HD).
// (srcX, srcY) is in window coordinates; mask/rgb point at the destination's
// top-left (logical and 2x coordinates respectively). Where the layer's text
// overlay shows, the text is composited over the HD pixel or the base
// palette colour (`basePalette8`; the overlay is ignored when it is null).
void hdComposeRect(const HdLayer* layer, const unsigned char* buffer, int bufferPitch,
    int srcX, int srcY, int width, int height,
    unsigned char* mask, int maskPitch, uint32_t* rgb, int rgbPitch, bool transparent,
    const unsigned char* basePalette8 = nullptr);

// Per-index colour mapping from the base palette to the presented palette.
// For every channel, with base value b and presented value p:
//   p == b: unchanged;  p < b: h * p / b (fades to black, darker gamma);
//   p > b: h + (p - b) * (255 - h) / (255 - b) (fades to white, brighter).
// An HD colour equal to the base colour therefore maps exactly to the
// presented colour, and the mapping is monotonic for every other value.
struct HdModulation {
    bool identity[256];
    unsigned char lut[256][3][256];
};

void hdModulationBuild(HdModulation* modulation, const unsigned char* basePalette8, const unsigned char* presentedPalette8);
void hdModulationUpdate(HdModulation* modulation, const unsigned char* basePalette8, const unsigned char* presentedPalette8, int start, int count);

inline uint32_t hdModulate(const HdModulation* modulation, uint32_t rgb, unsigned char index)
{
    if (modulation->identity[index]) {
        return rgb & 0xFFFFFF;
    }

    const unsigned char (*lut)[256] = modulation->lut[index];
    return (static_cast<uint32_t>(lut[0][(rgb >> 16) & 0xFF]) << 16)
        | (static_cast<uint32_t>(lut[1][(rgb >> 8) & 0xFF]) << 8)
        | lut[2][rgb & 0xFF];
}

// Resamples `src` to `width` x `height` (bilinear on premultiplied colour).
void hdImageResample(const HdImage& src, int width, int height, HdImage* dest);

// Builds a 2x image from an 8-bit frame: every pixel doubled, index 0
// transparent. Used when no HD art exists for a frame.
void hdImageFromIndexed(const unsigned char* data, int width, int height, int pitch, const unsigned char* palette8, bool zeroIsTransparent, HdImage* dest);

} // namespace fallout

#endif /* FALLOUT_HD_LAYER_H_ */
