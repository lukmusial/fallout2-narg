// Unit tests for the HD light field (src/hd_lights.cc). No game data needed.

#include "hd_lights.h"

#include <math.h>
#include <algorithm>
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

static const int kWidth = 64;
static const int kHeight = 48;

// 2x output for a kWidth x kHeight logical screen, one colour everywhere.
struct Output {
    std::vector<uint32_t> pixels;
    std::vector<unsigned char> mask;

    Output(uint32_t colour, unsigned char maskValue)
        : pixels(static_cast<size_t>(kWidth) * 2 * kHeight * 2, colour)
        , mask(static_cast<size_t>(kWidth) * kHeight, maskValue)
    {
    }

    void apply(const HdLightField& field)
    {
        for (int y = 0; y < kHeight; y++) {
            hdLightApplyRow(field, y, 0, kWidth, &mask[static_cast<size_t>(y) * kWidth], &pixels[static_cast<size_t>(y) * 2 * kWidth * 2], kWidth * 2);
        }
    }

    // Top-left output pixel of logical pixel (x, y).
    uint32_t at(int x, int y) const { return pixels[static_cast<size_t>(y) * 2 * kWidth * 2 + x * 2]; }
};

static int red(uint32_t c) { return (c >> 16) & 0xFF; }
static int green(uint32_t c) { return (c >> 8) & 0xFF; }
static int blue(uint32_t c) { return c & 0xFF; }

static void testNeutral()
{
    HdLightField field;
    hdLightFieldInit(&field, kWidth, kHeight);
    hdLightFieldFinish(&field);
    Output out(0x00806040, kHdMaskLit | kHdMaskShows);
    out.apply(field);
    for (uint32_t pixel : out.pixels) {
        CHECK(pixel == 0x00806040);
    }
}

static HdLightFootprint centredFootprint()
{
    // A light at (32, 24) with value 1 there, falling to 0 on a ring 20 px out.
    std::vector<HdLightSample> samples;
    samples.push_back({ 32.0f, 24.0f, 1.0f });
    for (int i = 0; i < 12; i++) {
        float angle = i * 3.14159265f / 6.0f;
        samples.push_back({ 32.0f + 10.0f * cosf(angle), 24.0f + 10.0f * sinf(angle), 0.5f });
        samples.push_back({ 32.0f + 20.0f * cosf(angle), 24.0f + 20.0f * sinf(angle), 0.0f });
    }
    HdLightFootprint footprint;
    hdLightFootprintBuild(samples, 12.0f, 12.0f, 1.0f, &footprint);
    return footprint;
}

static float footprintAt(const HdLightFootprint& footprint, float x, float y)
{
    int cx = static_cast<int>(x / kHdLightCell) - footprint.cellX;
    int cy = static_cast<int>(y / kHdLightCell) - footprint.cellY;
    if (cx < 0 || cy < 0 || cx >= footprint.cellsX || cy >= footprint.cellsY) {
        return 0.0f;
    }
    return footprint.weight[static_cast<size_t>(cy) * footprint.cellsX + cx];
}

static void testFootprint()
{
    HdLightFootprint footprint = centredFootprint();
    CHECK(footprint.cellsX > 0 && footprint.cellsY > 0);
    float centre = footprintAt(footprint, 32, 24);
    float middle = footprintAt(footprint, 42, 24);
    float edge = footprintAt(footprint, 51, 24);
    CHECK(centre > 0.55f);
    CHECK(middle < centre && middle > 0.2f);
    CHECK(edge < 0.25f);
    // Smooth: no jump between neighbouring cells larger than a fraction.
    for (int x = 20; x < 44; x += kHdLightCell) {
        CHECK(fabsf(footprintAt(footprint, static_cast<float>(x), 24) - footprintAt(footprint, static_cast<float>(x + kHdLightCell), 24)) < 0.25f);
    }
}

static void testColouredLight()
{
    HdLightField field;
    hdLightFieldInit(&field, kWidth, kHeight);
    HdLightFootprint footprint = centredFootprint();
    const float orange[3] = { 1.0f, 0.5f, 0.0f };
    hdLightFieldAddFootprint(&field, footprint, orange, 1.0f);
    hdLightFieldFinish(&field);

    Output out(0x00646464, kHdMaskLit);
    out.apply(field);
    uint32_t lit = out.at(32, 24);
    CHECK(red(lit) > 150);
    CHECK(green(lit) > 100 && green(lit) < red(lit));
    CHECK(blue(lit) == 100);
    // Far from the light: unchanged.
    CHECK(out.at(1, 1) == 0x00646464);
    CHECK(out.at(kWidth - 1, kHeight - 1) == 0x00646464);

    // Pixels outside the map window (no kHdMaskLit) never change.
    Output other(0x00646464, 0);
    other.apply(field);
    CHECK(other.at(32, 24) == 0x00646464);

    // Block form agrees with the row form.
    uint32_t block[4] = { 0x00646464, 0x00646464, 0x00646464, 0x00646464 };
    hdLightApplyBlock(field, 32, 24, false, block, 2);
    CHECK(block[0] == lit);
}

