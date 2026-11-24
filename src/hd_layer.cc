#include "hd_layer.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>

namespace fallout {

void hdLayerInit(HdLayer* layer, int width, int height)
{
    layer->width = std::max(width, 0);
    layer->height = std::max(height, 0);
    size_t logical = static_cast<size_t>(layer->width) * layer->height;
    layer->pixels.assign(logical * kHdScale * kHdScale, 0);
    layer->shadow.assign(logical, 0);
    layer->valid.assign(logical, 0);
    layer->textPixels.clear();
    layer->textGuard.clear();
    layer->textShadow.clear();
    layer->textUnder.clear();
    layer->textTop = 0;
    layer->textBottom = 0;
}

static void clearTextRows(HdLayer* layer, int top, int bottom)
{
    if (top < bottom) {
        std::fill(layer->textGuard.begin() + static_cast<size_t>(top) * layer->width,
            layer->textGuard.begin() + static_cast<size_t>(bottom) * layer->width, 0);
    }
}

void hdLayerClear(HdLayer* layer)
{
    std::fill(layer->valid.begin(), layer->valid.end(), 0);
    clearTextRows(layer, layer->textTop, layer->textBottom);
    layer->textTop = 0;
    layer->textBottom = 0;
}

static inline uint32_t paletteColor(const unsigned char* palette8, unsigned char index)
{
    return (static_cast<uint32_t>(palette8[index * 3]) << 16)
        | (static_cast<uint32_t>(palette8[index * 3 + 1]) << 8)
        | palette8[index * 3 + 2];
}

// Premultiplied `src` over opaque `under`.
static inline uint32_t premultipliedOver(uint32_t src, uint32_t under)
{
    uint32_t alpha = src >> 24;
    if (alpha == 0) {
        return under & 0xFFFFFF;
    }
    if (alpha == 255) {
        return src & 0xFFFFFF;
    }

    uint32_t inverse = 255 - alpha;
    uint32_t r = ((src >> 16) & 0xFF) + (((under >> 16) & 0xFF) * inverse + 127) / 255;
    uint32_t g = ((src >> 8) & 0xFF) + (((under >> 8) & 0xFF) * inverse + 127) / 255;
    uint32_t b = (src & 0xFF) + ((under & 0xFF) * inverse + 127) / 255;
    return (std::min(r, 255u) << 16) | (std::min(g, 255u) << 8) | std::min(b, 255u);
}

// Premultiplied `src` over premultiplied `under`.
static inline uint32_t premultipliedOverAlpha(uint32_t src, uint32_t under)
{
    uint32_t inverse = 255 - (src >> 24);
    uint32_t result = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        uint32_t value = ((src >> shift) & 0xFF) + (((under >> shift) & 0xFF) * inverse + 127) / 255;
        result |= std::min(value, 255u) << shift;
    }
    return result;
}

static void ensureTextOverlay(HdLayer* layer)
{
    size_t logical = static_cast<size_t>(layer->width) * layer->height;
    if (layer->textGuard.size() != logical) {
        layer->textPixels.assign(logical * kHdScale * kHdScale, 0);
        layer->textGuard.assign(logical, 0);
        layer->textShadow.assign(logical, 0);
        layer->textUnder.assign(logical, 0);
        layer->textTop = 0;
        layer->textBottom = 0;
    }
}

void hdLayerDrawText(HdLayer* layer, const unsigned char* buffer, int bufferPitch, const HdTextDraw& draw)
{
    if (layer == nullptr || draw.mask == nullptr || draw.pixels == nullptr || draw.before == nullptr || draw.width <= 0 || draw.height <= 0) {
        return;
    }

    int left = std::max(draw.x, 0);
    int top = std::max(draw.y, 0);
    int right = std::min(draw.x + draw.width, layer->width);
    int bottom = std::min(draw.y + draw.height, layer->height);
    if (left >= right || top >= bottom) {
        return;
    }

    ensureTextOverlay(layer);
    if (layer->textTop >= layer->textBottom) {
        layer->textTop = top;
        layer->textBottom = bottom;
    } else {
        layer->textTop = std::min(layer->textTop, top);
        layer->textBottom = std::max(layer->textBottom, bottom);
    }

    const int layerPitch = layer->width * kHdScale;
    const int drawPitch = draw.width * kHdScale;

    // Whether a pixel of the draw becomes a text pixel (so fringe pixels can
    // depend on it), and whether the overlay showed at each pixel before.
    auto isTextPixel = [&](int dx, int dy) {
        if (dx < 0 || dy < 0 || dx >= draw.width || dy >= draw.height) {
            return false;
        }
        int lx = draw.x + dx;
        int ly = draw.y + dy;
        if (lx < left || ly < top || lx >= right || ly >= bottom) {
            return false;
        }
        return draw.mask[static_cast<size_t>(dy) * draw.width + dx] != 0
            && !hdIsCyclingIndex(buffer[static_cast<size_t>(ly) * bufferPitch + lx]);
    };

    std::vector<unsigned char> showed(static_cast<size_t>(right - left) * (bottom - top));
    for (int ly = top; ly < bottom; ly++) {
        for (int lx = left; lx < right; lx++) {
            showed[static_cast<size_t>(ly - top) * (right - left) + (lx - left)] = hdLayerTextShows(layer, buffer, bufferPitch, lx, ly);
        }
    }

    for (int ly = top; ly < bottom; ly++) {
        for (int lx = left; lx < right; lx++) {
            int dx = lx - draw.x;
            int dy = ly - draw.y;
            size_t offset = static_cast<size_t>(ly) * layer->width + lx;
            const uint32_t* src = draw.pixels + static_cast<size_t>(dy) * kHdScale * drawPitch + dx * kHdScale;
            uint32_t* dest = layer->textPixels.data() + static_cast<size_t>(ly) * kHdScale * layerPitch + lx * kHdScale;
            unsigned char after = buffer[static_cast<size_t>(ly) * bufferPitch + lx];

            if (draw.mask[static_cast<size_t>(dy) * draw.width + dx] != 0) {
                if (hdIsCyclingIndex(after)) {
                    layer->textGuard[offset] = 0;
                    continue;
                }

                unsigned char under = draw.before[static_cast<size_t>(dy) * draw.width + dx];
                if (under == after && (layer->textGuard[offset] & kHdTextSelf) != 0 && layer->textShadow[offset] == after) {
                    // The same text drawn again over itself: it is still
                    // over what it was first drawn over.
                    under = layer->textUnder[offset];
                }

                for (int sy = 0; sy < kHdScale; sy++) {
                    for (int sx = 0; sx < kHdScale; sx++) {
                        dest[sy * layerPitch + sx] = src[sy * drawPitch + sx];
                    }
                }
                layer->textGuard[offset] = kHdTextSelf;
                layer->textShadow[offset] = after;
                layer->textUnder[offset] = under;
                continue;
            }

            uint32_t alphaOr = (src[0] | src[1] | src[drawPitch] | src[drawPitch + 1]) >> 24;
            if (alphaOr == 0 || hdIsCyclingIndex(after)) {
                continue;
            }

            uint16_t depends = 0;
            for (int bit = 0; bit < 8; bit++) {
                if (isTextPixel(dx + kHdTextNeighbours[bit].dx, dy + kHdTextNeighbours[bit].dy)) {
                    depends |= static_cast<uint16_t>(1 << bit);
                }
            }
            if (depends == 0) {
                continue;
            }

            uint16_t guard = layer->textGuard[offset];
            bool merge = showed[static_cast<size_t>(ly - top) * (right - left) + (lx - left)] != 0;
            if (merge && (guard & kHdTextSelf) != 0) {
                // A text pixel of other text that is still shown wins.
                continue;
            }

            for (int sy = 0; sy < kHdScale; sy++) {
                for (int sx = 0; sx < kHdScale; sx++) {
                    uint32_t value = src[sy * drawPitch + sx];
                    dest[sy * layerPitch + sx] = merge ? premultipliedOverAlpha(value, dest[sy * layerPitch + sx]) : value;
                }
            }
            layer->textGuard[offset] = merge ? static_cast<uint16_t>(guard | depends) : depends;
        }
    }
}

