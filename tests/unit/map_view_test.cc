// Unit tests for the map view (src/map_view_core.cc, Phase 2.13): the
// surface/screen transform, smooth pan and zoom motion, resampling of the
// composed surface into the map window, and hdLayerScrollRect. No game data.

#include "map_view_core.h"

#include <math.h>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>

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

static MapViewTransform makeTransform(float zoom, float panX, float panY)
{
    MapViewTransform t;
    t.screenWidth = 64;
    t.screenHeight = 40;
    t.surfaceWidth = 64 * 2 + 16;
    t.surfaceHeight = 40 * 2 + 12;
    t.zoom = zoom;
    t.panX = panX;
    t.panY = panY;
    return t;
}

static void testTransform()
{
    MapViewTransform t = makeTransform(1.0f, 0.0f, 0.0f);
    int mx;
    int my;
    mapViewPixelToSurface(t, 0, 0, &mx, &my);
    CHECK(mx == (t.surfaceWidth - t.screenWidth) / 2);
    CHECK(my == (t.surfaceHeight - t.screenHeight) / 2);
    mapViewPixelToSurface(t, 63, 39, &mx, &my);
    CHECK(mx == 63 + (t.surfaceWidth - t.screenWidth) / 2);
    CHECK(my == 39 + (t.surfaceHeight - t.screenHeight) / 2);

    // Zoom keeps the screen centre on the surface centre (plus the pan).
    for (float zoom : { 0.5f, 0.75f, 1.0f, 1.5f, 2.0f }) {
        MapViewTransform z = makeTransform(zoom, 3.5f, -2.0f);
        float sx;
        float sy;
        mapViewToSurface(z, z.screenWidth * 0.5f, z.screenHeight * 0.5f, &sx, &sy);
        CHECK(fabsf(sx - (z.surfaceWidth * 0.5f + 3.5f)) < 1e-4f);
        CHECK(fabsf(sy - (z.surfaceHeight * 0.5f - 2.0f)) < 1e-4f);
        // Inverse.
        float bx;
        float by;
        mapViewToScreen(z, 17.25f, 30.5f, &sx, &sy);
        mapViewToSurface(z, sx, sy, &bx, &by);
        CHECK(fabsf(bx - 17.25f) < 1e-3f && fabsf(by - 30.5f) < 1e-3f);
        // Distances scale with the zoom.
        float ax;
        float ay;
        mapViewToScreen(z, 10.0f, 10.0f, &ax, &ay);
        mapViewToScreen(z, 20.0f, 10.0f, &bx, &by);
        CHECK(fabsf((bx - ax) - 10.0f * zoom) < 1e-3f);
    }

    // Surface rectangles map to the screen rectangle that shows them.
    MapViewTransform z = makeTransform(2.0f, 0.0f, 0.0f);
    int l;
    int tp;
    int r;
    int b;
    CHECK(mapViewSurfaceRectToScreen(z, 72, 46, 74, 47, &l, &tp, &r, &b));
    CHECK(l <= 32 && r >= 36 && tp <= 20 && b >= 22);
    CHECK(!mapViewSurfaceRectToScreen(z, 0, 0, 4, 4, &l, &tp, &r, &b));

    // The visible part grows when zooming out.
    int left1;
    int top1;
    int right1;
    int bottom1;
    mapViewVisibleSurface(makeTransform(1.0f, 0.0f, 0.0f), 0, 0, &left1, &top1, &right1, &bottom1);
    CHECK(right1 - left1 == 64 && bottom1 - top1 == 40);
    mapViewVisibleSurface(makeTransform(0.5f, 0.0f, 0.0f), 0, 0, &left1, &top1, &right1, &bottom1);
    CHECK(right1 - left1 == 128 && bottom1 - top1 == 80);

    CHECK(mapViewQuantizePan(3.3f, 1.0f) == 3.5f);
    CHECK(mapViewQuantizePan(3.3f, 2.0f) == 3.3f);
}

