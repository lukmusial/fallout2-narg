#include "map_view.h"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <vector>

#include "debug.h"
#include "input.h"
#include "map.h"
#include "settings.h"
#include "tile.h"
#include "window_manager.h"

namespace fallout {

namespace {

    // Zoom levels (mapViewZoomStep).
    constexpr float kZoomLevels[] = { 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f };

    // Pan margin around the shown part of the surface: two scroll steps.
    constexpr int kMarginX = 64;
    constexpr int kMarginY = 48;

    // The original scroll: a step of 32 x 24 pixels at most every 33 ms.
    constexpr int kStepX = 32;
    constexpr int kStepY = 24;
    constexpr int kStepIntervalMs = 33;

    struct State {
        bool active = false;
        bool zoomEnabled = false;
        bool hd = false;
        int isoWindow = -1;
        int surfaceWindow = -1;
        unsigned char* surfaceBuffer = nullptr;
        int screenWidth = 0;
        int screenHeight = 0;
        int surfaceWidth = 0;
        int surfaceHeight = 0;
        float minZoom = 1.0f;
        float maxZoom = 1.0f;
        int scrollIntervalMs = kStepIntervalMs;

        MapViewMotion motion;
        // The drawn part of the surface (inclusive, engine convention).
        Rect region = { 0, 0, -1, -1 };

        bool smooth = false;
        // mapScroll's step (map.cc) and whether one is being made.
        MapViewScrollStepProc* stepProc = nullptr;
        bool stepping = false;
        // When mapViewScroll was last called (getRealTicks).
        unsigned int lastScroll = 0;

        bool snapshot = false;
        MapViewMotion saved;
        bool snapNext = false;
        bool presentAll = false;
        unsigned int lastTick = 0;

        // The surface composed for display (see MapViewComposite).
        std::vector<uint32_t> rgb;
        std::vector<unsigned char> legacy;
        std::vector<unsigned char> scratch;
        // Size and origin of the wrapped planes above.
        MapViewComposite composite;

        MapViewStats stats;
    };

    State gView;

    double clockMs()
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    MapViewTransform currentTransform()
    {
        MapViewTransform t;
        t.screenWidth = gView.screenWidth;
        t.screenHeight = gView.screenHeight;
        t.surfaceWidth = gView.surfaceWidth;
        t.surfaceHeight = gView.surfaceHeight;
        if (!gView.snapshot) {
            t.zoom = gView.motion.zoom;
            t.panX = mapViewQuantizePan(gView.motion.panX, t.zoom);
            t.panY = mapViewQuantizePan(gView.motion.panY, t.zoom);
        }
        return t;
    }

    bool mapWindowShown()
    {
        Window* window = windowGetWindow(gView.isoWindow);
        return window != nullptr && (window->flags & WINDOW_HIDDEN) == 0;
    }

    // The region needed to show zoom `zoom` with the pan margin (inclusive).
    Rect neededRegion(float zoom)
    {
        MapViewTransform t;
        t.screenWidth = gView.screenWidth;
        t.screenHeight = gView.screenHeight;
        t.surfaceWidth = gView.surfaceWidth;
        t.surfaceHeight = gView.surfaceHeight;
        t.zoom = zoom;
        int left;
        int top;
        int right;
        int bottom;
        mapViewVisibleSurface(t, kMarginX + 2, kMarginY + 2, &left, &top, &right, &bottom);
        Rect rect;
        rect.left = std::max(left, 0);
        rect.top = std::max(top, 0);
        rect.right = std::min(right, gView.surfaceWidth) - 1;
        rect.bottom = std::min(bottom, gView.surfaceHeight) - 1;
        return rect;
    }

    bool rectContains(const Rect& outer, const Rect& inner)
    {
        return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right && inner.bottom <= outer.bottom;
    }