void hdLayerTextBlock(const HdLayer* layer, const unsigned char* buffer, int bufferPitch, int x, int y,
    const unsigned char* basePalette8, uint32_t* block, int blockPitch)
{
    size_t offset = static_cast<size_t>(y) * layer->width + x;
    unsigned char baseIndex = (layer->textGuard[offset] & kHdTextSelf) != 0
        ? layer->textUnder[offset]
        : buffer[static_cast<size_t>(y) * bufferPitch + x];
    bool layerShows = layer->valid[offset] != 0 && layer->shadow[offset] == baseIndex;
    uint32_t flat = paletteColor(basePalette8, baseIndex);

    const int layerPitch = layer->width * kHdScale;
    const uint32_t* text = layer->textPixels.data() + static_cast<size_t>(y) * kHdScale * layerPitch + x * kHdScale;
    const uint32_t* hd = layer->pixels.data() + static_cast<size_t>(y) * kHdScale * layerPitch + x * kHdScale;
    for (int sy = 0; sy < kHdScale; sy++) {
        for (int sx = 0; sx < kHdScale; sx++) {
            uint32_t under = layerShows ? hd[sy * layerPitch + sx] : flat;
            block[sy * blockPitch + sx] = premultipliedOver(text[sy * layerPitch + sx], under);
        }
    }
}

void hdLayerBakeText(HdLayer* layer, const unsigned char* buffer, int bufferPitch, const unsigned char* basePalette8)
{
    if (layer == nullptr || layer->textGuard.empty()) {
        return;
    }

    const int layerPitch = layer->width * kHdScale;
    std::vector<uint32_t> blocks;
    std::vector<size_t> offsets;
    for (int y = layer->textTop; y < layer->textBottom; y++) {
        for (int x = 0; x < layer->width; x++) {
            if (hdLayerTextShows(layer, buffer, bufferPitch, x, y)) {
                uint32_t block[kHdScale * kHdScale];
                hdLayerTextBlock(layer, buffer, bufferPitch, x, y, basePalette8, block, kHdScale);
                blocks.insert(blocks.end(), block, block + kHdScale * kHdScale);
                offsets.push_back(static_cast<size_t>(y) * layer->width + x);
            }
        }
    }

    // Written after all blocks are made: a block reads the layer's pixels.
    for (size_t index = 0; index < offsets.size(); index++) {
        size_t offset = offsets[index];
        int x = static_cast<int>(offset % layer->width);
        int y = static_cast<int>(offset / layer->width);
        uint32_t* dest = layer->pixels.data() + static_cast<size_t>(y) * kHdScale * layerPitch + x * kHdScale;
        for (int sy = 0; sy < kHdScale; sy++) {
            for (int sx = 0; sx < kHdScale; sx++) {
                dest[sy * layerPitch + sx] = blocks[index * kHdScale * kHdScale + sy * kHdScale + sx];
            }
        }
        layer->shadow[offset] = buffer[static_cast<size_t>(y) * bufferPitch + x];
        layer->valid[offset] = 1;
    }

    clearTextRows(layer, layer->textTop, layer->textBottom);
    layer->textTop = 0;
    layer->textBottom = 0;
}

static inline uint32_t blendOver(uint32_t src, uint32_t under)
{
    uint32_t alpha = src >> 24;
    if (alpha == 255) {
        return src & 0xFFFFFF;
    }
    if (alpha == 0) {
        return under & 0xFFFFFF;
    }

    uint32_t inverse = 255 - alpha;
    uint32_t r = (((src >> 16) & 0xFF) * alpha + ((under >> 16) & 0xFF) * inverse + 127) / 255;
    uint32_t g = (((src >> 8) & 0xFF) * alpha + ((under >> 8) & 0xFF) * inverse + 127) / 255;
    uint32_t b = ((src & 0xFF) * alpha + (under & 0xFF) * inverse + 127) / 255;
    return (r << 16) | (g << 8) | b;
}

