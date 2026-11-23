#include "map_view_core.h"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <vector>

namespace fallout {

void mapViewPixelToSurface(const MapViewTransform& t, int x, int y, int* mx, int* my)
{
    float fx;
    float fy;
    mapViewToSurface(t, x + 0.5f, y + 0.5f, &fx, &fy);
    *mx = static_cast<int>(floorf(fx));
    *my = static_cast<int>(floorf(fy));
}

void mapViewVisibleSurface(const MapViewTransform& t, int marginX, int marginY, int* left, int* top, int* right, int* bottom)
{
    float x0;
    float y0;
    float x1;
    float y1;
    mapViewToSurface(t, 0.0f, 0.0f, &x0, &y0);
    mapViewToSurface(t, static_cast<float>(t.screenWidth), static_cast<float>(t.screenHeight), &x1, &y1);
    *left = static_cast<int>(floorf(x0)) - marginX;
    *top = static_cast<int>(floorf(y0)) - marginY;
    *right = static_cast<int>(ceilf(x1)) + marginX;
    *bottom = static_cast<int>(ceilf(y1)) + marginY;
}

bool mapViewSurfaceRectToScreen(const MapViewTransform& t, int left, int top, int right, int bottom,
    int* outLeft, int* outTop, int* outRight, int* outBottom)
{
    float x0;
    float y0;
    float x1;
    float y1;
    mapViewToScreen(t, static_cast<float>(left), static_cast<float>(top), &x0, &y0);
    mapViewToScreen(t, static_cast<float>(right), static_cast<float>(bottom), &x1, &y1);
    // One more pixel around: bilinear samples reach half a pixel further.
    int sl = static_cast<int>(floorf(x0)) - 1;
    int st = static_cast<int>(floorf(y0)) - 1;
    int sr = static_cast<int>(ceilf(x1)) + 1;
    int sb = static_cast<int>(ceilf(y1)) + 1;
    *outLeft = std::max(sl, 0);
    *outTop = std::max(st, 0);
    *outRight = std::min(sr, t.screenWidth);
    *outBottom = std::min(sb, t.screenHeight);
    return *outLeft < *outRight && *outTop < *outBottom;
}

float mapViewQuantizePan(float pan, float zoom)
{
    if (zoom != 1.0f) {
        return pan;
    }
    return roundf(pan * kHdScale) / kHdScale;
}

void MapViewMotion::request(int dx, int dy, float amount)
{
    // At most a step and a half pending: the view stops soon after the
    // requests stop.
    if (dx != 0) {
        if (remainX * dx < 0.0f) {
            remainX = 0.0f;
        }
        remainX = std::clamp(remainX + dx * stepX * amount, -1.5f * stepX, 1.5f * stepX);
        directionX = dx > 0 ? 1 : -1;
    }
    if (dy != 0) {
        if (remainY * dy < 0.0f) {
            remainY = 0.0f;
        }
        remainY = std::clamp(remainY + dy * stepY * amount, -1.5f * stepY, 1.5f * stepY);
        directionY = dy > 0 ? 1 : -1;
    }
}

void MapViewMotion::contentMoved(float dx, float dy, bool scrolling)
{
    panX = std::clamp(panX + dx, -marginX, marginX);
    panY = std::clamp(panY + dy, -marginY, marginY);
    if (!scrolling) {
        // Glide back to the (new) centre.
        remainX = -panX;
        remainY = -panY;
        directionX = 0;
        directionY = 0;
    }
}

bool MapViewMotion::advance(float seconds)
{
    if (seconds <= 0.0f) {
        return false;
    }

    bool changed = false;
    auto move = [&](float* pan, float* remain, int* direction, float speed, float step) {
        if (*remain == 0.0f) {
            return;
        }
        // The usual speed, faster when more than a step is pending (a
        // frame came late, or a jump).
        float distance = speed * std::max(1.0f, fabsf(*remain) / step) * seconds;
        float moved = std::min(fabsf(*remain), distance);
        int sign = *remain > 0.0f ? 1 : -1;
        *pan += moved * sign;
        *remain -= moved * sign;
        if (fabsf(*remain) < 1e-3f) {
            *remain = 0.0f;
        }
        *direction = sign;
        changed = true;
    };
    move(&panX, &remainX, &directionX, speedX, stepX);
    move(&panY, &remainY, &directionY, speedY, stepY);

    if (zoom != targetZoom) {
        // In log space, so zooming in and out feel alike.
        float from = logf(zoom);
        float to = logf(targetZoom);
        float k = zoomTime > 0.0f ? 1.0f - expf(-seconds / zoomTime) : 1.0f;
        float next = from + (to - from) * k;
        if (fabsf(to - next) < 0.004f) {
            zoom = targetZoom;
        } else {
            zoom = expf(next);
        }
        changed = true;
    }
    return changed;
}

int MapViewMotion::stepNeededX() const
{
    return directionX != 0 && panX * directionX > 0.0f ? directionX : 0;
}

int MapViewMotion::stepNeededY() const
{
    return directionY != 0 && panY * directionY > 0.0f ? directionY : 0;
}

void MapViewMotion::blockX()
{
    panX = std::min(std::max(panX, -marginX), marginX);
    if (panX * directionX > 0.0f) {
        panX = 0.0f;
    }
    remainX = 0.0f;
    directionX = 0;
}

void MapViewMotion::finishAtCentre()
{
    if (directionX != 0 && panX * directionX < 0.0f && remainX * directionX < -panX * directionX) {
        remainX = -panX;
    }
    if (directionY != 0 && panY * directionY < 0.0f && remainY * directionY < -panY * directionY) {
        remainY = -panY;
    }
}

void MapViewMotion::blockY()
{
    panY = std::min(std::max(panY, -marginY), marginY);
    if (panY * directionY > 0.0f) {
        panY = 0.0f;
    }
    remainY = 0.0f;
    directionY = 0;
}

int mapViewMotionScroll(MapViewMotion* motion, int dx, int dy, float amount, MapViewStepProc* step, void* context)
{
    if (dx == 0 && dy == 0) {
        return -1;
    }

    // Axes where the view is at or past the centre in the scroll direction
    // need the step now; elsewhere the engine is already a step ahead.
    int nowX = dx != 0 && motion->panX * dx >= 0.0f ? dx : 0;
    int nowY = dy != 0 && motion->panY * dy >= 0.0f ? dy : 0;
    if (nowX != 0 || nowY != 0) {
        // The engine refuses steps the original would refuse.
        motion->directionX = dx != 0 ? (dx > 0 ? 1 : -1) : motion->directionX;
        motion->directionY = dy != 0 ? (dy > 0 ? 1 : -1) : motion->directionY;
        if (step(nowX, nowY, context) != 0) {
            if (nowX != 0) {
                motion->blockX();
                dx = 0;
            }
            if (nowY != 0) {
                motion->blockY();
                dy = 0;
            }
            if (dx == 0 && dy == 0) {
                return -1;
            }
        }
    }
    motion->request(dx, dy, amount);
    return 0;
}

bool mapViewMotionUpdate(MapViewMotion* motion, float seconds, MapViewStepProc* step, void* context)
{
    bool changed = motion->advance(seconds);
    // A frame never moves more than a step or two.
    for (int attempt = 0; attempt < 4; attempt++) {
        int sx = motion->stepNeededX();
        int sy = motion->stepNeededY();
        if (sx == 0 && sy == 0) {
            break;
        }
        if (step(sx, sy, context) != 0) {
            if (sx != 0) {
                motion->blockX();
            }
            if (sy != 0) {
                motion->blockY();
            }
            changed = true;
        }
    }
    return changed;
}

namespace {

