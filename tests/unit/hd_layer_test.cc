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

static void testClipAndSource()
{
    std::vector<unsigned char> palette = makePalette();
    // Window 4x2; the art (4x2, all index 10) was drawn at (0, 0), then legacy
    // text put index 50 at (1, 0).
    std::vector<unsigned char> art(4 * 2, 10);
    std::vector<unsigned char> buffer = art;
    buffer[1] = 50;

    HdLayer layer;
    hdLayerInit(&layer, 4, 2);
    HdDrawOptions options;
    options.source = art.data();
    options.sourcePitch = 4;
    options.clipLeft = 0;
    options.clipRight = 3; // column 3 is outside the region
    hdLayerDrawImageEx(&layer, buffer.data(), 4, 0, 0, 4, 2, solidImage(8, 4, kRed), palette.data(), options);

    Staging staging(4, 2);
    staging.compose(&layer, buffer, 4);
    CHECK(staging.at(0, 0) == 1);
    CHECK(staging.at(1, 0) == 0); // text differs from the art: legacy
    CHECK(staging.at(2, 1) == 1);
    CHECK(staging.at(3, 0) == 0); // clipped
    CHECK(staging.hd(2, 1) == 0xFF0000);

    // A region draw of a sub-image: image positioned at (-2, 0) so that its
    // columns 2..3 land on window columns 0..1, clipped to those columns.
    HdImage image = solidImage(8, 4, kGreen);
    for (int y = 0; y < 4; y++) {
        for (int x = 4; x < 8; x++) {
            image.pixels[y * 8 + x] = kRed; // art columns 2..3
        }
    }
    std::fill(buffer.begin(), buffer.end(), 10);
    HdDrawOptions region;
    region.clipLeft = 0;
    region.clipRight = 2;
    hdLayerDrawImageEx(&layer, buffer.data(), 4, -2, 0, 4, 2, image, palette.data(), region);
    staging.compose(&layer, buffer, 4);
    CHECK(staging.at(1, 0) == 1);
    CHECK(staging.hd(0, 0) == 0xFF0000);
    CHECK(staging.hd(1, 1, 1, 1) == 0xFF0000);
}

// Phase 2.7: lit tile draws, invalidation, scrolling.
static void testLight()
{
    CHECK(hdLight(0x804020, 0x10000) == 0x804020);
    CHECK(hdLight(0x804020, 0x8000) == 0x402010);
    CHECK(hdLight(0x804020, 0) == 0);
    CHECK(hdLight(0x804020, -5) == 0);
    CHECK(hdLight(0x00FF80, 0x20000) == 0xFFFFFF);
    // Half way to white.
    CHECK(hdLight(0x000000, 0x18000) == 0x808080);
    // Smooth: neighbouring intensities give neighbouring colours.
    CHECK(hdLight(0xC8C8C8, 0x8000 + 512) == 0x666666);
    // Every channel value and a spread of intensities against the plain
    // per-channel formula.
    for (uint32_t value = 0; value < 256; value++) {
        for (int intensity = 0; intensity < 0x10000; intensity += 257) {
            uint32_t expected = (value * intensity + 0x8000) >> 16;
            uint32_t lit = hdLight((value << 16) | ((255 - value) << 8) | value, intensity);
            CHECK(lit == ((expected << 16) | ((((255 - value) * intensity + 0x8000) >> 16) << 8) | expected));
        }
    }
}