void hdLayerDrawImage(HdLayer* layer, const unsigned char* buffer, int bufferPitch,
    int x, int y, int width, int height, const HdImage& image, const unsigned char* basePalette8)
{
    hdLayerDrawImageEx(layer, buffer, bufferPitch, x, y, width, height, image, basePalette8, HdDrawOptions());
}

void hdLayerDrawImageEx(HdLayer* layer, const unsigned char* buffer, int bufferPitch,
    int x, int y, int width, int height, const HdImage& image, const unsigned char* basePalette8,
    const HdDrawOptions& options)
{
    if (layer == nullptr || width <= 0 || height <= 0 || image.width <= 0 || image.height <= 0) {
        return;
    }

    int left = std::max({ x, 0, options.clipLeft });
    int top = std::max({ y, 0, options.clipTop });
    int right = std::min({ x + width, layer->width, options.clipRight });
    int bottom = std::min({ y + height, layer->height, options.clipBottom });
    if (left >= right || top >= bottom) {
        return;
    }

    const HdImage* source = &image;
    HdImage resampled;
    if (image.width != width * kHdScale || image.height != height * kHdScale) {
        hdImageResample(image, width * kHdScale, height * kHdScale, &resampled);
        source = &resampled;
    }

    const int layerPitch = layer->width * kHdScale;

    for (int ly = top; ly < bottom; ly++) {
        for (int lx = left; lx < right; lx++) {
            size_t offset = static_cast<size_t>(ly) * layer->width + lx;
            unsigned char index = buffer[static_cast<size_t>(ly) * bufferPitch + lx];

            if (hdIsCyclingIndex(index)) {
                layer->valid[offset] = 0;
                continue;
            }

            if (options.source != nullptr && options.source[static_cast<size_t>(ly - y) * options.sourcePitch + (lx - x)] != index) {
                // Something else was drawn here (text, numbers): legacy.
                continue;
            }

            const uint32_t* src = source->pixels.data() + static_cast<size_t>(ly - y) * kHdScale * source->width + (lx - x) * kHdScale;
            bool anyVisible = false;
            for (int sy = 0; sy < kHdScale && !anyVisible; sy++) {
                for (int sx = 0; sx < kHdScale; sx++) {
                    if ((src[sy * source->width + sx] >> 24) != 0) {
                        anyVisible = true;
                        break;
                    }
                }
            }

            if (!anyVisible) {
                continue;
            }

            bool layerShows = layer->valid[offset] != 0 && layer->shadow[offset] == index;
            uint32_t legacy = (static_cast<uint32_t>(basePalette8[index * 3]) << 16)
                | (static_cast<uint32_t>(basePalette8[index * 3 + 1]) << 8)
                | basePalette8[index * 3 + 2];

            uint32_t* dest = layer->pixels.data() + static_cast<size_t>(ly) * kHdScale * layerPitch + lx * kHdScale;
            for (int sy = 0; sy < kHdScale; sy++) {
                for (int sx = 0; sx < kHdScale; sx++) {
                    uint32_t under = layerShows ? dest[sy * layerPitch + sx] : legacy;
                    dest[sy * layerPitch + sx] = blendOver(src[sy * source->width + sx], under);
                }
            }

            layer->shadow[offset] = index;
            layer->valid[offset] = 1;
        }
    }
}

void hdLayerDrawLit(HdLayer* layer, const unsigned char* buffer, int bufferPitch, const HdLitDraw& draw, const unsigned char* basePalette8)
{
    if (layer == nullptr || draw.frame == nullptr || draw.image == nullptr) {
        return;
    }

    const HdImage& image = *draw.image;
    int left = std::max(draw.x, 0);
    int top = std::max(draw.y, 0);
    int right = std::min(draw.x + draw.width, layer->width);
    int bottom = std::min(draw.y + draw.height, layer->height);
    if (left >= right || top >= bottom) {
        return;
    }

    // The frame rectangle must lie inside the HD image.
    if (draw.frameX + (left - draw.x) < 0 || draw.frameY + (top - draw.y) < 0
        || (draw.frameX + (right - draw.x)) * kHdScale > image.width
        || (draw.frameY + (bottom - draw.y)) * kHdScale > image.height) {
        return;
    }

    // Locals, so the byte stores to valid/shadow do not force reloads.
    const int layerPitch = layer->width * kHdScale;
    const int imageWidth = image.width;
    const uint32_t* imagePixels = image.pixels.data();
    uint32_t* layerPixels = layer->pixels.data();
    unsigned char* valid = layer->valid.data();
    unsigned char* shadow = layer->shadow.data();
    const unsigned char* expected = draw.expected;
    const int constantIntensity = draw.constantIntensity;
    const int frameOffsetX = draw.frameX - draw.x;

    for (int ly = top; ly < bottom; ly++) {
        int fy = draw.frameY + (ly - draw.y);
        const unsigned char* frameRow = draw.frame + static_cast<size_t>(fy) * draw.framePitch;
        const unsigned char* bufferRow = buffer + static_cast<size_t>(ly) * bufferPitch;
        const int* intensityRow = draw.intensity != nullptr
            ? draw.intensity + static_cast<size_t>(ly - draw.y) * draw.intensityPitch
            : nullptr;
        const uint32_t* imageRow = imagePixels + static_cast<size_t>(fy) * kHdScale * imageWidth;
        uint32_t* layerRow = layerPixels + static_cast<size_t>(ly) * kHdScale * layerPitch;
        unsigned char* validRow = valid + static_cast<size_t>(ly) * layer->width;
        unsigned char* shadowRow = shadow + static_cast<size_t>(ly) * layer->width;

        for (int lx = left; lx < right; lx++) {
            int fx = lx + frameOffsetX;
            unsigned char frameIndex = frameRow[fx];
            if (frameIndex == 0) {
                continue;
            }

            unsigned char index = bufferRow[lx];
            if (expected != nullptr && expected[frameIndex] != index) {
                continue;
            }

            if (hdIsCyclingIndex(index)) {
                validRow[lx] = 0;
                continue;
            }

            const uint32_t* src = imageRow + fx * kHdScale;
            uint32_t* dest = layerRow + lx * kHdScale;
            uint32_t s00 = src[0];
            uint32_t s01 = src[1];
            uint32_t s10 = src[imageWidth];
            uint32_t s11 = src[imageWidth + 1];
            uint32_t alphaAnd = s00 & s01 & s10 & s11;
            uint32_t alphaOr = s00 | s01 | s10 | s11;
            if ((alphaOr >> 24) == 0) {
                validRow[lx] = 0;
                continue;
            }

            int intensity = intensityRow != nullptr ? intensityRow[lx - draw.x] : constantIntensity;

            if ((alphaAnd >> 24) == 255) {
                dest[0] = hdLight(s00, intensity);
                dest[1] = hdLight(s01, intensity);
                dest[layerPitch] = hdLight(s10, intensity);
                dest[layerPitch + 1] = hdLight(s11, intensity);
            } else {
                bool layerShows = validRow[lx] != 0 && shadowRow[lx] == index;
                uint32_t legacy = (static_cast<uint32_t>(basePalette8[index * 3]) << 16)
                    | (static_cast<uint32_t>(basePalette8[index * 3 + 1]) << 8)
                    | basePalette8[index * 3 + 2];
                for (int sy = 0; sy < kHdScale; sy++) {
                    for (int sx = 0; sx < kHdScale; sx++) {
                        uint32_t pixel = src[sy * imageWidth + sx];
                        uint32_t lit = hdLight(pixel, intensity) | (pixel & 0xFF000000u);
                        uint32_t under = layerShows ? dest[sy * layerPitch + sx] : legacy;
                        dest[sy * layerPitch + sx] = blendOver(lit, under);
                    }
                }
            }

            shadowRow[lx] = index;
            validRow[lx] = 1;
        }
    }
}

