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