// A stand-in for the engine: the centre moves in whole steps, refused past
// `limit` steps to the right.
struct FakeEngine {
    MapViewMotion* motion;
    int centre = 0;
    int limit = 1000;
    int steps = 0;
};

static int fakeStep(int dx, int dy, void* context)
{
    FakeEngine* engine = static_cast<FakeEngine*>(context);
    if (engine->centre + dx > engine->limit) {
        return -1;
    }
    engine->centre += dx;
    engine->steps++;
    engine->motion->contentMoved(-dx * engine->motion->stepX, -dy * engine->motion->stepY, true);
    return 0;
}

// Where the view is, in content pixels.
static float viewPosition(const FakeEngine& engine)
{
    return engine.centre * engine.motion->stepX + engine.motion->panX;
}

static void testMotion()
{
    MapViewMotion motion;
    motion.speedX = 32.0f * 1000.0f / 33.0f;
    motion.speedY = 24.0f * 1000.0f / 33.0f;
    CHECK(motion.settled());
    CHECK(!motion.advance(0.016f));

    // One scroll step: the engine steps at once, the view follows over
    // about one step interval and stops exactly on the new centre.
    FakeEngine engine;
    engine.motion = &motion;
    CHECK(mapViewMotionScroll(&motion, 1, 0, 1.0f, fakeStep, &engine) == 0);
    CHECK(engine.centre == 1);
    CHECK(viewPosition(engine) == 0.0f);
    for (int frame = 0; frame < 10; frame++) {
        mapViewMotionUpdate(&motion, 1.0f / 60.0f, fakeStep, &engine);
    }
    CHECK(engine.centre == 1);
    CHECK(motion.settled());
    CHECK(viewPosition(engine) == 32.0f);

    // A held key at 60 frames per second (a request whenever 33 ms have
    // passed): the view moves the same distance every frame, at the
    // original speed, and the engine stays at most a step ahead.
    float previous = viewPosition(engine);
    float minDelta = 1e9f;
    float maxDelta = 0.0f;
    float lastRequest = -1.0f;
    for (int frame = 0; frame < 120; frame++) {
        float now = frame / 60.0f;
        if (lastRequest < 0.0f || now - lastRequest >= 0.033f) {
            lastRequest = now;
            mapViewMotionScroll(&motion, 1, 0, 1.0f, fakeStep, &engine);
        }
        mapViewMotionUpdate(&motion, 1.0f / 60.0f, fakeStep, &engine);
        float position = viewPosition(engine);
        if (frame > 2) {
            minDelta = std::min(minDelta, position - previous);
            maxDelta = std::max(maxDelta, position - previous);
        }
        previous = position;
        CHECK(motion.panX >= -motion.stepX - 0.01f && motion.panX <= 0.01f);
    }
    CHECK(minDelta > 15.0f && maxDelta < 17.0f);

    // Requests at irregular times (each travels the time since the previous
    // one, as mapViewScroll asks): still the same distance every frame.
    previous = viewPosition(engine);
    minDelta = 1e9f;
    maxDelta = 0.0f;
    float lastTime = 0.0f;
    float time = 0.0f;
    for (int frame = 0; frame < 120; frame++) {
        time += 1.0f / 60.0f;
        // Requests on 2 of 3 frames, as a 30 Hz key repeat beats with 60 Hz.
        if (frame % 3 != 2) {
            mapViewMotionScroll(&motion, 1, 0, (time - lastTime) / 0.033f, fakeStep, &engine);
            lastTime = time;
        }
        mapViewMotionUpdate(&motion, 1.0f / 60.0f, fakeStep, &engine);
        float position = viewPosition(engine);
        if (frame > 2) {
            minDelta = std::min(minDelta, position - previous);
            maxDelta = std::max(maxDelta, position - previous);
        }
        previous = position;
    }
    CHECK(minDelta > 15.0f && maxDelta < 17.0f);

    // Releasing the key: the view finishes the step and rests on the centre.
    motion.finishAtCentre();
    for (int frame = 0; frame < 10; frame++) {
        mapViewMotionUpdate(&motion, 1.0f / 60.0f, fakeStep, &engine);
    }
    CHECK(motion.settled());
    CHECK(viewPosition(engine) == engine.centre * 32.0f);

    // A scroll limit: the view stops exactly where the engine stops, and
    // the request reports the block.
    engine.limit = engine.centre + 2;
    int blocked = 0;
    for (int frame = 0; frame < 60; frame++) {
        if (frame % 2 == 0 && mapViewMotionScroll(&motion, 1, 0, 1.0f, fakeStep, &engine) == -1) {
            blocked++;
        }
        mapViewMotionUpdate(&motion, 1.0f / 60.0f, fakeStep, &engine);
    }
    CHECK(engine.centre == engine.limit);
    CHECK(blocked > 10);
    CHECK(motion.settled());
    CHECK(viewPosition(engine) == engine.limit * 32.0f);

    // A jump of the centre (to the player) glides back, clamped to the
    // margin.
    motion.contentMoved(-500.0f, 300.0f, false);
    CHECK(motion.panX == -motion.marginX);
    CHECK(motion.panY == motion.marginY);
    for (int frame = 0; frame < 30; frame++) {
        mapViewMotionUpdate(&motion, 1.0f / 60.0f, fakeStep, &engine);
    }
    CHECK(motion.settled());
    CHECK(engine.centre == engine.limit);

    // Zoom converges on the target, in both directions.
    MapViewMotion zoom;
    zoom.targetZoom = 2.0f;
    float last = zoom.zoom;
    int frames = 0;
    while (!zoom.settled() && frames < 200) {
        zoom.advance(1.0f / 60.0f);
        CHECK(zoom.zoom >= last && zoom.zoom <= 2.0f);
        last = zoom.zoom;
        frames++;
    }
    CHECK(zoom.zoom == 2.0f);
    CHECK(frames > 3 && frames < 40);
    zoom.targetZoom = 0.5f;
    frames = 0;
    while (!zoom.settled() && frames < 200) {
        zoom.advance(1.0f / 60.0f);
        frames++;
    }
    CHECK(zoom.zoom == 0.5f);
}

