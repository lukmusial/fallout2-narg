#include "hd_lights.h"

#include <math.h>

#include <algorithm>

namespace fallout {

namespace {

    // Values below this make no visible difference (well under one 8-bit
    // level at full brightness).
    constexpr float kNeutralMul = 1.0f / 512.0f;
    constexpr float kNeutralAdd = 0.25f;
    constexpr float kNeutralShade = 1.0f / 512.0f;

    // Bilinear sample position for logical coordinate `x` on a grid of
    // `cell` pixels per cell and `count` cells.
    inline void samplePosition(int x, int cell, int count, int* c0, int* c1, float* f)
    {
        float u = (x + 0.5f) / cell - 0.5f;
        int i = static_cast<int>(floorf(u));
        float frac = u - i;
        if (i < 0) {
            i = 0;
            frac = 0.0f;
        }
        if (i >= count - 1) {
            i = count - 1;
            frac = 0.0f;
        }
        *c0 = i;
        *c1 = std::min(i + 1, count - 1);
        *f = frac;
    }

    // One box blur pass along a line of `count` cells with stride `stride`
    // (in floats), 3 channels; `temp` holds a copy of the line.
    void boxLine(float* data, int count, int stride, int radius, std::vector<float>* temp)
    {
        temp->resize(static_cast<size_t>(count) * 3);
        for (int i = 0; i < count; i++) {
            for (int c = 0; c < 3; c++) {
                (*temp)[i * 3 + c] = data[static_cast<size_t>(i) * stride + c];
            }
        }

        float scale = 1.0f / (2 * radius + 1);
        float sum[3] = { 0.0f, 0.0f, 0.0f };
        // Running sum over [i - radius, i + radius]; cells outside the line
        // count as 0 (light falls off at the screen edge).
        for (int i = 0; i < std::min(radius, count); i++) {
            for (int c = 0; c < 3; c++) {
                sum[c] += (*temp)[i * 3 + c];
            }
        }
        for (int i = 0; i < count; i++) {
            int in = i + radius;
            if (in < count) {
                for (int c = 0; c < 3; c++) {
                    sum[c] += (*temp)[in * 3 + c];
                }
            }
            for (int c = 0; c < 3; c++) {
                data[static_cast<size_t>(i) * stride + c] = sum[c] * scale;
            }
            int out = i - radius;
            if (out >= 0) {
                for (int c = 0; c < 3; c++) {
                    sum[c] -= (*temp)[out * 3 + c];
                }
            }
        }
    }

} // namespace

void HdLightBox::add(const HdLightBox& other)
{
    if (other.empty()) {
        return;
    }
    if (empty()) {
        *this = other;
        return;
    }
    x0 = std::min(x0, other.x0);
    y0 = std::min(y0, other.y0);
    x1 = std::max(x1, other.x1);
    y1 = std::max(y1, other.y1);
}

void hdLightFieldInit(HdLightField* field, int width, int height)
{
    field->width = width;
    field->height = height;
    field->cellsX = std::max((width + kHdLightCell - 1) / kHdLightCell, 1);
    field->cellsY = std::max((height + kHdLightCell - 1) / kHdLightCell, 1);
    field->shadeX = std::max((width + kHdShadeCell - 1) / kHdShadeCell, 1);
    field->shadeY = std::max((height + kHdShadeCell - 1) / kHdShadeCell, 1);
    hdLightFieldClear(field);
}

void hdLightFieldClear(HdLightField* field)
{
    size_t cells = static_cast<size_t>(field->cellsX) * field->cellsY;
    size_t shades = static_cast<size_t>(field->shadeX) * field->shadeY;
    if (field->mul.size() != cells * 3 || field->shade.size() != shades) {
        field->mul.assign(cells * 3, 0.0f);
        field->add.assign(cells * 3, 0.0f);
        field->shade.assign(shades, 0.0f);
        field->lightActive.assign(cells, 0);
        field->shadeActive.assign(shades, 0);
    } else {
        const HdLightBox& light = field->lightBox;
        for (int y = light.y0; y < light.y1; y++) {
            size_t row = static_cast<size_t>(y) * field->cellsX;
            std::fill(field->mul.begin() + (row + light.x0) * 3, field->mul.begin() + (row + light.x1) * 3, 0.0f);
            std::fill(field->add.begin() + (row + light.x0) * 3, field->add.begin() + (row + light.x1) * 3, 0.0f);
            std::fill(field->lightActive.begin() + row + light.x0, field->lightActive.begin() + row + light.x1, 0);
        }
        const HdLightBox& shade = field->shadeBox;
        for (int y = shade.y0; y < shade.y1; y++) {
            size_t row = static_cast<size_t>(y) * field->shadeX;
            std::fill(field->shade.begin() + row + shade.x0, field->shade.begin() + row + shade.x1, 0.0f);
            std::fill(field->shadeActive.begin() + row + shade.x0, field->shadeActive.begin() + row + shade.x1, 0);
        }
    }
    field->lightBox = HdLightBox();
    field->shadeBox = HdLightBox();
}

void hdLightFieldFinish(HdLightField* field)
{
    // A cell is active when it or its right/lower neighbour is not neutral,
    // so the active area extends one cell up and left of the box.
    HdLightBox& light = field->lightBox;
    if (!light.empty()) {
        light.x0 = std::max(light.x0 - 1, 0);
        light.y0 = std::max(light.y0 - 1, 0);
        for (int y = light.y0; y < light.y1; y++) {
            int y1 = std::min(y + 1, field->cellsY - 1);
            for (int x = light.x0; x < light.x1; x++) {
                int x1 = std::min(x + 1, field->cellsX - 1);
                bool active = false;
                const size_t corners[4] = {
                    static_cast<size_t>(y) * field->cellsX + x,
                    static_cast<size_t>(y) * field->cellsX + x1,
                    static_cast<size_t>(y1) * field->cellsX + x,
                    static_cast<size_t>(y1) * field->cellsX + x1,
                };
                for (size_t corner : corners) {
                    const float* mul = &field->mul[corner * 3];
                    const float* add = &field->add[corner * 3];
                    if (fabsf(mul[0]) > kNeutralMul || fabsf(mul[1]) > kNeutralMul || fabsf(mul[2]) > kNeutralMul
                        || add[0] > kNeutralAdd || add[1] > kNeutralAdd || add[2] > kNeutralAdd) {
                        active = true;
                        break;
                    }
                }
                field->lightActive[static_cast<size_t>(y) * field->cellsX + x] = active;
            }
        }
    }

    HdLightBox& shade = field->shadeBox;
    if (!shade.empty()) {
        shade.x0 = std::max(shade.x0 - 1, 0);
        shade.y0 = std::max(shade.y0 - 1, 0);
        for (int y = shade.y0; y < shade.y1; y++) {
            int y1 = std::min(y + 1, field->shadeY - 1);
            for (int x = shade.x0; x < shade.x1; x++) {
                int x1 = std::min(x + 1, field->shadeX - 1);
                field->shadeActive[static_cast<size_t>(y) * field->shadeX + x]
                    = field->shade[static_cast<size_t>(y) * field->shadeX + x] > kNeutralShade
                    || field->shade[static_cast<size_t>(y) * field->shadeX + x1] > kNeutralShade
                    || field->shade[static_cast<size_t>(y1) * field->shadeX + x] > kNeutralShade
                    || field->shade[static_cast<size_t>(y1) * field->shadeX + x1] > kNeutralShade;
            }
        }
    }
}

void hdLightApplyBlock(const HdLightField& field, int x, int y, bool shaded, uint32_t* dest, int pitch)
{
    unsigned char mask = kHdMaskLit | (shaded ? kHdMaskShows : 0);
    hdLightApplyRow(field, y, x, 1, &mask, dest, pitch);
}

namespace {