void hdLayerCapture(const HdLayer* layer, const unsigned char* buffer, int bufferPitch,
    int x, int y, int width, int height, const unsigned char* basePalette8, std::vector<uint32_t>* under)
{
    width = std::max(width, 0);
    height = std::max(height, 0);
    const int pitch = width * kHdScale;
    under->assign(static_cast<size_t>(pitch) * height * kHdScale, 0);
    if (layer == nullptr) {
        return;
    }

    const int layerPitch = layer->width * kHdScale;
    for (int row = 0; row < height; row++) {
        int ly = y + row;
        if (ly < 0 || ly >= layer->height) {
            continue;
        }
        uint32_t* out = under->data() + static_cast<size_t>(row) * kHdScale * pitch;
        for (int column = 0; column < width; column++) {
            int lx = x + column;
            if (lx < 0 || lx >= layer->width) {
                continue;
            }
            unsigned char index = buffer[static_cast<size_t>(ly) * bufferPitch + lx];
            if (hdIsCyclingIndex(index)) {
                continue;
            }
            uint32_t* dest = out + column * kHdScale;
            if (hdLayerShows(layer, lx, ly, index)) {
                const uint32_t* src = layer->pixels.data() + static_cast<size_t>(ly) * kHdScale * layerPitch + lx * kHdScale;
                for (int sy = 0; sy < kHdScale; sy++) {
                    for (int sx = 0; sx < kHdScale; sx++) {
                        dest[sy * pitch + sx] = 0xFF000000u | src[sy * layerPitch + sx];
                    }
                }
            } else {
                uint32_t colour = 0xFF000000u
                    | (static_cast<uint32_t>(basePalette8[index * 3]) << 16)
                    | (static_cast<uint32_t>(basePalette8[index * 3 + 1]) << 8)
                    | basePalette8[index * 3 + 2];
                for (int sy = 0; sy < kHdScale; sy++) {
                    for (int sx = 0; sx < kHdScale; sx++) {
                        dest[sy * pitch + sx] = colour;
                    }
                }
            }
        }
    }
}

void hdLayerDrawOver(HdLayer* layer, const unsigned char* buffer, int bufferPitch,
    int x, int y, int width, int height, const HdImage& image, const uint32_t* under, const unsigned char* basePalette8)
{
    if (layer == nullptr || under == nullptr || width <= 0 || height <= 0 || image.width <= 0 || image.height <= 0) {
        return;
    }

    const HdImage* source = &image;
    HdImage resampled;
    if (image.width != width * kHdScale || image.height != height * kHdScale) {
        hdImageResample(image, width * kHdScale, height * kHdScale, &resampled);
        source = &resampled;
    }

    const int left = std::max(x, 0);
    const int top = std::max(y, 0);
    const int right = std::min(x + width, layer->width);
    const int bottom = std::min(y + height, layer->height);
    const int layerPitch = layer->width * kHdScale;
    const int underPitch = width * kHdScale;

    for (int ly = top; ly < bottom; ly++) {
        for (int lx = left; lx < right; lx++) {
            size_t offset = static_cast<size_t>(ly) * layer->width + lx;
            unsigned char index = buffer[static_cast<size_t>(ly) * bufferPitch + lx];
            if (hdIsCyclingIndex(index)) {
                layer->valid[offset] = 0;
                continue;
            }

            const uint32_t* src = source->pixels.data() + static_cast<size_t>(ly - y) * kHdScale * source->width + (lx - x) * kHdScale;
            bool anyVisible = false;
            for (int sy = 0; sy < kHdScale && !anyVisible; sy++) {
                for (int sx = 0; sx < kHdScale; sx++) {
                    if ((src[sy * source->width + sx] >> 24) != 0) {
                        anyVisible = true;
                        break;
                    }
                }
            }
            if (!anyVisible) {
                continue;
            }

            const uint32_t* below = under + static_cast<size_t>(ly - y) * kHdScale * underPitch + (lx - x) * kHdScale;
            uint32_t legacy = (static_cast<uint32_t>(basePalette8[index * 3]) << 16)
                | (static_cast<uint32_t>(basePalette8[index * 3 + 1]) << 8)
                | basePalette8[index * 3 + 2];
            uint32_t* dest = layer->pixels.data() + static_cast<size_t>(ly) * kHdScale * layerPitch + lx * kHdScale;
            for (int sy = 0; sy < kHdScale; sy++) {
                for (int sx = 0; sx < kHdScale; sx++) {
                    uint32_t colour = below[sy * underPitch + sx];
                    // No captured colour (a colour-cycling index was there):
                    // blend over the legacy colour now in the buffer.
                    uint32_t base = (colour >> 24) != 0 ? colour : legacy;
                    dest[sy * layerPitch + sx] = blendOver(src[sy * source->width + sx], base);
                }
            }

            layer->shadow[offset] = index;
            layer->valid[offset] = 1;
        }
    }
}

