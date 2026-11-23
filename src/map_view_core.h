#ifndef FALLOUT_MAP_VIEW_CORE_H_
#define FALLOUT_MAP_VIEW_CORE_H_

#include <stdint.h>

#include "hd_layer.h"

namespace fallout {

// Map view (Phase 2.13), the engine-independent part: smooth panning and
// zoom of the map window. See map_view.h for how the game uses it.
//
// The map is drawn into an off-screen "surface" (logical pixels, larger than
// the map window by a margin and, for zooming out, by 1 / minimum zoom). The
// map window shows the surface through a transform: the surface point
//
//   m = surfaceCentre + pan + (s - screenCentre) / zoom
//
// is shown at map window point s (continuous logical coordinates, pixel x
// covers [x, x + 1)). With zoom 1 and no pan, map window pixel x shows
// surface pixel x + (surfaceWidth - screenWidth) / 2 (both differences are
// even, so this is exact).

struct MapViewTransform {
    int screenWidth = 0;
    int screenHeight = 0;
    int surfaceWidth = 0;
    int surfaceHeight = 0;
    float zoom = 1.0f;
    // Surface pixels.
    float panX = 0.0f;
    float panY = 0.0f;
};

inline void mapViewToSurface(const MapViewTransform& t, float sx, float sy, float* mx, float* my)
{
    *mx = t.surfaceWidth * 0.5f + t.panX + (sx - t.screenWidth * 0.5f) / t.zoom;
    *my = t.surfaceHeight * 0.5f + t.panY + (sy - t.screenHeight * 0.5f) / t.zoom;
}

inline void mapViewToScreen(const MapViewTransform& t, float mx, float my, float* sx, float* sy)
{
    *sx = t.screenWidth * 0.5f + (mx - t.surfaceWidth * 0.5f - t.panX) * t.zoom;
    *sy = t.screenHeight * 0.5f + (my - t.surfaceHeight * 0.5f - t.panY) * t.zoom;
}

// The surface pixel shown at map window pixel (x, y) (its centre).
void mapViewPixelToSurface(const MapViewTransform& t, int x, int y, int* mx, int* my);

// The part of the surface the map window shows (logical rectangle, right and
// bottom exclusive, not clipped), grown by `margin` surface pixels.
void mapViewVisibleSurface(const MapViewTransform& t, int marginX, int marginY, int* left, int* top, int* right, int* bottom);

// The map window rectangle (right and bottom exclusive, clipped to the
// window) that shows any of surface rectangle [left, right) x [top, bottom).
bool mapViewSurfaceRectToScreen(const MapViewTransform& t, int left, int top, int right, int bottom,
    int* outLeft, int* outTop, int* outRight, int* outBottom);

// Pan values the display can show without leaving the surface: pans are
// rounded to whole output pixels (1 / kHdScale) at zoom 1 so the picture
// stays sharp while panning.
float mapViewQuantizePan(float pan, float zoom);

// Smooth motion of the view.
//
// Pan: `pan` is where the view is relative to the engine's centre (surface
// pixels, positive = right / down). Scrolling asks for whole steps
// (`request`): `remain` is how far the view still has to travel, at
// `speedX/Y` pixels per second (the original scroll speed, one step per
// step interval; faster while more than a step is pending), so the view
// moves the same distance every frame. The engine's centre moves in whole
// steps ahead of the view: whenever the view moves past the centre in the
// direction it travels (`stepNeeded`), the game makes the step, which
// moves the content and the pan back by one step (`contentMoved` while
// scrolling); a step the game refuses (scroll limits, blockers) stops that
// axis (`block`). Other centre changes (a jump to the player) also keep the
// picture still and then glide back to the centre (`contentMoved` with
// scrolling false). Offsets beyond the margin are clamped (the surface has
// no picture further out).
//
// Zoom: moves towards `targetZoom` exponentially in log space (time
// constant `zoomTime` seconds), snapping when close.
struct MapViewMotion {
    float panX = 0.0f;
    float panY = 0.0f;
    float remainX = 0.0f;
    float remainY = 0.0f;
    float zoom = 1.0f;
    float targetZoom = 1.0f;
    float stepX = 32.0f;
    float stepY = 24.0f;
    float speedX = 960.0f;
    float speedY = 720.0f;
    float marginX = 64.0f;
    float marginY = 48.0f;
    float zoomTime = 0.06f;
    // Direction of the last pan movement per axis (-1, 0, 1).
    int directionX = 0;
    int directionY = 0;