    // Integer form of samplePosition: cell and fraction (0..256) of logical
    // coordinate x on a grid of `cell` (a power of two) pixels per cell.
    inline void sampleCell(int x, int cell, int count, int* c0, int* frac256)
    {
        int numerator = 2 * x + 1 - cell;
        if (numerator < 0) {
            *c0 = 0;
            *frac256 = 0;
            return;
        }
        int i = numerator / (2 * cell);
        if (i >= count - 1) {
            *c0 = count - 1;
            *frac256 = 0;
            return;
        }
        *c0 = i;
        *frac256 = (numerator % (2 * cell)) * 256 / (2 * cell);
    }

    inline uint32_t lightChannel(uint32_t value, int factor256, int add)
    {
        int result = ((static_cast<int>(value) * factor256 + 128) >> 8) + add;
        return result <= 0 ? 0 : (result >= 255 ? 255 : static_cast<uint32_t>(result));
    }

} // namespace

void hdLightApplyRow(const HdLightField& field, int y, int left, int width, const unsigned char* maskRow, uint32_t* dest, int pitch)
{
    int ly0;
    int ly1;
    float lfy;
    samplePosition(y, kHdLightCell, field.cellsY, &ly0, &ly1, &lfy);
    int sy0;
    int sy1;
    float sfy;
    samplePosition(y, kHdShadeCell, field.shadeY, &sy0, &sy1, &sfy);

    const unsigned char* lightActive = &field.lightActive[static_cast<size_t>(ly0) * field.cellsX];
    const unsigned char* shadeActive = &field.shadeActive[static_cast<size_t>(sy0) * field.shadeX];
    const float* mul0 = &field.mul[static_cast<size_t>(ly0) * field.cellsX * 3];
    const float* mul1 = &field.mul[static_cast<size_t>(ly1) * field.cellsX * 3];
    const float* add0 = &field.add[static_cast<size_t>(ly0) * field.cellsX * 3];
    const float* add1 = &field.add[static_cast<size_t>(ly1) * field.cellsX * 3];
    const float* shade0 = &field.shade[static_cast<size_t>(sy0) * field.shadeX];
    const float* shade1 = &field.shade[static_cast<size_t>(sy1) * field.shadeX];

    // Vertically interpolated mul/add of light cells c and c + 1, for the
    // last cell seen (pixels of one cell share them).
    int cachedCell = -1;
    float columns[2][6];

    for (int index = 0; index < width; index++) {
        unsigned char mask = maskRow[index];
        if ((mask & kHdMaskLit) == 0) {
            continue;
        }

        int x = left + index;
        int lx0;
        int lfx;
        sampleCell(x, kHdLightCell, field.cellsX, &lx0, &lfx);
        bool lit = lightActive[lx0] != 0;

        int sx0 = 0;
        int sfx = 0;
        bool shaded = false;
        if ((mask & kHdMaskShows) != 0) {
            sampleCell(x, kHdShadeCell, field.shadeX, &sx0, &sfx);
            shaded = shadeActive[sx0] != 0;
        }

        if (!lit && !shaded) {
            continue;
        }

        float factor[3] = { 1.0f, 1.0f, 1.0f };
        float add[3] = { 0.0f, 0.0f, 0.0f };
        if (lit) {
            if (cachedCell != lx0) {
                cachedCell = lx0;
                int lx1 = std::min(lx0 + 1, field.cellsX - 1);
                for (int column = 0; column < 2; column++) {
                    size_t offset = static_cast<size_t>(column == 0 ? lx0 : lx1) * 3;
                    for (int c = 0; c < 3; c++) {
                        columns[column][c] = mul0[offset + c] + (mul1[offset + c] - mul0[offset + c]) * lfy;
                        columns[column][3 + c] = add0[offset + c] + (add1[offset + c] - add0[offset + c]) * lfy;
                    }
                }
            }
            float fx = lfx * (1.0f / 256.0f);
            for (int c = 0; c < 3; c++) {
                factor[c] += columns[0][c] + (columns[1][c] - columns[0][c]) * fx;
                add[c] = columns[0][3 + c] + (columns[1][3 + c] - columns[0][3 + c]) * fx;
            }
        }

        if (shaded) {
            int sx1 = std::min(sx0 + 1, field.shadeX - 1);
            float fx = sfx * (1.0f / 256.0f);
            float top = shade0[sx0] + (shade0[sx1] - shade0[sx0]) * fx;
            float bottom = shade1[sx0] + (shade1[sx1] - shade1[sx0]) * fx;
            float keep = 1.0f - (top + (bottom - top) * sfy);
            factor[0] *= keep;
            factor[1] *= keep;
            factor[2] *= keep;
        }

        int factor256[3];
        int add8[3];
        for (int c = 0; c < 3; c++) {
            factor256[c] = static_cast<int>(factor[c] * 256.0f + 0.5f);
            add8[c] = static_cast<int>(add[c] + 0.5f);
        }

        uint32_t* block = dest + index * 2;
        for (int row = 0; row < 2; row++) {
            for (int column = 0; column < 2; column++) {
                uint32_t& pixel = block[row * pitch + column];
                pixel = (lightChannel((pixel >> 16) & 0xFF, factor256[0], add8[0]) << 16)
                    | (lightChannel((pixel >> 8) & 0xFF, factor256[1], add8[1]) << 8)
                    | lightChannel(pixel & 0xFF, factor256[2], add8[2]);
            }
        }
    }
}

void hdLightFieldDiff(const HdLightField& a, const HdLightField& b, std::vector<HdLightRect>* rects)
{
    rects->clear();
    if (a.cellsX != b.cellsX || a.cellsY != b.cellsY || a.shadeX != b.shadeX || a.shadeY != b.shadeY) {
        rects->push_back({ 0, 0, std::max(a.width, b.width), std::max(a.height, b.height) });
        return;
    }

    constexpr int kBlock = 16;
    constexpr float kEpsilon = 1.0f / 1024.0f;
    int blocksX = (a.cellsX + kBlock - 1) / kBlock;
    int blocksY = (a.cellsY + kBlock - 1) / kBlock;
    std::vector<unsigned char> changed(static_cast<size_t>(blocksX) * blocksY, 0);

    HdLightBox light = a.lightBox;
    light.add(b.lightBox);
    for (int y = light.y0; y < light.y1; y++) {
        for (int x = light.x0; x < light.x1; x++) {
            size_t index = (static_cast<size_t>(y) * a.cellsX + x) * 3;
            bool differs = false;
            for (int c = 0; c < 3 && !differs; c++) {
                differs = fabsf(a.mul[index + c] - b.mul[index + c]) > kEpsilon
                    || fabsf(a.add[index + c] - b.add[index + c]) > 64.0f * kEpsilon;
            }
            if (differs) {
                changed[static_cast<size_t>(y / kBlock) * blocksX + x / kBlock] = 1;
            }
        }
    }

    constexpr int kShadeRatio = kHdLightCell / kHdShadeCell;
    HdLightBox shade = a.shadeBox;
    shade.add(b.shadeBox);
    for (int y = shade.y0; y < shade.y1; y++) {
        for (int x = shade.x0; x < shade.x1; x++) {
            size_t index = static_cast<size_t>(y) * a.shadeX + x;
            if (fabsf(a.shade[index] - b.shade[index]) > kEpsilon) {
                int bx = std::min(x / kShadeRatio / kBlock, blocksX - 1);
                int by = std::min(y / kShadeRatio / kBlock, blocksY - 1);
                changed[static_cast<size_t>(by) * blocksX + bx] = 1;
            }
        }
    }

    // Merge changed blocks into horizontal runs; every run grows by one cell
    // on each side (a pixel reads the cells around it).
    const int blockPixels = kBlock * kHdLightCell;
    for (int by = 0; by < blocksY; by++) {
        int bx = 0;
        while (bx < blocksX) {
            if (!changed[static_cast<size_t>(by) * blocksX + bx]) {
                bx++;
                continue;
            }
            int start = bx;
            while (bx < blocksX && changed[static_cast<size_t>(by) * blocksX + bx]) {
                bx++;
            }
            int left = std::max(start * blockPixels - kHdLightCell, 0);
            int top = std::max(by * blockPixels - kHdLightCell, 0);
            int right = std::min(bx * blockPixels + kHdLightCell, a.width);
            int bottom = std::min((by + 1) * blockPixels + kHdLightCell, a.height);
            if (right > left && bottom > top) {
                rects->push_back({ left, top, right - left, bottom - top });
            }
        }
    }
}

void hdLightEmissive(const unsigned char* index, const unsigned char* mask, int pitch, int width, int height,
    const unsigned char* palette8, const float* weight, std::vector<float>* grid, HdLightBox* box)
{
    int cellsX = std::max((width + kHdLightCell - 1) / kHdLightCell, 1);
    int cellsY = std::max((height + kHdLightCell - 1) / kHdLightCell, 1);
    grid->assign(static_cast<size_t>(cellsX) * cellsY * 3, 0.0f);
    *box = HdLightBox();

    // Pre-scaled colour per index: weight * colour / (255 * pixels per cell).
    float colour[256][3];
    bool emissive[256];
    const float scale = 1.0f / (255.0f * kHdLightCell * kHdLightCell);
    for (int i = 0; i < 256; i++) {
        emissive[i] = weight[i] > 0.0f;
        for (int c = 0; c < 3; c++) {
            colour[i][c] = weight[i] * palette8[i * 3 + c] * scale;
        }
    }

    int minX = INT32_MAX;
    int minY = INT32_MAX;
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < height; y++) {
        const unsigned char* indexRow = index + static_cast<size_t>(y) * pitch;
        const unsigned char* maskRow = mask + static_cast<size_t>(y) * pitch;
        float* cellRow = grid->data() + static_cast<size_t>(y / kHdLightCell) * cellsX * 3;
        for (int x = 0; x < width; x++) {
            unsigned char value = indexRow[x];
            if (emissive[value] && (maskRow[x] & kHdMaskLit) != 0) {
                int cx = x / kHdLightCell;
                float* cell = cellRow + cx * 3;
                cell[0] += colour[value][0];
                cell[1] += colour[value][1];
                cell[2] += colour[value][2];
                minX = std::min(minX, cx);
                maxX = std::max(maxX, cx);
                minY = std::min(minY, y / kHdLightCell);
                maxY = std::max(maxY, y / kHdLightCell);
            }
        }
    }

