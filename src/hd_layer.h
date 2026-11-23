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

struct HdLayer {
    // Logical size (the window's size); `pixels` is kHdScale times larger in
    // each direction and holds opaque 0x00RRGGBB colours.
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;
    std::vector<unsigned char> shadow;
    std::vector<unsigned char> valid;
};

void hdLayerInit(HdLayer* layer, int width, int height);
void hdLayerClear(HdLayer* layer);

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
// top-left (logical and 2x coordinates respectively).
void hdComposeRect(const HdLayer* layer, const unsigned char* buffer, int bufferPitch,
    int srcX, int srcY, int width, int height,
    unsigned char* mask, int maskPitch, uint32_t* rgb, int rgbPitch, bool transparent);

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