    // Makes the drawn region cover zoom `zoom`; draws it anew when it grows.
    void ensureRegion(float zoom)
    {
        Rect needed = neededRegion(zoom);
        if (rectContains(gView.region, needed)) {
            return;
        }
        Rect grown;
        grown.left = std::min(gView.region.left, needed.left);
        grown.top = std::min(gView.region.top, needed.top);
        grown.right = std::max(gView.region.right, needed.right);
        grown.bottom = std::max(gView.region.bottom, needed.bottom);
        gView.region = grown;
        tileSetWindowRegion(&gView.region);
        tileWindowRefresh();
    }

    // Once the zoom has settled, only what it shows is drawn.
    void shrinkRegion()
    {
        Rect needed = neededRegion(gView.motion.zoom);
        if (needed.left != gView.region.left || needed.top != gView.region.top || needed.right != gView.region.right
            || needed.bottom != gView.region.bottom) {
            if (rectContains(gView.region, needed)) {
                gView.region = needed;
                tileSetWindowRegion(&gView.region);
            }
        }
    }

    HdLayer* surfaceLayer()
    {
        Window* window = windowGetWindow(gView.surfaceWindow);
        return window != nullptr ? window->hdLayer : nullptr;
    }

    void compose(const Rect& rect)
    {
        if (!gView.hd) {
            return;
        }
        int width = rect.right - rect.left + 1;
        int height = rect.bottom - rect.top + 1;
        if (width <= 0 || height <= 0) {
            return;
        }
        double start = clockMs();
        unsigned char basePalette8[256 * 3];
        windowHdGetBasePalette(basePalette8);
        gView.scratch.resize(static_cast<size_t>(width) * height);
        mapViewCompose(surfaceLayer(), gView.surfaceBuffer, gView.surfaceWidth, rect.left, rect.top, width, height, basePalette8,
            gView.composite, gView.rgb.data(), gView.legacy.data(), gView.scratch.data());
        gView.stats.composes++;
        gView.stats.composeMs += clockMs() - start;
    }

    // Shows map window rectangle [left, right) x [top, bottom).
    void present(int left, int top, int right, int bottom)
    {
        if (left >= right || top >= bottom) {
            return;
        }
        double start = clockMs();

        MapViewComposite source = gView.composite;
        HdLayer* layer = nullptr;
        if (gView.hd) {
            source.legacy = gView.legacy.data();
            source.rgb = gView.rgb.data();
            layer = windowHdGetLayer(gView.isoWindow);
        }

        mapViewResample(currentTransform(), source, left, top, right - left, bottom - top, windowGetBuffer(gView.isoWindow),
            gView.screenWidth, layer);
        gView.stats.resampleMs += clockMs() - start;

        Rect rect = { left, top, right - 1, bottom - 1 };
        windowRefreshRect(gView.isoWindow, &rect);

        double ms = clockMs() - start;
        gView.stats.presents++;
        gView.stats.presentMs += ms;
        gView.stats.presentMaxMs = std::max(gView.stats.presentMaxMs, ms);
    }

    void presentAll()
    {
        gView.presentAll = false;
        present(0, 0, gView.screenWidth, gView.screenHeight);
    }