    if (maxX >= 0) {
        *box = { minX, minY, maxX + 1, maxY + 1 };
    }
}

void hdLightBlur(std::vector<float>* grid, int cellsX, int cellsY, int radiusX, int radiusY, int passes, HdLightBox* box)
{
    if (box->empty()) {
        return;
    }

    std::vector<float> temp;
    for (int pass = 0; pass < passes; pass++) {
        // What this pass spreads into.
        box->x0 = std::max(box->x0 - radiusX, 0);
        box->x1 = std::min(box->x1 + radiusX, cellsX);
        box->y0 = std::max(box->y0 - radiusY, 0);
        box->y1 = std::min(box->y1 + radiusY, cellsY);
        int spanX = box->x1 - box->x0;
        int spanY = box->y1 - box->y0;
        if (radiusX > 0) {
            for (int y = box->y0; y < box->y1; y++) {
                boxLine(grid->data() + (static_cast<size_t>(y) * cellsX + box->x0) * 3, spanX, 3, radiusX, &temp);
            }
        }
        if (radiusY > 0) {
            for (int x = box->x0; x < box->x1; x++) {
                boxLine(grid->data() + (static_cast<size_t>(box->y0) * cellsX + x) * 3, spanY, cellsX * 3, radiusY, &temp);
            }
        }
    }
}

