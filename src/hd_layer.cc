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
}

void hdLayerClear(HdLayer* layer)
{
    std::fill(layer->valid.begin(), layer->valid.end(), 0);
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

    auto moveRow = [&](int destY) {
        int srcY = destY + dy;
        unsigned char* valid = layer->valid.data() + static_cast<size_t>(destY) * width;
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
}

void hdComposeRect(const HdLayer* layer, const unsigned char* buffer, int bufferPitch,
    int srcX, int srcY, int width, int height,
    unsigned char* mask, int maskPitch, uint32_t* rgb, int rgbPitch, bool transparent)
{
    for (int y = 0; y < height; y++) {
        const unsigned char* src = buffer + static_cast<size_t>(srcY + y) * bufferPitch + srcX;
        unsigned char* maskRow = mask + static_cast<size_t>(y) * maskPitch;
        uint32_t* rgbRow = rgb + static_cast<size_t>(y) * kHdScale * rgbPitch;

        for (int x = 0; x < width; x++) {
            unsigned char index = src[x];
            if (transparent && index == 0) {
                continue;
            }

            int lx = srcX + x;
            int ly = srcY + y;
            if (layer != nullptr && lx < layer->width && ly < layer->height && hdLayerShows(layer, lx, ly, index)) {
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