    // Content moves seen since the last scroll (to check the memmove).
    int gMovedX = 0;
    int gMovedY = 0;

} // namespace

bool mapViewInit(int isoWindow)
{
    gView = State();
    bool smooth = settings.graphics.smooth_scroll;
    bool zoom = settings.graphics.zoom;
    if (settings.system.executableIsMapper() || (!smooth && !zoom)) {
        return false;
    }

    gView.isoWindow = isoWindow;
    gView.screenWidth = windowGetWidth(isoWindow);
    gView.screenHeight = windowGetHeight(isoWindow);
    gView.zoomEnabled = zoom;
    gView.minZoom = zoom ? kZoomLevels[0] : 1.0f;
    gView.maxZoom = zoom ? kZoomLevels[sizeof(kZoomLevels) / sizeof(kZoomLevels[0]) - 1] : 1.0f;

    int surfaceWidth = static_cast<int>(ceilf(gView.screenWidth / gView.minZoom)) + 2 * kMarginX + 8;
    int surfaceHeight = static_cast<int>(ceilf(gView.screenHeight / gView.minZoom)) + 2 * kMarginY + 8;
    surfaceWidth += (surfaceWidth - gView.screenWidth) & 1;
    surfaceHeight += (surfaceHeight - gView.screenHeight) & 1;

    gView.surfaceWindow = windowCreate(0, 0, surfaceWidth, surfaceHeight, static_cast<ColorWithFlags>(256),
        WINDOW_HIDDEN | WINDOW_DONT_MOVE_TOP | WINDOW_OFFSCREEN);
    if (gView.surfaceWindow == -1) {
        debugPrint("map view: cannot create the %dx%d surface\n", surfaceWidth, surfaceHeight);
        gView = State();
        return false;
    }
    gView.surfaceBuffer = windowGetBuffer(gView.surfaceWindow);
    gView.surfaceWidth = surfaceWidth;
    gView.surfaceHeight = surfaceHeight;

    int speed = std::clamp(settings.graphics.scroll_speed, 25, 400);
    gView.scrollIntervalMs = std::max(kStepIntervalMs * 100 / speed, 1);
    gView.smooth = smooth;
    gView.motion.stepX = static_cast<float>(kStepX);
    gView.motion.stepY = static_cast<float>(kStepY);
    gView.motion.speedX = kStepX * 1000.0f / gView.scrollIntervalMs;
    gView.motion.speedY = kStepY * 1000.0f / gView.scrollIntervalMs;
    gView.motion.marginX = static_cast<float>(kMarginX);
    gView.motion.marginY = static_cast<float>(kMarginY);

    gView.hd = windowHdIsActive();
    if (gView.hd) {
        gView.rgb.assign(static_cast<size_t>(surfaceWidth) * kHdScale * surfaceHeight * kHdScale, 0);
        gView.legacy.assign(static_cast<size_t>(surfaceWidth) * surfaceHeight, 1);
    }

    gView.composite.width = surfaceWidth;
    gView.composite.height = surfaceHeight;
    gView.composite.index = gView.surfaceBuffer;
    gView.composite.indexPitch = surfaceWidth;

    gView.region = neededRegion(1.0f);
    gView.active = true;
    gView.lastTick = getRealTicks();
    tickersAdd(mapViewTick);

    debugPrint("map view: surface %dx%d for a %dx%d map window, zoom %.2f..%.2f, smooth %d\n", surfaceWidth, surfaceHeight,
        gView.screenWidth, gView.screenHeight, gView.minZoom, gView.maxZoom, smooth ? 1 : 0);
    return true;
}

void mapViewExit()
{
    if (!gView.active) {
        return;
    }
    tickersRemove(mapViewTick);
    windowDestroy(gView.surfaceWindow);
    gView = State();
}

bool mapViewIsActive()
{
    return gView.active;
}

unsigned char* mapViewSurfaceBuffer()
{
    return gView.surfaceBuffer;
}

int mapViewSurfaceWindow()
{
    return gView.surfaceWindow;
}

int mapViewSurfaceWidth()
{
    return gView.surfaceWidth;
}

int mapViewSurfaceHeight()
{
    return gView.surfaceHeight;
}

void mapViewGetRegion(Rect* rect)
{
    *rect = gView.region;
}

int mapViewScrollInterval()
{
    return gView.active ? gView.scrollIntervalMs : kStepIntervalMs;
}

void mapViewSurfaceChanged(const Rect* rect)
{
    if (!gView.active) {
        return;
    }
    Rect clipped;
    if (rectIntersection(rect, &gView.region, &clipped) != 0) {
        return;
    }
    compose(clipped);

    int left;
    int top;
    int right;
    int bottom;
    if (mapViewSurfaceRectToScreen(currentTransform(), clipped.left, clipped.top, clipped.right + 1, clipped.bottom + 1, &left, &top,
            &right, &bottom)) {
        present(left, top, right, bottom);
    }
}

void mapViewSurfaceScrolled(int dx, int dy, const Rect* strips, int stripCount)
{
    if (!gView.active) {
        return;
    }
    double start = clockMs();

    if (gMovedX != -dx || gMovedY != -dy) {
        debugPrint("map view: scroll moved the content by (%d, %d), the centre by (%d, %d)\n", -dx, -dy, gMovedX, gMovedY);
    }
    gMovedX = 0;
    gMovedY = 0;

    mapViewCompositeScroll(&gView.composite, dx, dy);
    for (int index = 0; index < stripCount; index++) {
        Rect clipped;
        if (rectIntersection(&strips[index], &gView.region, &clipped) == 0) {
            compose(clipped);
        }
    }

    gView.stats.scrolls++;
    gView.stats.scrollMs += clockMs() - start;
}

void mapViewContentMoved(int dx, int dy)
{
    if (!gView.active) {
        return;
    }
    gMovedX += dx;
    gMovedY += dy;
    if (gView.snapshot) {
        return;
    }
    if (gView.snapNext || !mapWindowShown()) {
        gView.snapNext = false;
        gView.motion.panX = 0.0f;
        gView.motion.panY = 0.0f;
        gView.motion.remainX = 0.0f;
        gView.motion.remainY = 0.0f;
        gView.presentAll = true;
        return;
    }
    if (!gView.smooth) {
        // Zoom only: centre changes show at once.
        gView.presentAll = true;
        return;
    }
    float expectedX = gView.motion.panX + dx;
    float expectedY = gView.motion.panY + dy;
    gView.motion.contentMoved(static_cast<float>(dx), static_cast<float>(dy), gView.stepping);
    if (gView.motion.panX != expectedX || gView.motion.panY != expectedY) {
        // Clamped: the picture moves.
        gView.presentAll = true;
    }
}

namespace {