void hdLightFieldAddGrid(HdLightField* field, const std::vector<float>& grid, const HdLightBox& box, float gain, bool additive)
{
    if (box.empty() || gain == 0.0f) {
        return;
    }

    std::vector<float>& target = additive ? field->add : field->mul;
    for (int y = box.y0; y < box.y1; y++) {
        size_t start = (static_cast<size_t>(y) * field->cellsX + box.x0) * 3;
        size_t end = (static_cast<size_t>(y) * field->cellsX + box.x1) * 3;
        for (size_t index = start; index < end; index++) {
            target[index] += grid[index] * gain;
        }
    }
    field->lightBox.add(box);
}

void hdLightFootprintBuild(const std::vector<HdLightSample>& samples, float kernelX, float kernelY, float exponent,
    HdLightFootprint* footprint)
{
    footprint->weight.clear();
    footprint->cellsX = 0;
    footprint->cellsY = 0;
    if (samples.empty()) {
        return;
    }

    float minX = samples[0].x;
    float maxX = samples[0].x;
    float minY = samples[0].y;
    float maxY = samples[0].y;
    for (const HdLightSample& sample : samples) {
        minX = std::min(minX, sample.x);
        maxX = std::max(maxX, sample.x);
        minY = std::min(minY, sample.y);
        maxY = std::max(maxY, sample.y);
    }

    footprint->cellX = static_cast<int>(floorf((minX - kernelX) / kHdLightCell));
    footprint->cellY = static_cast<int>(floorf((minY - kernelY) / kHdLightCell));
    int lastX = static_cast<int>(ceilf((maxX + kernelX) / kHdLightCell));
    int lastY = static_cast<int>(ceilf((maxY + kernelY) / kHdLightCell));
    footprint->cellsX = lastX - footprint->cellX + 1;
    footprint->cellsY = lastY - footprint->cellY + 1;

    size_t cells = static_cast<size_t>(footprint->cellsX) * footprint->cellsY;
    std::vector<float> sum(cells, 0.0f);
    std::vector<float> norm(cells, 0.0f);

    for (const HdLightSample& sample : samples) {
        int cx0 = static_cast<int>(ceilf((sample.x - kernelX) / kHdLightCell - 0.5f));
        int cx1 = static_cast<int>(floorf((sample.x + kernelX) / kHdLightCell - 0.5f));
        int cy0 = static_cast<int>(ceilf((sample.y - kernelY) / kHdLightCell - 0.5f));
        int cy1 = static_cast<int>(floorf((sample.y + kernelY) / kHdLightCell - 0.5f));
        for (int cy = cy0; cy <= cy1; cy++) {
            float wy = 1.0f - fabsf((cy + 0.5f) * kHdLightCell - sample.y) / kernelY;
            if (wy <= 0.0f) {
                continue;
            }
            int row = cy - footprint->cellY;
            for (int cx = cx0; cx <= cx1; cx++) {
                float wx = 1.0f - fabsf((cx + 0.5f) * kHdLightCell - sample.x) / kernelX;
                if (wx <= 0.0f) {
                    continue;
                }
                float w = wx * wy;
                size_t offset = static_cast<size_t>(row) * footprint->cellsX + (cx - footprint->cellX);
                sum[offset] += w * sample.value;
                norm[offset] += w;
            }
        }
    }

    footprint->weight.assign(cells, 0.0f);
    for (size_t index = 0; index < cells; index++) {
        if (norm[index] > 0.0f) {
            float value = std::clamp(sum[index] / norm[index], 0.0f, 1.0f);
            footprint->weight[index] = powf(value, exponent);
        }
    }
}

