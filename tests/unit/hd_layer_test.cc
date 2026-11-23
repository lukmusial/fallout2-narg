// Unit tests for the HD layer compositor rules (src/hd_layer.cc).
// No game data needed; run with ctest or directly.

#include "hd_layer.h"

#include <stdio.h>

#include <vector>

using namespace fallout;

static int gFailures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                #condition);                                                 \
            gFailures++;                                                     \
        }                                                                    \
    } while (0)

static const uint32_t kRed = 0xFFFF0000;
static const uint32_t kGreen = 0xFF00FF00;

// Base palette: entry i = (i, i, i), except a few named entries.
static std::vector<unsigned char> makePalette()
{
    std::vector<unsigned char> palette(768);
    for (int i = 0; i < 256; i++) {
        palette[i * 3] = palette[i * 3 + 1] = palette[i * 3 + 2] = static_cast<unsigned char>(i);
    }
    return palette;
}

static HdImage solidImage(int width, int height, uint32_t color)
{
    HdImage image;
    image.width = width;
    image.height = height;
    image.pixels.assign(static_cast<size_t>(width) * height, color);
    return image;
}

struct Staging {
    int width;
    int height;
    std::vector<unsigned char> mask;
    std::vector<uint32_t> rgb;

    Staging(int w, int h)
        : width(w)
        , height(h)
        , mask(static_cast<size_t>(w) * h, 7)
        , rgb(static_cast<size_t>(w) * h * kHdScale * kHdScale, 0xDEADBEEF)
    {
    }

    void compose(const HdLayer* layer, const std::vector<unsigned char>& buffer, int bufferPitch, bool transparent = false)
    {
        hdComposeRect(layer, buffer.data(), bufferPitch, 0, 0, width, height, mask.data(), width, rgb.data(), width * kHdScale, transparent);
    }

    uint32_t hd(int x, int y, int sx = 0, int sy = 0) const
    {
        return rgb[static_cast<size_t>(y * kHdScale + sy) * width * kHdScale + x * kHdScale + sx];
    }

    unsigned char at(int x, int y) const { return mask[static_cast<size_t>(y) * width + x]; }
};

static void testShadowValidation()
{
    std::vector<unsigned char> palette = makePalette();
    std::vector<unsigned char> buffer(4 * 4, 10);
    HdLayer layer;
    hdLayerInit(&layer, 4, 4);

    hdLayerDrawImage(&layer, buffer.data(), 4, 0, 0, 4, 4, solidImage(8, 8, kRed), palette.data());

    Staging staging(4, 4);
    staging.compose(&layer, buffer, 4);
    CHECK(staging.at(0, 0) == 1);
    CHECK(staging.at(3, 3) == 1);
    CHECK(staging.hd(2, 2) == 0xFF0000);
    CHECK(staging.hd(2, 2, 1, 1) == 0xFF0000);

    // Legacy code draws over one pixel: it shows legacy there, HD elsewhere.
    buffer[1 * 4 + 2] = 99;
    staging.compose(&layer, buffer, 4);
    CHECK(staging.at(2, 1) == 0);
    CHECK(staging.at(1, 1) == 1);

    // Restoring the original index re-validates the HD block.
    buffer[1 * 4 + 2] = 10;
    staging.compose(&layer, buffer, 4);
    CHECK(staging.at(2, 1) == 1);
    CHECK(staging.hd(2, 1) == 0xFF0000);

    // A window without a layer never shows HD.
    staging.compose(nullptr, buffer, 4);
    CHECK(staging.at(0, 0) == 0);
}

static void testOverwriteByLegacyThenHdRedraw()
{
    std::vector<unsigned char> palette = makePalette();
    std::vector<unsigned char> buffer(2 * 2, 10);
    HdLayer layer;
    hdLayerInit(&layer, 2, 2);
    hdLayerDrawImage(&layer, buffer.data(), 2, 0, 0, 2, 2, solidImage(4, 4, kRed), palette.data());

    // Legacy redraw with new indices, then an HD redraw: the new shadow wins.
    std::fill(buffer.begin(), buffer.end(), 20);
    hdLayerDrawImage(&layer, buffer.data(), 2, 0, 0, 2, 2, solidImage(4, 4, kGreen), palette.data());

    Staging staging(2, 2);
    staging.compose(&layer, buffer, 2);
    CHECK(staging.at(0, 0) == 1);
    CHECK(staging.hd(1, 1) == 0x00FF00);

    // The old indices no longer validate.
    std::fill(buffer.begin(), buffer.end(), 10);
    staging.compose(&layer, buffer, 2);
    CHECK(staging.at(0, 0) == 0);
}

static void testCyclingIndicesStayLegacy()
{
    std::vector<unsigned char> palette = makePalette();
    std::vector<unsigned char> buffer = { 10, static_cast<unsigned char>(kHdCycleFirst), static_cast<unsigned char>(kHdCycleLast), 255 };
    HdLayer layer;
    hdLayerInit(&layer, 4, 1);
    hdLayerDrawImage(&layer, buffer.data(), 4, 0, 0, 4, 1, solidImage(8, 2, kRed), palette.data());

    Staging staging(4, 1);
    staging.compose(&layer, buffer, 4);
    CHECK(staging.at(0, 0) == 1);
    CHECK(staging.at(1, 0) == 0);
    CHECK(staging.at(2, 0) == 0);
    CHECK(staging.at(3, 0) == 1);
}