    int scrollStep(int dx, int dy, void* context)
    {
        (void)context;
        if (gView.stepProc == nullptr) {
            return -1;
        }
        gView.stepping = true;
        gMovedX = 0;
        gMovedY = 0;
        int rc = gView.stepProc(dx, dy);
        gView.stepping = false;
        return rc;
    }

} // namespace

void mapViewSetScrollStepProc(MapViewScrollStepProc* proc)
{
    gView.stepProc = proc;
}

bool mapViewSmoothScrolling()
{
    return gView.active && gView.smooth && !gView.snapshot;
}

int mapViewScroll(int dx, int dy)
{
    if (!mapViewSmoothScrolling()) {
        return scrollStep(dx, dy, nullptr);
    }
    // A single request travels a step; while requests keep coming, each
    // travels as far as the speed allows since the previous one.
    unsigned int now = getRealTicks();
    unsigned int elapsed = getTicksBetween(now, gView.lastScroll);
    float amount = elapsed > static_cast<unsigned int>(2 * gView.scrollIntervalMs)
        ? 1.0f
        : std::min(static_cast<float>(elapsed) / gView.scrollIntervalMs, 1.0f);
    gView.lastScroll = now;
    int rc = mapViewMotionScroll(&gView.motion, dx, dy, amount, scrollStep, nullptr);
    if (rc != 0) {
        gView.presentAll = true;
    }
    return rc;
}

void mapViewReset()
{
    if (!gView.active) {
        return;
    }
    gView.snapNext = true;
    gView.motion.panX = 0.0f;
    gView.motion.panY = 0.0f;
    gView.motion.remainX = 0.0f;
    gView.motion.remainY = 0.0f;
    gMovedX = 0;
    gMovedY = 0;
    if (gView.hd) {
        std::fill(gView.legacy.begin(), gView.legacy.end(), 1);
    }
}

void mapViewTick()
{
    if (!gView.active) {
        return;
    }
    unsigned int now = getRealTicks();
    float seconds = std::min(getTicksBetween(now, gView.lastTick) / 1000.0f, 0.1f);
    gView.lastTick = now;
    if (!mapWindowShown()) {
        return;
    }

    gMovedX = 0;
    gMovedY = 0;

    // Requests stopped: travel on to the centre the engine is at.
    if (getTicksBetween(now, gView.lastScroll) > static_cast<unsigned int>(2 * gView.scrollIntervalMs)) {
        gView.motion.finishAtCentre();
    }

    bool moved = !gView.snapshot && mapViewMotionUpdate(&gView.motion, seconds, scrollStep, nullptr);
    if (gView.motion.zoom == gView.motion.targetZoom && !gView.snapshot) {
        shrinkRegion();
    }
    if (moved || gView.presentAll) {
        presentAll();
    }
}

void mapViewScreenToSurface(int* x, int* y)
{
    if (!gView.active) {
        return;
    }
    mapViewPixelToSurface(currentTransform(), *x, *y, x, y);
}

void mapViewSurfaceToScreen(float* x, float* y)
{
    if (!gView.active) {
        return;
    }
    mapViewToScreen(currentTransform(), *x, *y, x, y);
}

void mapViewGetTransform(MapViewTransform* transform)
{
    if (gView.active) {
        *transform = currentTransform();
        return;
    }
    *transform = MapViewTransform();
    transform->screenWidth = transform->surfaceWidth = windowGetWidth(gIsoWindow);
    transform->screenHeight = transform->surfaceHeight = windowGetHeight(gIsoWindow);
}

float mapViewGetZoom()
{
    return gView.active && !gView.snapshot ? gView.motion.zoom : 1.0f;
}

float mapViewGetTargetZoom()
{
    return gView.active ? gView.motion.targetZoom : 1.0f;
}

bool mapViewZoomEnabled()
{
    return gView.active && gView.zoomEnabled;
}

void mapViewSetZoom(float zoom, bool animate)
{
    if (!mapViewZoomEnabled()) {
        return;
    }
    zoom = std::clamp(zoom, gView.minZoom, gView.maxZoom);
    if (gView.snapshot) {
        gView.saved.targetZoom = zoom;
        if (!animate) {
            gView.saved.zoom = zoom;
        }
        return;
    }
    gView.motion.targetZoom = zoom;
    if (!animate) {
        gView.motion.zoom = zoom;
    }
    if (mapWindowShown()) {
        ensureRegion(std::min(gView.motion.zoom, zoom));
    }
    gView.presentAll = true;
}

void mapViewZoomStep(int direction)
{
    if (!mapViewZoomEnabled() || direction == 0) {
        return;
    }
    const int count = static_cast<int>(sizeof(kZoomLevels) / sizeof(kZoomLevels[0]));
    float current = gView.snapshot ? gView.saved.targetZoom : gView.motion.targetZoom;
    int nearest = 0;
    for (int index = 1; index < count; index++) {
        if (fabsf(kZoomLevels[index] - current) < fabsf(kZoomLevels[nearest] - current)) {
            nearest = index;
        }
    }
    int next = std::clamp(nearest + (direction > 0 ? 1 : -1), 0, count - 1);
    mapViewSetZoom(kZoomLevels[next], true);
}

void mapViewZoomReset()
{
    mapViewSetZoom(1.0f, true);
}

void mapViewBeginSnapshot()
{
    if (!gView.active || gView.snapshot) {
        return;
    }
    gView.saved = gView.motion;
    gView.snapshot = true;
    gView.motion.panX = 0.0f;
    gView.motion.panY = 0.0f;
    gView.motion.remainX = 0.0f;
    gView.motion.remainY = 0.0f;
    gView.motion.zoom = 1.0f;
    gView.motion.targetZoom = 1.0f;
    if (mapWindowShown()) {
        presentAll();
    }
}

void mapViewEndSnapshot()
{
    if (!gView.active || !gView.snapshot) {
        return;
    }
    gView.snapshot = false;
    gView.motion.zoom = gView.saved.zoom;
    gView.motion.targetZoom = gView.saved.targetZoom;
    gView.motion.panX = 0.0f;
    gView.motion.panY = 0.0f;
    gView.motion.remainX = 0.0f;
    gView.motion.remainY = 0.0f;
    if (mapWindowShown()) {
        ensureRegion(std::min(gView.motion.zoom, gView.motion.targetZoom));
    }
    gView.presentAll = true;
}

bool mapViewIsMoving()
{
    return gView.active && !gView.motion.settled();
}

void mapViewGetStats(MapViewStats* stats)
{
    *stats = gView.stats;
}

void mapViewResetStats()
{
    gView.stats = MapViewStats();
}

} // namespace fallout