// A composite with an index and a colour per logical pixel (each 2x2 block
// one colour) and every 7th pixel legacy.
struct TestComposite {
    int width;
    int height;
    std::vector<unsigned char> index;
    std::vector<unsigned char> legacy;
    std::vector<uint32_t> rgb;

    TestComposite(int w, int h)
        : width(w)
        , height(h)
        , index(static_cast<size_t>(w) * h)
        , legacy(static_cast<size_t>(w) * h)
        , rgb(static_cast<size_t>(w) * h * 4)
    {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                size_t offset = static_cast<size_t>(y) * w + x;
                index[offset] = static_cast<unsigned char>((x * 7 + y * 13) % 200 + 1);
                legacy[offset] = offset % 7 == 0 ? 1 : 0;
                uint32_t colour = (static_cast<uint32_t>(x * 3 % 256) << 16) | (static_cast<uint32_t>(y * 5 % 256) << 8) | ((x + y) % 256);
                for (int sy = 0; sy < 2; sy++) {
                    for (int sx = 0; sx < 2; sx++) {
                        rgb[static_cast<size_t>(y * 2 + sy) * w * 2 + x * 2 + sx] = colour;
                    }
                }
            }
        }
    }

    MapViewComposite view() const
    {
        MapViewComposite c;
        c.width = width;
        c.height = height;
        c.index = index.data();
        c.indexPitch = width;
        c.legacy = legacy.data();
        c.rgb = rgb.data();
        return c;
    }
};