// lerp(a, b, t / 256) per channel, t in 0..256.
static inline uint32_t mixRgb(uint32_t a, uint32_t b, int t)
{
    uint32_t weight = static_cast<uint32_t>(t);
    uint32_t inverse = 256 - weight;
    uint32_t r = (((a >> 16) & 0xFF) * inverse + ((b >> 16) & 0xFF) * weight + 128) >> 8;
    uint32_t g = (((a >> 8) & 0xFF) * inverse + ((b >> 8) & 0xFF) * weight + 128) >> 8;
    uint32_t bl = ((a & 0xFF) * inverse + (b & 0xFF) * weight + 128) >> 8;
    return (r << 16) | (g << 8) | bl;
}

void hdLayerDrawBlended(HdLayer* layer, const unsigned char* buffer, int bufferPitch, const HdBlendDraw& draw)
{
    if (layer == nullptr || draw.frame == nullptr || draw.image == nullptr || draw.under == nullptr
        || (draw.mode == HdBlendMode::Mask && draw.mask == nullptr)) {
        return;
    }

    const HdImage& image = *draw.image;
    int left = std::max(draw.x, 0);
    int top = std::max(draw.y, 0);
    int right = std::min(draw.x + draw.width, layer->width);
    int bottom = std::min(draw.y + draw.height, layer->height);
    if (left >= right || top >= bottom) {
        return;
    }
    if (draw.frameX + (left - draw.x) < 0 || draw.frameY + (top - draw.y) < 0
        || (draw.frameX + (right - draw.x)) * kHdScale > image.width
        || (draw.frameY + (bottom - draw.y)) * kHdScale > image.height) {
        return;
    }

    const int layerPitch = layer->width * kHdScale;
    const int underPitch = draw.width * kHdScale;
    const int greyTotal = std::max(draw.greyR + draw.greyG + draw.greyB, 1);

    for (int ly = top; ly < bottom; ly++) {
        int fy = draw.frameY + (ly - draw.y);
        const unsigned char* frameRow = draw.frame + static_cast<size_t>(fy) * draw.framePitch;
        const unsigned char* bufferRow = buffer + static_cast<size_t>(ly) * bufferPitch;
        const uint32_t* imageRow = image.pixels.data() + static_cast<size_t>(fy) * kHdScale * image.width;
        const uint32_t* underRow = draw.under + static_cast<size_t>(ly - draw.y) * kHdScale * underPitch;
        uint32_t* layerRow = layer->pixels.data() + static_cast<size_t>(ly) * kHdScale * layerPitch;
        unsigned char* validRow = layer->valid.data() + static_cast<size_t>(ly) * layer->width;
        unsigned char* shadowRow = layer->shadow.data() + static_cast<size_t>(ly) * layer->width;
        const unsigned char* maskRow = draw.mask != nullptr
            ? draw.mask + static_cast<size_t>(ly - draw.y) * draw.maskPitch
            : nullptr;

        for (int lx = left; lx < right; lx++) {
            int fx = draw.frameX + (lx - draw.x);
            if (frameRow[fx] == 0) {
                continue;
            }

            unsigned char index = bufferRow[lx];
            const uint32_t* src = imageRow + fx * kHdScale;
            const uint32_t* under = underRow + (lx - draw.x) * kHdScale;
            uint32_t alphaOr = src[0] | src[1] | src[image.width] | src[image.width + 1];
            int m = maskRow != nullptr ? maskRow[lx - draw.x] : 0;
            bool needsUnder = draw.mode == HdBlendMode::Tint || m != 0;
            if (hdIsCyclingIndex(index) || (alphaOr >> 24) == 0 || (needsUnder && (under[0] >> 24) == 0)) {
                validRow[lx] = 0;
                continue;
            }

            uint32_t* dest = layerRow + lx * kHdScale;
            for (int sy = 0; sy < kHdScale; sy++) {
                for (int sx = 0; sx < kHdScale; sx++) {
                    uint32_t pixel = src[sy * image.width + sx];
                    uint32_t below = under[sy * underPitch + sx];
                    uint32_t result;
                    if (draw.mode == HdBlendMode::Tint) {
                        // Legacy: grey5 = weighted 5-bit grey, level = grey5 >> 2
                        // (0..7), alpha = level / 7; here without the steps
                        // (centred on them).
                        int grey8 = (draw.greyR * static_cast<int>((pixel >> 16) & 0xFF)
                                        + draw.greyG * static_cast<int>((pixel >> 8) & 0xFF)
                                        + draw.greyB * static_cast<int>(pixel & 0xFF))
                            / greyTotal;
                        int alpha = std::clamp((grey8 * 31 * 256 / 255 - 384) / 28, 0, 256);
                        if ((pixel >> 24) == 0) {
                            alpha = 0;
                        }
                        result = hdLight(mixRgb(below, draw.tint, alpha), draw.intensity);
                    } else {
                        uint32_t lit = hdLight(pixel, draw.intensity);
                        if ((pixel >> 24) != 255) {
                            lit = blendOver(lit | (pixel & 0xFF000000u), below);
                        }
                        result = m != 0 ? mixRgb(below, lit, std::min(m, 128) * 2) : lit;
                    }
                    dest[sy * layerPitch + sx] = result & 0xFFFFFF;
                }
            }
            shadowRow[lx] = index;
            validRow[lx] = 1;
        }
    }
}

