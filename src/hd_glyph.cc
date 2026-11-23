#include "hd_glyph.h"

#include <math.h>
#include <string.h>

#include <algorithm>

namespace fallout {

static constexpr int kMaxLevel = 7;

// EPX / Scale2x (AdvMAME2x) on the level map, zero outside the glyph.
static void upscaleSharp(const unsigned char* levels, int width, int height, std::vector<unsigned char>* alpha)
{
    const int outWidth = (width + 2 * kHdGlyphMargin) * 2;
    const int outHeight = (height + 2 * kHdGlyphMargin) * 2;
    alpha->assign(static_cast<size_t>(outWidth) * outHeight, 0);

    auto at = [&](int x, int y) -> int {
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return 0;
        }
        return std::min<int>(levels[static_cast<size_t>(y) * width + x], kMaxLevel);
    };

    auto put = [&](int x, int y, int level) {
        (*alpha)[static_cast<size_t>(y) * outWidth + x] = static_cast<unsigned char>((level * 255 + kMaxLevel / 2) / kMaxLevel);
    };

    // EPX decides on the glyph's shape: a pixel is "ink" at level 4 or more
    // (the anti-aliasing steps of the interface fonts would otherwise never
    // compare equal and diagonals would stay jagged). A corner sub-pixel
    // whose two neighbours agree on a shape the pixel itself does not have
    // takes their level (the brighter for ink, the darker for background);
    // levels inside the shape are kept, so textured fonts keep their grain.
    // EPX only changes sub-pixels of the glyph box itself, so the margin
    // stays empty.
    constexpr int kInk = 4;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int e = at(x, y);
            int b = at(x, y - 1);
            int d = at(x - 1, y);
            int f = at(x + 1, y);
            int h = at(x, y + 1);
            bool inkE = e >= kInk;
            bool inkB = b >= kInk;
            bool inkD = d >= kInk;
            bool inkF = f >= kInk;
            bool inkH = h >= kInk;

            auto corner = [&](bool inkU, int u, bool inkV, int v) {
                if (inkU != inkV || inkU == inkE) {
                    return e;
                }
                return inkU ? std::max(u, v) : std::min(u, v);
            };

            int e0 = e;
            int e1 = e;
            int e2 = e;
            int e3 = e;
            if (inkB != inkH && inkD != inkF) {
                e0 = corner(inkD, d, inkB, b);
                e1 = corner(inkB, b, inkF, f);
                e2 = corner(inkD, d, inkH, h);
                e3 = corner(inkH, h, inkF, f);
            }

            int ox = (x + kHdGlyphMargin) * 2;
            int oy = (y + kHdGlyphMargin) * 2;
            put(ox, oy, e0);
            put(ox + 1, oy, e1);
            put(ox, oy + 1, e2);
            put(ox + 1, oy + 1, e3);
        }
    }
}

// Catmull-Rom kernel.
static inline float cubic(float x)
{
    x = fabsf(x);
    if (x <= 1.0f) {
        return (1.5f * x - 2.5f) * x * x + 1.0f;
    }
    if (x < 2.0f) {
        return ((-0.5f * x + 2.5f) * x - 4.0f) * x + 2.0f;
    }
    return 0.0f;
}