static void testResample()
{
    MapViewTransform t = makeTransform(1.0f, 0.0f, 0.0f);
    TestComposite composite(t.surfaceWidth, t.surfaceHeight);
    std::vector<unsigned char> dest(static_cast<size_t>(t.screenWidth) * t.screenHeight);
    HdLayer layer;
    hdLayerInit(&layer, t.screenWidth, t.screenHeight);

    // Identity: an exact copy of the centred part.
    mapViewResample(t, composite.view(), 0, 0, t.screenWidth, t.screenHeight, dest.data(), t.screenWidth, &layer);
    int ox = (t.surfaceWidth - t.screenWidth) / 2;
    int oy = (t.surfaceHeight - t.screenHeight) / 2;
    bool exact = true;
    for (int y = 0; y < t.screenHeight; y++) {
        for (int x = 0; x < t.screenWidth; x++) {
            size_t src = static_cast<size_t>(y + oy) * t.surfaceWidth + x + ox;
            size_t dst = static_cast<size_t>(y) * t.screenWidth + x;
            exact = exact && dest[dst] == composite.index[src];
            exact = exact && layer.valid[dst] == (composite.legacy[src] ? 0 : 1);
            exact = exact && (!layer.valid[dst] || layer.shadow[dst] == composite.index[src]);
            for (int s = 0; s < 4; s++) {
                exact = exact
                    && layer.pixels[static_cast<size_t>(y * 2 + s / 2) * t.screenWidth * 2 + x * 2 + s % 2]
                        == composite.rgb[static_cast<size_t>((y + oy) * 2 + s / 2) * t.surfaceWidth * 2 + (x + ox) * 2 + s % 2];
            }
        }
    }
    CHECK(exact);

    // Half a logical pixel of pan at zoom 1: still an exact copy, one
    // output pixel over.
    t.panX = 0.5f;
    mapViewResample(t, composite.view(), 0, 0, t.screenWidth, t.screenHeight, dest.data(), t.screenWidth, &layer);
    CHECK(layer.pixels[10] == composite.rgb[static_cast<size_t>(oy * 2) * t.surfaceWidth * 2 + ox * 2 + 11]);

    // Zoom 2: the surface centre fills the screen centre, each surface
    // output pixel covers about 2x2 output pixels.
    t = makeTransform(2.0f, 0.0f, 0.0f);
    mapViewResample(t, composite.view(), 0, 0, t.screenWidth, t.screenHeight, dest.data(), t.screenWidth, &layer);
    int mx;
    int my;
    mapViewPixelToSurface(t, 32, 20, &mx, &my);
    CHECK(mx == t.surfaceWidth / 2 && my == t.surfaceHeight / 2);
    CHECK(dest[20 * t.screenWidth + 32] == composite.index[static_cast<size_t>(my) * t.surfaceWidth + mx]);

    // Zoom 0.5: each output pixel averages 2x2 surface output pixels.
    t = makeTransform(0.5f, 0.0f, 0.0f);
    mapViewResample(t, composite.view(), 0, 0, t.screenWidth, t.screenHeight, dest.data(), t.screenWidth, &layer);
    uint32_t pixel = layer.pixels[static_cast<size_t>(41) * t.screenWidth * 2 + 64];
    float u = (t.surfaceWidth * 0.5f - t.screenWidth * 0.5f / 0.5f) * 2 + (64 + 0.5f) * 2 - 0.5f;
    float v = (t.surfaceHeight * 0.5f - t.screenHeight * 0.5f / 0.5f) * 2 + (41 + 0.5f) * 2 - 0.5f;
    int ui = static_cast<int>(u);
    int vi = static_cast<int>(v);
    int expectedRed = 0;
    for (int s = 0; s < 4; s++) {
        expectedRed += (composite.rgb[static_cast<size_t>(vi + s / 2) * t.surfaceWidth * 2 + ui + s % 2] >> 16) & 0xFF;
    }
    CHECK(abs(static_cast<int>((pixel >> 16) & 0xFF) - expectedRed / 4) <= 2);

    // hd=0: only the indices.
    mapViewResample(t, composite.view(), 0, 0, t.screenWidth, t.screenHeight, dest.data(), t.screenWidth, nullptr);
    mapViewPixelToSurface(t, 5, 7, &mx, &my);
    CHECK(dest[7 * t.screenWidth + 5] == composite.index[static_cast<size_t>(my) * t.surfaceWidth + mx]);
}