void hdLayerTintChanged(HdLayer* layer, const unsigned char* buffer, int bufferPitch,
    int x, int y, int width, int height, const unsigned char* before, const uint32_t* under, uint32_t tint, int alpha256)
{
    if (layer == nullptr || before == nullptr) {
        return;
    }

    const int layerPitch = layer->width * kHdScale;
    const int underPitch = width * kHdScale;
    for (int row = std::max(-y, 0); row < height && y + row < layer->height; row++) {
        int ly = y + row;
        for (int column = std::max(-x, 0); column < width && x + column < layer->width; column++) {
            int lx = x + column;
            unsigned char index = buffer[static_cast<size_t>(ly) * bufferPitch + lx];
            if (index == before[static_cast<size_t>(row) * width + column]) {
                continue;
            }

            size_t offset = static_cast<size_t>(ly) * layer->width + lx;
            const uint32_t* below = under != nullptr ? under + static_cast<size_t>(row) * kHdScale * underPitch + column * kHdScale : nullptr;
            if (alpha256 <= 0 || below == nullptr || (below[0] >> 24) == 0 || hdIsCyclingIndex(index)) {
                layer->valid[offset] = 0;
                continue;
            }

            uint32_t* dest = layer->pixels.data() + static_cast<size_t>(ly) * kHdScale * layerPitch + lx * kHdScale;
            for (int sy = 0; sy < kHdScale; sy++) {
                for (int sx = 0; sx < kHdScale; sx++) {
                    dest[sy * layerPitch + sx] = mixRgb(below[sy * underPitch + sx], tint, std::min(alpha256, 256));
                }
            }
            layer->shadow[offset] = index;
            layer->valid[offset] = 1;
        }
    }
}

void hdLayerInvalidateRect(HdLayer* layer, int x, int y, int width, int height)
{
    if (layer == nullptr) {
        return;
    }

    int left = std::max(x, 0);
    int top = std::max(y, 0);
    int right = std::min(x + width, layer->width);
    int bottom = std::min(y + height, layer->height);
    for (int ly = top; ly < bottom; ly++) {
        memset(layer->valid.data() + static_cast<size_t>(ly) * layer->width + left, 0, std::max(right - left, 0));
    }
}

void hdLayerClearTextRect(HdLayer* layer, int x, int y, int width, int height)
{
    if (layer == nullptr || layer->textGuard.empty()) {
        return;
    }

    int left = std::max(x, 0);
    int top = std::max({ y, 0, layer->textTop });
    int right = std::min(x + width, layer->width);
    int bottom = std::min({ y + height, layer->height, layer->textBottom });
    for (int ly = top; ly < bottom; ly++) {
        std::fill_n(layer->textGuard.data() + static_cast<size_t>(ly) * layer->width + left, std::max(right - left, 0), uint16_t { 0 });
    }
}

void hdLayerInvalidateMasked(HdLayer* layer, int x, int y, int width, int height, const unsigned char* mask, int maskPitch)
{
    if (layer == nullptr || mask == nullptr) {
        return;
    }

    int left = std::max(x, 0);
    int top = std::max(y, 0);
    int right = std::min(x + width, layer->width);
    int bottom = std::min(y + height, layer->height);
    for (int ly = top; ly < bottom; ly++) {
        const unsigned char* maskRow = mask + static_cast<size_t>(ly - y) * maskPitch;
        unsigned char* valid = layer->valid.data() + static_cast<size_t>(ly) * layer->width;
        for (int lx = left; lx < right; lx++) {
            if (maskRow[lx - x] != 0) {
                valid[lx] = 0;
            }
        }
    }
}

void hdLayerScroll(HdLayer* layer, int dx, int dy)
{
    if (layer == nullptr || (dx == 0 && dy == 0)) {
        return;
    }

    int width = layer->width;
    int height = layer->height;
    if (dx <= -width || dx >= width || dy <= -height || dy >= height) {
        hdLayerClear(layer);
        return;
    }

    // Destination columns [destLeft, destLeft + count) take source columns
    // shifted by dx; rows likewise.
    int destLeft = std::max(-dx, 0);
    int count = width - std::abs(dx);
    int pitch = width * kHdScale;

    // Text rows move like the others, but only the rows that hold text.
    int oldTop = layer->textTop;
    int oldBottom = layer->textBottom;
    int newTop = std::max(oldTop - dy, 0);
    int newBottom = std::min(oldBottom - dy, height);
    if (newTop >= newBottom) {
        newTop = 0;
        newBottom = 0;
    }

    auto moveRow = [&](int destY) {
        int srcY = destY + dy;
        unsigned char* valid = layer->valid.data() + static_cast<size_t>(destY) * width;
        bool textRow = destY >= newTop && destY < newBottom;
        if (!textRow && destY >= oldTop && destY < oldBottom) {
            clearTextRows(layer, destY, destY + 1);
        }

        if (srcY < 0 || srcY >= height) {
            memset(valid, 0, width);
            return;
        }

        const size_t srcOffset = static_cast<size_t>(srcY) * width + destLeft + dx;
        const size_t destOffset = static_cast<size_t>(destY) * width + destLeft;
        memmove(layer->valid.data() + destOffset, layer->valid.data() + srcOffset, count);
        memmove(layer->shadow.data() + destOffset, layer->shadow.data() + srcOffset, count);
        for (int sy = 0; sy < kHdScale; sy++) {
            uint32_t* destPixels = layer->pixels.data() + static_cast<size_t>(destY * kHdScale + sy) * pitch + destLeft * kHdScale;
            const uint32_t* srcPixels = layer->pixels.data() + static_cast<size_t>(srcY * kHdScale + sy) * pitch + (destLeft + dx) * kHdScale;
            memmove(destPixels, srcPixels, sizeof(uint32_t) * count * kHdScale);
        }

        // Columns with no source.
        if (dx > 0) {
            memset(valid + width - dx, 0, dx);
        } else if (dx < 0) {
            memset(valid, 0, -dx);
        }

        if (textRow) {
            uint16_t* guard = layer->textGuard.data() + static_cast<size_t>(destY) * width;
            memmove(layer->textGuard.data() + destOffset, layer->textGuard.data() + srcOffset, sizeof(uint16_t) * count);
            memmove(layer->textShadow.data() + destOffset, layer->textShadow.data() + srcOffset, count);
            memmove(layer->textUnder.data() + destOffset, layer->textUnder.data() + srcOffset, count);
            for (int sy = 0; sy < kHdScale; sy++) {
                uint32_t* destPixels = layer->textPixels.data() + static_cast<size_t>(destY * kHdScale + sy) * pitch + destLeft * kHdScale;
                const uint32_t* srcPixels = layer->textPixels.data() + static_cast<size_t>(srcY * kHdScale + sy) * pitch + (destLeft + dx) * kHdScale;
                memmove(destPixels, srcPixels, sizeof(uint32_t) * count * kHdScale);
            }
            if (dx > 0) {
                std::fill(guard + width - dx, guard + width, 0);
            } else if (dx < 0) {
                std::fill(guard, guard - dx, 0);
            }
        }
    };

    // Copy in the direction that never overwrites rows still to be read.
    if (dy > 0) {
        for (int y = 0; y < height; y++) {
            moveRow(y);
        }
    } else {
        for (int y = height - 1; y >= 0; y--) {
            moveRow(y);
        }
    }

    layer->textTop = newTop;
    layer->textBottom = newBottom;
}