static void upscaleSmooth(const unsigned char* levels, int width, int height, std::vector<unsigned char>* alpha)
{
    // Samples per logical pixel for the interpolated coverage (a 2x output
    // pixel averages (kFine / 2)^2 of them).
    constexpr int kFine = 8;
    constexpr int kPerOut = kFine / 2;
    // Soft threshold: coverage below 0.5 - kSoft is background, above
    // 0.5 + kSoft is text, smoothstep in between.
    constexpr float kSoft = 0.25f;

    const int boxWidth = width + 2 * kHdGlyphMargin;
    const int boxHeight = height + 2 * kHdGlyphMargin;
    const int fineWidth = boxWidth * kFine;
    const int fineHeight = boxHeight * kFine;
    const int outWidth = boxWidth * 2;
    const int outHeight = boxHeight * 2;

    auto coverage = [&](int x, int y) -> float {
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return 0.0f;
        }
        return static_cast<float>(std::min<int>(levels[static_cast<size_t>(y) * width + x], kMaxLevel)) / kMaxLevel;
    };

    // Fine sample i covers logical coordinate (i + 0.5) / kFine - margin;
    // pixel centres are at k + 0.5.
    auto sourcePosition = [&](int fine) {
        return (fine + 0.5f) / kFine - kHdGlyphMargin - 0.5f;
    };

    // Horizontal pass: every source row (plus two rows of padding on each
    // side for the kernel) at fine x positions.
    const int padRows = 2;
    const int rows = height + 2 * padRows;
    std::vector<float> horizontal(static_cast<size_t>(rows) * fineWidth, 0.0f);
    for (int fx = 0; fx < fineWidth; fx++) {
        float position = sourcePosition(fx);
        int base = static_cast<int>(floorf(position));
        float weights[4];
        for (int tap = 0; tap < 4; tap++) {
            weights[tap] = cubic(position - static_cast<float>(base - 1 + tap));
        }
        for (int row = 0; row < rows; row++) {
            int sy = row - padRows;
            float sum = 0.0f;
            for (int tap = 0; tap < 4; tap++) {
                sum += weights[tap] * coverage(base - 1 + tap, sy);
            }
            horizontal[static_cast<size_t>(row) * fineWidth + fx] = sum;
        }
    }

    std::vector<float> accumulated(static_cast<size_t>(outWidth) * outHeight, 0.0f);
    for (int fy = 0; fy < fineHeight; fy++) {
        float position = sourcePosition(fy);
        int base = static_cast<int>(floorf(position));
        float weights[4];
        int taps[4];
        for (int tap = 0; tap < 4; tap++) {
            weights[tap] = cubic(position - static_cast<float>(base - 1 + tap));
            taps[tap] = std::clamp(base - 1 + tap + padRows, 0, rows - 1);
        }
        float* outRow = accumulated.data() + static_cast<size_t>(fy / kPerOut) * outWidth;
        for (int fx = 0; fx < fineWidth; fx++) {
            float value = 0.0f;
            for (int tap = 0; tap < 4; tap++) {
                value += weights[tap] * horizontal[static_cast<size_t>(taps[tap]) * fineWidth + fx];
            }
            float t = std::clamp((value - (0.5f - kSoft)) / (2.0f * kSoft), 0.0f, 1.0f);
            outRow[fx / kPerOut] += t * t * (3.0f - 2.0f * t);
        }
    }

    alpha->assign(static_cast<size_t>(outWidth) * outHeight, 0);
    for (size_t index = 0; index < accumulated.size(); index++) {
        float value = accumulated[index] / (kPerOut * kPerOut);
        (*alpha)[index] = static_cast<unsigned char>(std::clamp(value * 255.0f + 0.5f, 0.0f, 255.0f));
    }
}

void hdGlyphUpscale(const unsigned char* levels, int width, int height, HdGlyphStyle style, std::vector<unsigned char>* alpha)
{
    if (width <= 0 || height <= 0 || levels == nullptr) {
        int outWidth = (std::max(width, 0) + 2 * kHdGlyphMargin) * 2;
        int outHeight = (std::max(height, 0) + 2 * kHdGlyphMargin) * 2;
        alpha->assign(static_cast<size_t>(outWidth) * outHeight, 0);
        return;
    }

    switch (style) {
    case HdGlyphStyle::Sharp:
        upscaleSharp(levels, width, height, alpha);
        break;
    case HdGlyphStyle::Smooth:
        upscaleSmooth(levels, width, height, alpha);
        break;
    }
}

bool hdGlyphStyleFromName(const char* name, HdGlyphStyle* style)
{
    if (strcmp(name, "sharp") == 0) {
        *style = HdGlyphStyle::Sharp;
        return true;
    }
    if (strcmp(name, "smooth") == 0) {
        *style = HdGlyphStyle::Smooth;
        return true;
    }
    return false;
}

const char* hdGlyphStyleName(HdGlyphStyle style)
{
    return style == HdGlyphStyle::Sharp ? "sharp" : "smooth";
}

} // namespace fallout