static void testAlpha()
{
    std::vector<unsigned char> palette = makePalette();
    std::vector<unsigned char> buffer(2, 100);
    HdLayer layer;
    hdLayerInit(&layer, 2, 1);

    // Pixel 0: fully transparent block -> untouched (stays invalid).
    // Pixel 1: half-transparent red over the legacy colour (100, 100, 100).
    HdImage image = solidImage(4, 2, 0x00000000);
    for (int sy = 0; sy < 2; sy++) {
        for (int sx = 2; sx < 4; sx++) {
            image.pixels[sy * 4 + sx] = 0x80FF0000;
        }
    }
    hdLayerDrawImage(&layer, buffer.data(), 2, 0, 0, 2, 1, image, palette.data());

    Staging staging(2, 1);
    staging.compose(&layer, buffer, 2);
    CHECK(staging.at(0, 0) == 0);
    CHECK(staging.at(1, 0) == 1);
    uint32_t blended = staging.hd(1, 0);
    CHECK(((blended >> 16) & 0xFF) == (255 * 128 + 100 * 127 + 127) / 255);
    CHECK(((blended >> 8) & 0xFF) == (100 * 127 + 127) / 255);

    // A second translucent draw blends over the HD pixel, not the legacy colour.
    hdLayerDrawImage(&layer, buffer.data(), 2, 0, 0, 2, 1, image, palette.data());
    staging.compose(&layer, buffer, 2);
    uint32_t twice = staging.hd(1, 0);
    CHECK(((twice >> 16) & 0xFF) > ((blended >> 16) & 0xFF));
}

static void testTransparentWindow()
{
    std::vector<unsigned char> buffer = { 0, 5 };
    Staging staging(2, 1);
    staging.mask[0] = 1;
    staging.rgb[0] = 0x123456;
    staging.compose(nullptr, buffer, 2, true);
    CHECK(staging.at(0, 0) == 1); // see-through pixel keeps what is below
    CHECK(staging.hd(0, 0) == 0x123456);
    CHECK(staging.at(1, 0) == 0); // opaque legacy pixel of the upper window
}

static void testResampleOnScaledDraw()
{
    std::vector<unsigned char> palette = makePalette();
    std::vector<unsigned char> buffer(4 * 4, 10);
    HdLayer layer;
    hdLayerInit(&layer, 4, 4);
    // A 2x2 image drawn over 4x4 logical pixels (8x8 HD pixels).
    hdLayerDrawImage(&layer, buffer.data(), 4, 0, 0, 4, 4, solidImage(2, 2, kGreen), palette.data());
    Staging staging(4, 4);
    staging.compose(&layer, buffer, 4);
    CHECK(staging.at(3, 3) == 1);
    CHECK(staging.hd(3, 3, 1, 1) == 0x00FF00);
}

static void testModulation()
{
    std::vector<unsigned char> base = makePalette();
    std::vector<unsigned char> presented = base;
    HdModulation modulation;

    hdModulationBuild(&modulation, base.data(), presented.data());
    CHECK(modulation.identity[50]);
    CHECK(hdModulate(&modulation, 0x123456, 50) == 0x123456);

    // Fade to black: everything maps to 0.
    std::vector<unsigned char> black(768, 0);
    hdModulationBuild(&modulation, base.data(), black.data());
    CHECK(hdModulate(&modulation, 0xFFFFFF, 200) == 0);
    // Index 0 (base 0, presented 0) is unchanged by definition.
    CHECK(modulation.identity[0]);

    // Halfway: entry 200 presented as 100 -> HD colours halve.
    presented = base;
    presented[200 * 3] = presented[200 * 3 + 1] = presented[200 * 3 + 2] = 100;
    hdModulationBuild(&modulation, base.data(), presented.data());
    CHECK(hdModulate(&modulation, 0xC8C8C8, 200) == 0x646464); // base colour -> presented colour
    CHECK(hdModulate(&modulation, 0x0A141E, 200) == 0x050A0F);
    CHECK(hdModulate(&modulation, 0x0A141E, 199) == 0x0A141E); // other entries untouched

    // Fade to white: entry 100 presented as 255 -> everything becomes white,
    // and the base colour maps exactly to the presented colour at any step.
    presented = base;
    presented[100 * 3] = presented[100 * 3 + 1] = presented[100 * 3 + 2] = 255;
    hdModulationUpdate(&modulation, base.data(), presented.data(), 100, 1);
    CHECK(hdModulate(&modulation, 0x000000, 100) == 0xFFFFFF);
    presented[100 * 3] = 180;
    hdModulationUpdate(&modulation, base.data(), presented.data(), 100, 1);
    CHECK(((hdModulate(&modulation, 0x646464, 100) >> 16) & 0xFF) == 180);

    // Monotonic per channel.
    bool monotonic = true;
    for (int value = 1; value < 256; value++) {
        if (modulation.lut[100][0][value] < modulation.lut[100][0][value - 1]) {
            monotonic = false;
        }
    }
    CHECK(monotonic);
}

static void testFromIndexed()
{
    std::vector<unsigned char> palette = makePalette();
    unsigned char data[2] = { 0, 77 };
    HdImage image;
    hdImageFromIndexed(data, 2, 1, 2, palette.data(), true, &image);
    CHECK(image.width == 4 && image.height == 2);
    CHECK(image.pixels[0] == 0);
    CHECK(image.pixels[2] == 0xFF4D4D4D);
    CHECK(image.pixels[4 + 3] == 0xFF4D4D4D);
}

int main()
{
    testShadowValidation();
    testOverwriteByLegacyThenHdRedraw();
    testCyclingIndicesStayLegacy();
    testAlpha();
    testTransparentWindow();
    testResampleOnScaledDraw();
    testModulation();
    testFromIndexed();

    if (gFailures != 0) {
        fprintf(stderr, "hd_layer_test: %d check(s) failed\n", gFailures);
        return 1;
    }
    printf("hd_layer_test: all checks passed\n");
    return 0;
}