void hdLayerScrollRect(HdLayer* layer, int left, int top, int width, int height, int dx, int dy)
{
    if (layer == nullptr || (dx == 0 && dy == 0)) {
        return;
    }

    int right = std::min(left + width, layer->width);
    int bottom = std::min(top + height, layer->height);
    left = std::max(left, 0);
    top = std::max(top, 0);
    if (left >= right || top >= bottom) {
        return;
    }
    width = right - left;
    height = bottom - top;
    if (dx <= -width || dx >= width || dy <= -height || dy >= height) {
        hdLayerInvalidateRect(layer, left, top, width, height);
        return;
    }

    const int layerWidth = layer->width;
    const int pitch = layerWidth * kHdScale;
    const int destLeft = left + std::max(-dx, 0);
    const int count = width - std::abs(dx);
    bool text = !layer->textGuard.empty() && layer->textTop < layer->textBottom;

    auto moveRow = [&](int destY) {
        int srcY = destY + dy;
        unsigned char* valid = layer->valid.data() + static_cast<size_t>(destY) * layerWidth;
        uint16_t* guard = text ? layer->textGuard.data() + static_cast<size_t>(destY) * layerWidth : nullptr;
        if (srcY < top || srcY >= bottom) {
            memset(valid + left, 0, width);
            if (guard != nullptr) {
                std::fill(guard + left, guard + right, 0);
            }
            return;
        }

        const size_t srcOffset = static_cast<size_t>(srcY) * layerWidth + destLeft + dx;
        const size_t destOffset = static_cast<size_t>(destY) * layerWidth + destLeft;
        memmove(layer->valid.data() + destOffset, layer->valid.data() + srcOffset, count);
        memmove(layer->shadow.data() + destOffset, layer->shadow.data() + srcOffset, count);
        for (int sy = 0; sy < kHdScale; sy++) {
            uint32_t* destPixels = layer->pixels.data() + static_cast<size_t>(destY * kHdScale + sy) * pitch + destLeft * kHdScale;
            const uint32_t* srcPixels = layer->pixels.data() + static_cast<size_t>(srcY * kHdScale + sy) * pitch + (destLeft + dx) * kHdScale;
            memmove(destPixels, srcPixels, sizeof(uint32_t) * count * kHdScale);
        }
        if (dx > 0) {
            memset(valid + right - dx, 0, dx);
        } else if (dx < 0) {
            memset(valid + left, 0, -dx);
        }

        if (guard != nullptr) {
            memmove(layer->textGuard.data() + destOffset, layer->textGuard.data() + srcOffset, sizeof(uint16_t) * count);
            memmove(layer->textShadow.data() + destOffset, layer->textShadow.data() + srcOffset, count);
            memmove(layer->textUnder.data() + destOffset, layer->textUnder.data() + srcOffset, count);
            for (int sy = 0; sy < kHdScale; sy++) {
                uint32_t* destPixels = layer->textPixels.data() + static_cast<size_t>(destY * kHdScale + sy) * pitch + destLeft * kHdScale;
                const uint32_t* srcPixels = layer->textPixels.data() + static_cast<size_t>(srcY * kHdScale + sy) * pitch + (destLeft + dx) * kHdScale;
                memmove(destPixels, srcPixels, sizeof(uint32_t) * count * kHdScale);
            }
            if (dx > 0) {
                std::fill(guard + right - dx, guard + right, 0);
            } else if (dx < 0) {
                std::fill(guard + left, guard + left - dx, 0);
            }
        }
    };

    if (dy > 0) {
        for (int y = top; y < bottom; y++) {
            moveRow(y);
        }
    } else {
        for (int y = bottom - 1; y >= top; y--) {
            moveRow(y);
        }
    }

    if (text) {
        // Text rows: the old range and where it moved (a superset is fine,
        // rows without text have no guard bits).
        layer->textTop = std::max(std::min(layer->textTop, layer->textTop - dy), 0);
        layer->textBottom = std::min(std::max(layer->textBottom, layer->textBottom - dy), layer->height);
    }
}

void hdComposeRect(const HdLayer* layer, const unsigned char* buffer, int bufferPitch,
    int srcX, int srcY, int width, int height,
    unsigned char* mask, int maskPitch, uint32_t* rgb, int rgbPitch, bool transparent,
    const unsigned char* basePalette8)
{
    bool anyText = layer != nullptr && basePalette8 != nullptr && layer->textTop < layer->textBottom;

    for (int y = 0; y < height; y++) {
        const unsigned char* src = buffer + static_cast<size_t>(srcY + y) * bufferPitch + srcX;
        bool hasText = anyText && srcY + y >= layer->textTop && srcY + y < layer->textBottom;
        unsigned char* maskRow = mask + static_cast<size_t>(y) * maskPitch;
        uint32_t* rgbRow = rgb + static_cast<size_t>(y) * kHdScale * rgbPitch;

        for (int x = 0; x < width; x++) {
            unsigned char index = src[x];
            if (transparent && index == 0) {
                continue;
            }

            int lx = srcX + x;
            int ly = srcY + y;
            if (hasText && lx < layer->width && ly < layer->height && hdLayerTextShows(layer, buffer, bufferPitch, lx, ly)) {
                hdLayerTextBlock(layer, buffer, bufferPitch, lx, ly, basePalette8, rgbRow + x * kHdScale, rgbPitch);
                maskRow[x] = 1;
            } else if (layer != nullptr && lx < layer->width && ly < layer->height && hdLayerShows(layer, lx, ly, index)) {
                const uint32_t* hd = layer->pixels.data() + static_cast<size_t>(ly) * kHdScale * layer->width * kHdScale + lx * kHdScale;
                for (int sy = 0; sy < kHdScale; sy++) {
                    memcpy(rgbRow + sy * rgbPitch + x * kHdScale, hd + static_cast<size_t>(sy) * layer->width * kHdScale, sizeof(uint32_t) * kHdScale);
                }
                maskRow[x] = 1;
            } else {
                maskRow[x] = 0;
            }
        }
    }
}