    inline int wrap(int value, int size)
    {
        int result = value % size;
        return result < 0 ? result + size : result;
    }

} // namespace

void mapViewCompose(const HdLayer* layer, const unsigned char* buffer, int bufferPitch, int x, int y, int width, int height,
    const unsigned char* basePalette8, const MapViewComposite& composite, uint32_t* rgb, unsigned char* legacy,
    unsigned char* scratch)
{
    if (width <= 0 || height <= 0 || composite.width <= 0 || composite.height <= 0) {
        return;
    }

    // Pieces of the rectangle that are contiguous in the wrapped planes.
    const int rgbPitch = composite.width * kHdScale;
    int column = 0;
    while (column < width) {
        int destX = wrap(x + column + composite.originX, composite.width);
        int pieceWidth = std::min(width - column, composite.width - destX);
        int row = 0;
        while (row < height) {
            int destY = wrap(y + row + composite.originY, composite.height);
            int pieceHeight = std::min(height - row, composite.height - destY);

            uint32_t* rgbOrigin = rgb + static_cast<size_t>(destY) * kHdScale * rgbPitch + static_cast<size_t>(destX) * kHdScale;
            hdComposeRect(layer, buffer, bufferPitch, x + column, y + row, pieceWidth, pieceHeight, scratch, pieceWidth, rgbOrigin,
                rgbPitch, false, basePalette8);

            for (int r = 0; r < pieceHeight; r++) {
                const unsigned char* src = buffer + static_cast<size_t>(y + row + r) * bufferPitch + x + column;
                const unsigned char* mask = scratch + static_cast<size_t>(r) * pieceWidth;
                unsigned char* legacyRow = legacy + static_cast<size_t>(destY + r) * composite.width + destX;
                uint32_t* rgbRow = rgbOrigin + static_cast<size_t>(r) * kHdScale * rgbPitch;
                for (int c = 0; c < pieceWidth; c++) {
                    unsigned char index = src[c];
                    if (mask[c] != 0) {
                        legacyRow[c] = 0;
                        continue;
                    }
                    legacyRow[c] = hdIsCyclingIndex(index) ? 1 : 0;
                    const unsigned char* entry = basePalette8 + index * 3;
                    uint32_t colour = (static_cast<uint32_t>(entry[0]) << 16) | (static_cast<uint32_t>(entry[1]) << 8) | entry[2];
                    for (int sy = 0; sy < kHdScale; sy++) {
                        for (int sx = 0; sx < kHdScale; sx++) {
                            rgbRow[sy * rgbPitch + c * kHdScale + sx] = colour;
                        }
                    }
                }
            }
            row += pieceHeight;
        }
        column += pieceWidth;
    }
}

void mapViewCompositeScroll(MapViewComposite* composite, int dx, int dy)
{
    if (composite->width <= 0 || composite->height <= 0) {
        return;
    }
    composite->originX = wrap(composite->originX + dx, composite->width);
    composite->originY = wrap(composite->originY + dy, composite->height);
}

namespace {