// Composing, scrolling the composite (origin only) and composing the new
// strips gives the same picture as composing the scrolled surface afresh.
static void testCompositeScroll()
{
    const int width = 40;
    const int height = 30;
    std::vector<unsigned char> buffer(static_cast<size_t>(width) * height);
    for (size_t i = 0; i < buffer.size(); i++) {
        buffer[i] = static_cast<unsigned char>((i * 37) % 228 + 1);
    }
    buffer[5] = 240; // a colour-cycling index stays legacy
    unsigned char palette[256 * 3];
    for (int i = 0; i < 256 * 3; i++) {
        palette[i] = static_cast<unsigned char>(i * 7);
    }
    HdLayer layer;
    hdLayerInit(&layer, width, height);
    for (size_t i = 0; i < layer.valid.size(); i++) {
        layer.valid[i] = i % 2;
        layer.shadow[i] = buffer[i];
    }
    layer.valid[5] = 0; // HD never covers colour-cycling pixels
    for (size_t i = 0; i < layer.pixels.size(); i++) {
        layer.pixels[i] = static_cast<uint32_t>(i * 2654435761u) & 0xFFFFFF;
    }

    MapViewComposite composite;
    composite.width = width;
    composite.height = height;
    composite.index = buffer.data();
    composite.indexPitch = width;
    std::vector<uint32_t> rgb(static_cast<size_t>(width) * height * 4);
    std::vector<unsigned char> legacy(static_cast<size_t>(width) * height);
    std::vector<unsigned char> scratch(static_cast<size_t>(width) * height);
    composite.rgb = rgb.data();
    composite.legacy = legacy.data();
    mapViewCompose(&layer, buffer.data(), width, 0, 0, width, height, palette, composite, rgb.data(), legacy.data(), scratch.data());
    CHECK(legacy[5] == 1 && legacy[6] == 0);

    // Scroll everything by (7, -4): buffer and layer move, the composite
    // only moves its origin, the uncovered strips are composed again.
    const int dx = 7;
    const int dy = -4;
    std::vector<unsigned char> moved(buffer.size(), 0);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int sx = x + dx;
            int sy = y + dy;
            moved[static_cast<size_t>(y) * width + x] = sx >= 0 && sx < width && sy >= 0 && sy < height
                ? buffer[static_cast<size_t>(sy) * width + sx]
                : static_cast<unsigned char>(200);
        }
    }
    buffer = moved;
    hdLayerScroll(&layer, dx, dy);
    mapViewCompositeScroll(&composite, dx, dy);
    mapViewCompose(&layer, buffer.data(), width, width - dx, 0, dx, height, palette, composite, rgb.data(), legacy.data(), scratch.data());
    mapViewCompose(&layer, buffer.data(), width, 0, 0, width, -dy, palette, composite, rgb.data(), legacy.data(), scratch.data());

    MapViewComposite fresh = composite;
    std::vector<uint32_t> freshRgb(rgb.size());
    std::vector<unsigned char> freshLegacy(legacy.size());
    fresh.originX = 0;
    fresh.originY = 0;
    fresh.rgb = freshRgb.data();
    fresh.legacy = freshLegacy.data();
    mapViewCompose(&layer, buffer.data(), width, 0, 0, width, height, palette, fresh, freshRgb.data(), freshLegacy.data(), scratch.data());

    // Both show the same through any transform.
    for (float zoom : { 1.0f, 2.0f, 0.75f }) {
        MapViewTransform t;
        t.screenWidth = 30;
        t.screenHeight = 20;
        t.surfaceWidth = width;
        t.surfaceHeight = height;
        t.zoom = zoom;
        t.panX = 1.5f;
        HdLayer a;
        HdLayer b;
        hdLayerInit(&a, 30, 20);
        hdLayerInit(&b, 30, 20);
        std::vector<unsigned char> ia(600);
        std::vector<unsigned char> ib(600);
        mapViewResample(t, composite, 0, 0, 30, 20, ia.data(), 30, &a);
        mapViewResample(t, fresh, 0, 0, 30, 20, ib.data(), 30, &b);
        CHECK(ia == ib);
        CHECK(a.valid == b.valid);
        CHECK(a.pixels == b.pixels);
    }
}