static void buildModulationEntry(HdModulation* modulation, const unsigned char* basePalette8, const unsigned char* presentedPalette8, int index)
{
    bool identity = true;
    for (int channel = 0; channel < 3; channel++) {
        int base = basePalette8[index * 3 + channel];
        int presented = presentedPalette8[index * 3 + channel];
        unsigned char* lut = modulation->lut[index][channel];

        if (presented == base) {
            for (int value = 0; value < 256; value++) {
                lut[value] = static_cast<unsigned char>(value);
            }
        } else if (presented < base) {
            identity = false;
            for (int value = 0; value < 256; value++) {
                lut[value] = static_cast<unsigned char>((value * presented + base / 2) / base);
            }
        } else {
            identity = false;
            for (int value = 0; value < 256; value++) {
                int result = value + ((presented - base) * (255 - value) + (255 - base) / 2) / (255 - base);
                lut[value] = static_cast<unsigned char>(std::min(result, 255));
            }
        }
    }
    modulation->identity[index] = identity;
}

void hdModulationBuild(HdModulation* modulation, const unsigned char* basePalette8, const unsigned char* presentedPalette8)
{
    hdModulationUpdate(modulation, basePalette8, presentedPalette8, 0, 256);
}

void hdModulationUpdate(HdModulation* modulation, const unsigned char* basePalette8, const unsigned char* presentedPalette8, int start, int count)
{
    for (int index = std::max(start, 0); index < std::min(start + count, 256); index++) {
        buildModulationEntry(modulation, basePalette8, presentedPalette8, index);
    }
}

void hdImageResample(const HdImage& src, int width, int height, HdImage* dest)
{
    dest->width = width;
    dest->height = height;
    dest->pixels.assign(static_cast<size_t>(width) * height, 0);
    if (src.width <= 0 || src.height <= 0 || width <= 0 || height <= 0) {
        return;
    }

    auto sample = [&](int sx, int sy, float weight, float* acc) {
        sx = std::clamp(sx, 0, src.width - 1);
        sy = std::clamp(sy, 0, src.height - 1);
        uint32_t pixel = src.pixels[static_cast<size_t>(sy) * src.width + sx];
        float alpha = static_cast<float>(pixel >> 24) * weight;
        acc[0] += alpha;
        acc[1] += static_cast<float>((pixel >> 16) & 0xFF) * alpha;
        acc[2] += static_cast<float>((pixel >> 8) & 0xFF) * alpha;
        acc[3] += static_cast<float>(pixel & 0xFF) * alpha;
    };

    float scaleX = static_cast<float>(src.width) / width;
    float scaleY = static_cast<float>(src.height) / height;
    for (int y = 0; y < height; y++) {
        float fy = (y + 0.5f) * scaleY - 0.5f;
        int y0 = static_cast<int>(fy >= 0 ? fy : fy - 1);
        float wy = fy - y0;
        for (int x = 0; x < width; x++) {
            float fx = (x + 0.5f) * scaleX - 0.5f;
            int x0 = static_cast<int>(fx >= 0 ? fx : fx - 1);
            float wx = fx - x0;

            float acc[4] = { 0, 0, 0, 0 };
            sample(x0, y0, (1 - wx) * (1 - wy), acc);
            sample(x0 + 1, y0, wx * (1 - wy), acc);
            sample(x0, y0 + 1, (1 - wx) * wy, acc);
            sample(x0 + 1, y0 + 1, wx * wy, acc);

            uint32_t pixel = 0;
            if (acc[0] > 0.0f) {
                auto channel = [&](float value) {
                    return static_cast<uint32_t>(std::clamp(value / acc[0] + 0.5f, 0.0f, 255.0f));
                };
                uint32_t alpha = static_cast<uint32_t>(std::clamp(acc[0] + 0.5f, 0.0f, 255.0f));
                pixel = (alpha << 24) | (channel(acc[1]) << 16) | (channel(acc[2]) << 8) | channel(acc[3]);
            }
            dest->pixels[static_cast<size_t>(y) * width + x] = pixel;
        }
    }
}

void hdImageFromIndexed(const unsigned char* data, int width, int height, int pitch, const unsigned char* palette8, bool zeroIsTransparent, HdImage* dest)
{
    dest->width = width * kHdScale;
    dest->height = height * kHdScale;
    dest->pixels.assign(static_cast<size_t>(dest->width) * dest->height, 0);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            unsigned char index = data[static_cast<size_t>(y) * pitch + x];
            uint32_t pixel = 0;
            if (!(zeroIsTransparent && index == 0)) {
                pixel = 0xFF000000u
                    | (static_cast<uint32_t>(palette8[index * 3]) << 16)
                    | (static_cast<uint32_t>(palette8[index * 3 + 1]) << 8)
                    | palette8[index * 3 + 2];
            }
            for (int sy = 0; sy < kHdScale; sy++) {
                for (int sx = 0; sx < kHdScale; sx++) {
                    dest->pixels[static_cast<size_t>(y * kHdScale + sy) * dest->width + x * kHdScale + sx] = pixel;
                }
            }
        }
    }
}

} // namespace fallout