    // A scroll of (dx, dy) times `amount` steps.
    void request(int dx, int dy, float amount = 1.0f);
    // The engine moved the content by (dx, dy) surface pixels.
    void contentMoved(float dx, float dy, bool scrolling);
    // Advances by `seconds`; returns true when the view changed.
    bool advance(float seconds);
    // The engine step (-1, 0, 1) the view needs on each axis now.
    int stepNeededX() const;
    int stepNeededY() const;
    // The engine refused a step on an axis: the view stops at the centre.
    void blockX();
    void blockY();
    // Scroll requests stopped: travel on to the engine's centre (the view
    // rests where the original would show the map).
    void finishAtCentre();
    bool settled() const { return panX == 0.0f && panY == 0.0f && remainX == 0.0f && remainY == 0.0f && zoom == targetZoom; }
};

// Makes an engine step of (dx, dy) scroll steps (mapScroll's step); returns
// 0 when it was made (the content moved, reported through
// contentMoved(..., true)), else non-zero.
typedef int(MapViewStepProc)(int dx, int dy, void* context);

// A scroll request (mapScroll with smooth scrolling): the engine steps at
// once on the axes where the view is at or past the centre, then the view
// travels the step. Returns 0, or -1 when every requested axis is blocked.
// `amount` is how many steps to travel: 1 for a single request (a key
// press, a wheel notch: one step as before), the time since the last
// request in step intervals while requests keep coming (a held key, the
// mouse at the screen edge), so the speed does not depend on when they come.
int mapViewMotionScroll(MapViewMotion* motion, int dx, int dy, float amount, MapViewStepProc* step, void* context);

// Advances the motion by `seconds` and makes the engine steps the view
// needs; returns true when the view changed.
bool mapViewMotionUpdate(MapViewMotion* motion, float seconds, MapViewStepProc* step, void* context);

// The surface composed for display (see mapViewResample): per logical
// surface pixel its 8-bit index (the surface buffer) and whether it must
// stay legacy (a colour-cycling index: the palette animation shows
// through), and per output pixel (kHdScale x kHdScale per logical pixel)
// its colour relative to the base palette (the HD pixel where the surface's
// HD layer shows, else the base palette colour of the index).
//
// `legacy` and `rgb` are stored wrapped around: surface pixel (x, y) is at
// ((x + originX) mod width, (y + originY) mod height), so a scroll of the
// surface only moves the origin (mapViewCompositeScroll) instead of the
// pixels.
struct MapViewComposite {
    int width = 0;
    int height = 0;
    const unsigned char* index = nullptr;
    int indexPitch = 0;
    const unsigned char* legacy = nullptr;
    const uint32_t* rgb = nullptr;
    int originX = 0;
    int originY = 0;
};

// Composes surface rectangle (x, y, width, height) into the composite's
// `rgb` and `legacy` planes (`composite` gives their size and origin): the
// HD layer (with its text overlay) where it shows, else the base palette
// colour of the buffer index. `layer` may be null. `scratch` needs
// width * height bytes.
void mapViewCompose(const HdLayer* layer, const unsigned char* buffer, int bufferPitch, int x, int y, int width, int height,
    const unsigned char* basePalette8, const MapViewComposite& composite, uint32_t* rgb, unsigned char* legacy,
    unsigned char* scratch);

// The surface content moved like a scroll (afterwards (x, y) holds what was
// at (x + dx, y + dy)): the composite follows by moving its origin.
void mapViewCompositeScroll(MapViewComposite* composite, int dx, int dy);

// Writes map window rectangle (x, y, width, height) from the composite
// through the transform: `destIndex` (pitch destPitch) gets the index of the
// surface pixel shown there (nearest); `layer` (the map window's HD layer,
// may be null) gets the colour, sampled bilinearly from the composite (an
// exact copy when the transform is a whole-output-pixel shift), valid with
// the index as shadow except where the nearest surface pixel is legacy.
void mapViewResample(const MapViewTransform& t, const MapViewComposite& source, int x, int y, int width, int height,
    unsigned char* destIndex, int destPitch, HdLayer* layer);

} // namespace fallout

#endif /* FALLOUT_MAP_VIEW_CORE_H_ */