static void testScrollRect()
{
    // On the whole layer, hdLayerScrollRect equals hdLayerScroll.
    for (int dx : { -5, 0, 7 }) {
        for (int dy : { -3, 0, 4 }) {
            HdLayer a;
            hdLayerInit(&a, 24, 16);
            for (size_t i = 0; i < a.valid.size(); i++) {
                a.valid[i] = static_cast<unsigned char>(i % 3 != 0);
                a.shadow[i] = static_cast<unsigned char>(i % 251);
            }
            for (size_t i = 0; i < a.pixels.size(); i++) {
                a.pixels[i] = static_cast<uint32_t>(i * 2654435761u) & 0xFFFFFF;
            }
            HdLayer b = a;
            hdLayerScroll(&a, dx, dy);
            hdLayerScrollRect(&b, 0, 0, 24, 16, dx, dy);
            bool same = a.valid == b.valid;
            for (size_t i = 0; i < a.valid.size(); i++) {
                if (a.valid[i] && a.shadow[i] != b.shadow[i]) {
                    same = false;
                }
            }
            for (int y = 0; y < 16; y++) {
                for (int x = 0; x < 24; x++) {
                    if (!a.valid[static_cast<size_t>(y) * 24 + x]) {
                        continue;
                    }
                    for (int s = 0; s < 4; s++) {
                        size_t p = static_cast<size_t>(y * 2 + s / 2) * 48 + x * 2 + s % 2;
                        same = same && a.pixels[p] == b.pixels[p];
                    }
                }
            }
            CHECK(same);
        }
    }

    // Inside a rectangle only: the rest is untouched, nothing comes in
    // from outside the rectangle.
    HdLayer layer;
    hdLayerInit(&layer, 24, 16);
    std::fill(layer.valid.begin(), layer.valid.end(), 1);
    for (size_t i = 0; i < layer.shadow.size(); i++) {
        layer.shadow[i] = static_cast<unsigned char>(i % 251);
    }
    std::vector<unsigned char> before = layer.shadow;
    hdLayerScrollRect(&layer, 4, 2, 10, 8, 3, 1);
    CHECK(layer.shadow[0] == before[0] && layer.valid[0] == 1);
    CHECK(layer.shadow[static_cast<size_t>(2) * 24 + 4] == before[static_cast<size_t>(3) * 24 + 7]);
    CHECK(layer.valid[static_cast<size_t>(2) * 24 + 13] == 0); // source column 16 is outside
    CHECK(layer.valid[static_cast<size_t>(9) * 24 + 5] == 0); // source row 10 is outside
    CHECK(layer.valid[static_cast<size_t>(2) * 24 + 14] == 1); // outside the rectangle
}

int main()
{
    testTransform();
    testMotion();
    testResample();
    testCompositeScroll();
    testScrollRect();

    if (gFailures != 0) {
        fprintf(stderr, "%d check(s) failed\n", gFailures);
        return 1;
    }
    printf("map_view_test: all checks passed\n");
    return 0;
}