static void testGlowAndBlur()
{
    HdLightField field;
    hdLightFieldInit(&field, kWidth, kHeight);

    // One emissive (red) pixel cell in the middle of the screen.
    std::vector<unsigned char> index(static_cast<size_t>(kWidth) * kHeight, 1);
    std::vector<unsigned char> mask(index.size(), kHdMaskLit);
    for (int y = 20; y < 24; y++) {
        for (int x = 32; x < 36; x++) {
            index[static_cast<size_t>(y) * kWidth + x] = 2;
        }
    }
    unsigned char palette[256 * 3] = {};
    palette[2 * 3] = 255;
    float weights[256] = {};
    weights[2] = 1.0f;
    std::vector<float> grid;
    HdLightBox box;
    hdLightEmissive(index.data(), mask.data(), kWidth, kWidth, kHeight, palette, weights, &grid, &box);
    CHECK(box.x0 == 8 && box.x1 == 9 && box.y0 == 5 && box.y1 == 6);
    CHECK(fabsf(grid[(5 * field.cellsX + 8) * 3] - 1.0f) < 1e-5f);

    float before = 0.0f;
    for (size_t i = 0; i < grid.size(); i += 3) {
        before += grid[i];
    }
    hdLightBlur(&grid, field.cellsX, field.cellsY, 1, 1, 3, &box);
    float after = 0.0f;
    for (size_t i = 0; i < grid.size(); i += 3) {
        after += grid[i];
    }
    CHECK(fabsf(before - after) < 1e-4f);
    CHECK(grid[(5 * field.cellsX + 8) * 3] < 0.5f);
    CHECK(box.x0 == 5 && box.x1 == 12);

    hdLightFieldAddGrid(&field, grid, box, 255.0f, true);
    hdLightFieldFinish(&field);
    Output out(0x00000000, kHdMaskLit);
    out.apply(field);
    CHECK(red(out.at(33, 21)) > 10);
    CHECK(green(out.at(33, 21)) == 0);
    CHECK(out.at(2, 2) == 0);
}

static void testShadow()
{
    HdLightField field;
    hdLightFieldInit(&field, kWidth, kHeight);
    hdLightFieldAddShadow(&field, 32.0f, 24.0f, 10.0f, 0.0f, 4.0f, 0.5f);
    hdLightFieldFinish(&field);

    // Shadows darken HD pixels only.
    Output hd(0x00808080, kHdMaskLit | kHdMaskShows);
    hd.apply(field);
    CHECK(red(hd.at(32, 24)) < 0x80 && red(hd.at(32, 24)) >= 0x40);
    CHECK(hd.at(32, 40) == 0x00808080);
    Output legacy(0x00808080, kHdMaskLit);
    legacy.apply(field);
    CHECK(legacy.at(32, 24) == 0x00808080);
}

static void testDiffAndClear()
{
    HdLightField a;
    HdLightField b;
    hdLightFieldInit(&a, kWidth, kHeight);
    hdLightFieldInit(&b, kWidth, kHeight);
    hdLightFieldFinish(&a);
    hdLightFieldFinish(&b);
    std::vector<HdLightRect> rects;
    hdLightFieldDiff(a, b, &rects);
    CHECK(rects.empty());

    hdLightFieldAddShadow(&b, 10.0f, 10.0f, 3.0f, 0.0f, 3.0f, 0.5f);
    hdLightFieldFinish(&b);
    hdLightFieldDiff(a, b, &rects);
    CHECK(!rects.empty());
    bool covered = false;
    for (const HdLightRect& rect : rects) {
        covered |= rect.left <= 10 && rect.top <= 10 && rect.left + rect.width > 10 && rect.top + rect.height > 10;
    }
    CHECK(covered);

    // Clearing restores a neutral field (only the touched area is reset).
    hdLightFieldClear(&b);
    hdLightFieldFinish(&b);
    hdLightFieldDiff(a, b, &rects);
    CHECK(rects.empty());
    for (float value : b.shade) {
        CHECK(value == 0.0f);
    }
}