void hdLightFieldAddFootprint(HdLightField* field, const HdLightFootprint& footprint, const float* colour, float strength)
{
    int x0 = std::max(footprint.cellX, 0);
    int y0 = std::max(footprint.cellY, 0);
    int x1 = std::min(footprint.cellX + footprint.cellsX, field->cellsX);
    int y1 = std::min(footprint.cellY + footprint.cellsY, field->cellsY);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    field->lightBox.add({ x0, y0, x1, y1 });
    float rgb[3] = { colour[0] * strength, colour[1] * strength, colour[2] * strength };
    for (int y = y0; y < y1; y++) {
        const float* weights = &footprint.weight[static_cast<size_t>(y - footprint.cellY) * footprint.cellsX];
        float* mul = &field->mul[(static_cast<size_t>(y) * field->cellsX) * 3];
        for (int x = x0; x < x1; x++) {
            float w = weights[x - footprint.cellX];
            if (w > 0.0f) {
                mul[x * 3] += w * rgb[0];
                mul[x * 3 + 1] += w * rgb[1];
                mul[x * 3 + 2] += w * rgb[2];
            }
        }
    }
}

void hdLightFieldAddShadow(HdLightField* field, float cx, float cy, float axisX, float axisY, float minor, float darkness)
{
    float major = sqrtf(axisX * axisX + axisY * axisY);
    if (major <= 0.0f || minor <= 0.0f || darkness <= 0.0f) {
        return;
    }

    float ux = axisX / major;
    float uy = axisY / major;
    float extentX = fabsf(axisX) + fabsf(uy) * minor;
    float extentY = fabsf(axisY) + fabsf(ux) * minor;

    int x0 = std::max(static_cast<int>(floorf((cx - extentX) / kHdShadeCell)), 0);
    int x1 = std::min(static_cast<int>(ceilf((cx + extentX) / kHdShadeCell)), field->shadeX - 1);
    int y0 = std::max(static_cast<int>(floorf((cy - extentY) / kHdShadeCell)), 0);
    int y1 = std::min(static_cast<int>(ceilf((cy + extentY) / kHdShadeCell)), field->shadeY - 1);
    if (x0 > x1 || y0 > y1) {
        return;
    }
    field->shadeBox.add({ x0, y0, x1 + 1, y1 + 1 });

    for (int y = y0; y <= y1; y++) {
        float py = (y + 0.5f) * kHdShadeCell - cy;
        for (int x = x0; x <= x1; x++) {
            float px = (x + 0.5f) * kHdShadeCell - cx;
            float along = (px * ux + py * uy) / major;
            float across = (-px * uy + py * ux) / minor;
            float r2 = along * along + across * across;
            if (r2 >= 1.0f) {
                continue;
            }
            float t = 1.0f - r2;
            float amount = darkness * t * t;
            float& shade = field->shade[static_cast<size_t>(y) * field->shadeX + x];
            shade = 1.0f - (1.0f - shade) * (1.0f - amount);
        }
    }
}

} // namespace fallout