static void testDrawLit()
{
    std::vector<unsigned char> palette = makePalette();
    HdLayer layer;
    hdLayerInit(&layer, 4, 2);
    Staging staging(4, 2);

    // A 3x2 frame: index 0 at (0, 0) is transparent, index 230 at (2, 1)
    // is colour-cycling (alpha 0 in the HD art).
    const unsigned char frame[6] = { 0, 50, 50, 50, 50, 230 };
    HdImage image = solidImage(6, 4, 0xFFC8C8C8);
    for (int sy = 2; sy < 4; sy++) {
        for (int sx = 4; sx < 6; sx++) {
            image.pixels[sy * 6 + sx] = 0;
        }
    }

    // Buffer after the legacy draw at (1, 0): lit index 25, 230 kept.
    std::vector<unsigned char> buffer = { 9, 9, 25, 25, 9, 25, 25, 230 };
    layer.valid[7] = 1; // stale HD pixel under the cycling pixel
    layer.shadow[7] = 230;

    // Per-pixel intensities (pitch 3): right column darker.
    const int intensity[6] = { 0x10000, 0x10000, 0x8000, 0x10000, 0x10000, 0x8000 };
    HdLitDraw draw;
    draw.frame = frame;
    draw.framePitch = 3;
    draw.image = &image;
    draw.x = 1;
    draw.width = 3;
    draw.height = 2;
    draw.intensity = intensity;
    draw.intensityPitch = 3;
    hdLayerDrawLit(&layer, buffer.data(), 4, draw, palette.data());
    staging.compose(&layer, buffer, 4);

    CHECK(staging.at(0, 0) == 0); // outside the draw
    CHECK(staging.at(1, 0) == 0); // transparent frame pixel
    CHECK(staging.at(2, 0) == 1 && staging.hd(2, 0) == 0xC8C8C8);
    CHECK(staging.at(3, 0) == 1 && staging.hd(3, 0, 1, 1) == 0x646464);
    CHECK(staging.at(1, 1) == 1 && staging.hd(1, 1) == 0xC8C8C8);
    CHECK(staging.at(3, 1) == 0); // cycling: legacy, stale pixel dropped
    CHECK(layer.valid[7] == 0);

    // Legacy drawing over it wins; an object pixel of the same index too,
    // once invalidated.
    buffer[2] = 40;
    const unsigned char objectMask[2] = { 0, 1 };
    hdLayerInvalidateMasked(&layer, 1, 1, 2, 1, objectMask, 2);
    staging.compose(&layer, buffer, 4);
    CHECK(staging.at(2, 0) == 0);
    CHECK(staging.at(1, 1) == 1);
    CHECK(staging.at(2, 1) == 0);

    // Constant intensity with an expected-colour check: pixels the legacy
    // code drew differently (blended) keep what the layer had.
    hdLayerClear(&layer);
    std::fill(buffer.begin(), buffer.end(), 20);
    buffer[4] = 21; // "blended"
    unsigned char expected[256];
    for (int index = 0; index < 256; index++) {
        expected[index] = 20;
    }
    draw.intensity = nullptr;
    draw.constantIntensity = 0x8000;
    draw.expected = expected;
    draw.x = 0;
    draw.height = 2;
    hdLayerDrawLit(&layer, buffer.data(), 4, draw, palette.data());
    staging.compose(&layer, buffer, 4);
    CHECK(staging.at(1, 0) == 1 && staging.hd(1, 0) == 0x646464);
    CHECK(staging.at(0, 1) == 0);
    CHECK(staging.at(1, 1) == 1);

    // A frame part outside the image is rejected.
    draw.frameX = 1;
    hdLayerClear(&layer);
    hdLayerDrawLit(&layer, buffer.data(), 4, draw, palette.data());
    staging.compose(&layer, buffer, 4);
    CHECK(staging.at(1, 0) == 0);
}

static void testScrollAndInvalidate()
{
    std::vector<unsigned char> palette = makePalette();
    HdLayer layer;
    hdLayerInit(&layer, 4, 3);

    // Logical pixel (x, y) holds colour x * 16 + y, shadow 1, valid.
    for (int y = 0; y < 3; y++) {
        for (int x = 0; x < 4; x++) {
            for (int sy = 0; sy < kHdScale; sy++) {
                for (int sx = 0; sx < kHdScale; sx++) {
                    layer.pixels[(y * kHdScale + sy) * 4 * kHdScale + x * kHdScale + sx] = x * 16 + y;
                }
            }
            layer.shadow[y * 4 + x] = 1;
            layer.valid[y * 4 + x] = 1;
        }
    }

    hdLayerScroll(&layer, 1, -1);
    std::vector<unsigned char> buffer(12, 1);
    Staging staging(4, 3);
    staging.compose(&layer, buffer, 4);
    for (int y = 0; y < 3; y++) {
        for (int x = 0; x < 4; x++) {
            bool hasSource = x + 1 < 4 && y - 1 >= 0;
            CHECK(staging.at(x, y) == (hasSource ? 1 : 0));
            if (hasSource) {
                CHECK(staging.hd(x, y, 1, 1) == static_cast<uint32_t>((x + 1) * 16 + y - 1));
            }
        }
    }

    hdLayerScroll(&layer, -2, 1);
    staging.compose(&layer, buffer, 4);
    CHECK(staging.at(0, 0) == 0 && staging.at(1, 0) == 0);
    CHECK(staging.at(2, 0) == 1 && staging.hd(2, 0) == 0x10);
    CHECK(staging.at(2, 1) == 1 && staging.hd(2, 1) == 0x11);
    CHECK(staging.at(2, 2) == 0); // came from the vacated bottom row

    hdLayerScroll(&layer, 0, 5);
    staging.compose(&layer, buffer, 4);
    for (int y = 0; y < 3; y++) {
        for (int x = 0; x < 4; x++) {
            CHECK(staging.at(x, y) == 0);
        }
    }

    std::fill(layer.valid.begin(), layer.valid.end(), 1);
    hdLayerInvalidateRect(&layer, -1, 1, 3, 9);
    staging.compose(&layer, buffer, 4);
    CHECK(staging.at(0, 0) == 1 && staging.at(1, 1) == 0 && staging.at(1, 2) == 0 && staging.at(2, 1) == 1);
}

int main()
{
    testShadowValidation();
    testClipAndSource();
    testOverwriteByLegacyThenHdRedraw();
    testCyclingIndicesStayLegacy();
    testAlpha();
    testTransparentWindow();
    testResampleOnScaledDraw();
    testModulation();
    testFromIndexed();
    testLight();
    testDrawLit();
    testScrollAndInvalidate();

    if (gFailures != 0) {
        fprintf(stderr, "hd_layer_test: %d check(s) failed\n", gFailures);
        return 1;
    }
    printf("hd_layer_test: all checks passed\n");
    return 0;
}