// Phase 2.13: a light carried by a walking critter follows the sprite.
static void testMovedLight()
{
    const int width = 320;
    const int height = 192;
    const float ambient = 16384.0f;
    const float maximum = 65536.0f;
    const float lightX = 160.0f;
    const float lightY = 96.0f;

    // Hex centres (32 px apart, rows 12 px apart and offset by 16): the
    // engine's light falls off linearly over 4 hexes (screen y foreshortened).
    std::vector<HdMovedLightHex> hexes;
    for (int row = 0; row < height / 12; row++) {
        for (int column = 0; column < width / 32; column++) {
            float x = column * 32.0f + (row % 2 != 0 ? 16.0f : 0.0f);
            float y = row * 12.0f;
            float distance = sqrtf((x - lightX) * (x - lightX) + 4.0f * (y - lightY) * (y - lightY)) / 32.0f;
            float own = distance < 4.0f ? 49152.0f * (1.0f - distance / 4.0f) : 0.0f;
            hexes.push_back({ x, y, 655.0f + own, own });
        }
    }

    HdLightField field;
    hdLightFieldInit(&field, width, height);
    hdLightFieldAddMovedLight(&field, hexes, 0.0f, 0.0f, ambient, maximum, 32.0f, 20.0f);
    CHECK(field.lightBox.empty());

    const float dx = 16.0f;
    const float dy = 6.0f;
    hdLightFieldAddMovedLight(&field, hexes, dx, dy, ambient, maximum, 32.0f, 20.0f);
    CHECK(!field.lightBox.empty());

    // The corrected light (drawn light times 1 + mul) equals the light moved
    // to the sprite, and its centre moves with the sprite.
    double sumDrawn = 0.0;
    double sumDrawnX = 0.0;
    double sumDrawnY = 0.0;
    double sumCorrected = 0.0;
    double sumCorrectedX = 0.0;
    double sumCorrectedY = 0.0;
    float worst = 0.0f;
    for (int cy = 0; cy < field.cellsY; cy++) {
        for (int cx = 0; cx < field.cellsX; cx++) {
            float x = (cx + 0.5f) * kHdLightCell;
            float y = (cy + 0.5f) * kHdLightCell;
            float drawn = hdMovedLightAt(hexes, x, y, 0.0f, 0.0f, ambient, maximum, 32.0f, 20.0f);
            float moved = hdMovedLightAt(hexes, x, y, dx, dy, ambient, maximum, 32.0f, 20.0f);
            float corrected = drawn * (1.0f + field.mul[(static_cast<size_t>(cy) * field.cellsX + cx) * 3]);
            if (drawn > 0.0f) {
                worst = std::max(worst, fabsf(corrected - moved) / maximum);
            }
            // Away from the screen edge (where hexes are missing).
            if (x > 40.0f && x < width - 40.0f && y > 30.0f && y < height - 30.0f) {
                sumDrawn += drawn - ambient;
                sumDrawnX += (drawn - ambient) * x;
                sumDrawnY += (drawn - ambient) * y;
                sumCorrected += corrected - ambient;
                sumCorrectedX += (corrected - ambient) * x;
                sumCorrectedY += (corrected - ambient) * y;
            }
        }
    }
    CHECK(worst < 0.01f);
    CHECK(sumDrawn > 0.0 && sumCorrected > 0.0);
    double shiftX = sumCorrectedX / sumCorrected - sumDrawnX / sumDrawn;
    double shiftY = sumCorrectedY / sumCorrected - sumDrawnY / sumDrawn;
    CHECK(fabs(shiftX - dx) < 2.0);
    CHECK(fabs(shiftY - dy) < 2.0);

    // The moved light at the sprite equals the unmoved light at the hex.
    float atHex = hdMovedLightAt(hexes, lightX, lightY, 0.0f, 0.0f, ambient, maximum, 32.0f, 20.0f);
    float atSprite = hdMovedLightAt(hexes, lightX + dx, lightY + dy, dx, dy, ambient, maximum, 32.0f, 20.0f);
    CHECK(fabsf(atHex - atSprite) < 0.02f * maximum);
}

int main()
{
    testNeutral();
    testFootprint();
    testColouredLight();
    testGlowAndBlur();
    testShadow();
    testDiffAndClear();
    testMovedLight();

    if (gFailures != 0) {
        fprintf(stderr, "%d check(s) failed\n", gFailures);
        return 1;
    }
    printf("hd_lights_test: all checks passed\n");
    return 0;
}