    // a + (b - a) * f / 256 on packed 0x00RRGGBB.
    inline uint32_t lerpPacked(uint32_t a, uint32_t b, uint32_t f)
    {
        uint32_t g = 256 - f;
        uint32_t rb = (((a & 0xFF00FF) * g + (b & 0xFF00FF) * f) >> 8) & 0xFF00FF;
        uint32_t gr = (((a & 0x00FF00) * g + (b & 0x00FF00) * f) >> 8) & 0x00FF00;
        return rb | gr;
    }

} // namespace

void mapViewResample(const MapViewTransform& t, const MapViewComposite& source, int x, int y, int width, int height,
    unsigned char* destIndex, int destPitch, HdLayer* layer)
{
    if (width <= 0 || height <= 0 || source.width <= 0 || source.height <= 0) {
        return;
    }

    const float zoom = t.zoom;
    // Surface coordinate at map window coordinate 0.
    const float originX = t.surfaceWidth * 0.5f + t.panX - t.screenWidth * 0.5f / zoom;
    const float originY = t.surfaceHeight * 0.5f + t.panY - t.screenHeight * 0.5f / zoom;

    // Nearest surface pixel per map window column (and its wrapped column in
    // the composite).
    std::vector<int> columns(width);
    std::vector<int> wrappedColumns(width);
    for (int column = 0; column < width; column++) {
        int mx = static_cast<int>(floorf(originX + (x + column + 0.5f) / zoom));
        columns[column] = std::clamp(mx, 0, source.width - 1);
        wrappedColumns[column] = wrap(columns[column] + source.originX, source.width);
    }

    bool hd = layer != nullptr && source.rgb != nullptr && source.legacy != nullptr;

    for (int row = 0; row < height; row++) {
        int my = std::clamp(static_cast<int>(floorf(originY + (y + row + 0.5f) / zoom)), 0, source.height - 1);
        const unsigned char* srcIndex = source.index + static_cast<size_t>(my) * source.indexPitch;
        unsigned char* dest = destIndex + static_cast<size_t>(y + row) * destPitch + x;
        for (int column = 0; column < width; column++) {
            dest[column] = srcIndex[columns[column]];
        }

        if (!hd) {
            continue;
        }

        const unsigned char* srcLegacy = source.legacy + static_cast<size_t>(wrap(my + source.originY, source.height)) * source.width;
        size_t offset = static_cast<size_t>(y + row) * layer->width + x;
        for (int column = 0; column < width; column++) {
            layer->shadow[offset + column] = dest[column];
            layer->valid[offset + column] = srcLegacy[wrappedColumns[column]] != 0 ? 0 : 1;
        }
        if (!layer->textGuard.empty() && y + row >= layer->textTop && y + row < layer->textBottom) {
            std::fill(layer->textGuard.begin() + offset, layer->textGuard.begin() + offset + width, 0);
        }
    }

    if (!hd) {
        return;
    }

    const int srcWidth2 = source.width * kHdScale;
    const int srcHeight2 = source.height * kHdScale;
    const int wrapX2 = source.originX * kHdScale;
    const int wrapY2 = source.originY * kHdScale;
    const int destPitch2 = layer->width * kHdScale;
    const float scale = 1.0f / zoom;
    const float baseU = originX * kHdScale + 0.5f * scale - 0.5f;
    const float baseV = originY * kHdScale + 0.5f * scale - 0.5f;
    auto sourceRow = [&](int v) {
        v = std::clamp(v, 0, srcHeight2 - 1);
        return source.rgb + static_cast<size_t>(wrap(v + wrapY2, srcHeight2)) * srcWidth2;
    };

    // A whole-output-pixel shift at zoom 1: copy.
    float shiftU = roundf(baseU);
    float shiftV = roundf(baseV);
    if (zoom == 1.0f && fabsf(baseU - shiftU) < 1e-3f && fabsf(baseV - shiftV) < 1e-3f) {
        int du = static_cast<int>(shiftU);
        int dv = static_cast<int>(shiftV);
        int first = x * kHdScale;
        int last = (x + width) * kHdScale;
        for (int row = y * kHdScale; row < (y + height) * kHdScale; row++) {
            uint32_t* out = layer->pixels.data() + static_cast<size_t>(row) * destPitch2;
            const uint32_t* in = sourceRow(row + dv);
            int column = first;
            while (column < last) {
                int u = std::clamp(column + du, 0, srcWidth2 - 1);
                int stored = wrap(u + wrapX2, srcWidth2);
                // A run that stays inside the source and does not wrap.
                int run = std::min(last - column, srcWidth2 - stored);
                if (column + du < 0 || column + du >= srcWidth2) {
                    run = 1;
                } else {
                    run = std::min(run, srcWidth2 - (column + du));
                }
                memcpy(out + column, in + stored, sizeof(uint32_t) * run);
                column += run;
            }
        }
        return;
    }

    // Bilinear.
    const int width2 = width * kHdScale;
    std::vector<int> u0(width2);
    std::vector<int> u1(width2);
    std::vector<uint32_t> fu(width2);
    for (int column = 0; column < width2; column++) {
        float u = baseU + (x * kHdScale + column) * scale;
        float fl = floorf(u);
        int i = static_cast<int>(fl);
        fu[column] = static_cast<uint32_t>((u - fl) * 256.0f);
        u0[column] = wrap(std::clamp(i, 0, srcWidth2 - 1) + wrapX2, srcWidth2);
        u1[column] = wrap(std::clamp(i + 1, 0, srcWidth2 - 1) + wrapX2, srcWidth2);
    }

    // Source rows interpolated horizontally, kept for the next output rows
    // (zoomed in, several output rows read the same source rows).
    std::vector<uint32_t> rows[2] = { std::vector<uint32_t>(width2), std::vector<uint32_t>(width2) };
    int rowSource[2] = { -1, -1 };
    auto horizontal = [&](int v) -> const uint32_t* {
        v = std::clamp(v, 0, srcHeight2 - 1);
        for (int slot = 0; slot < 2; slot++) {
            if (rowSource[slot] == v) {
                return rows[slot].data();
            }
        }
        // Replace the slot not holding v - 1 (the other row of the pair).
        int slot = rowSource[0] == v - 1 ? 1 : 0;
        rowSource[slot] = v;
        const uint32_t* in = sourceRow(v);
        uint32_t* out = rows[slot].data();
        for (int column = 0; column < width2; column++) {
            out[column] = lerpPacked(in[u0[column]], in[u1[column]], fu[column]);
        }
        return out;
    };

    for (int row = y * kHdScale; row < (y + height) * kHdScale; row++) {
        float v = baseV + row * scale;
        float fl = floorf(v);
        int i = static_cast<int>(fl);
        uint32_t fv = static_cast<uint32_t>((v - fl) * 256.0f);
        const uint32_t* top = horizontal(i);
        const uint32_t* bottom = horizontal(i + 1);
        uint32_t* out = layer->pixels.data() + static_cast<size_t>(row) * destPitch2 + x * kHdScale;
        if (fv == 0) {
            memcpy(out, top, sizeof(uint32_t) * width2);
            continue;
        }
        for (int column = 0; column < width2; column++) {
            out[column] = lerpPacked(top[column], bottom[column], fv);
        }
    }
}

} // namespace fallout
