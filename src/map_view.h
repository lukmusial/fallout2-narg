#ifndef FALLOUT_MAP_VIEW_H_
#define FALLOUT_MAP_VIEW_H_

#include "geometry.h"
#include "map_view_core.h"

namespace fallout {

// Map view (Phase 2.13): smooth panning and zoom of the map window.
//
// Enabled with `[graphics] smooth_scroll=1` and/or `[graphics] zoom=1` (both
// off by default, so the original path runs unchanged). Then the map (tiles,
// objects, floating text, the HD layer) is drawn into an off-screen
// surface instead of the map window: the surface is larger than the window
// by a pan margin on every side and, with zoom, by 1 / minimum zoom. The
// engine keeps its own coordinates (the centre tile, `tileToScreenXY`, the
// scroll steps, blockers and limits) on the surface, so game logic is
// unchanged. The map window shows the surface through a transform (see
// map_view_core.h):
//
// - smooth panning: every change of the centre (a scroll step, a jump to the
//   player) moves the surface content in whole hexes as before; the view
//   takes the opposite pan offset and glides back to 0 at the scroll speed,
//   so the steps blend into constant motion. Pixels shown are whole output
//   pixels at zoom 1 (sharp). Offsets beyond the margin are clamped (a long
//   jump only glides at its end).
// - zoom: 0.5x .. 2x in steps (mouse wheel over the map, Ctrl+= / Ctrl+-,
//   Ctrl+0 resets), animated, about the centre of the map window. The
//   interface bar and every other window keep their size. Bilinear in HD,
//   nearest pixel with hd=0. The zoom lasts for the session (not saved).
//
// Every map window pixel shown maps to a surface pixel, so hit-testing goes
// through mapViewScreenToSurface (the game mouse, tile/object picking) and
// the HD lights place their light field with mapViewSurfaceToScreen.

// Called by isoInit after creating the map window. Returns true when the map
// view is on; the surface then replaces the window buffer for drawing.
bool mapViewInit(int isoWindow);
void mapViewExit();
bool mapViewIsActive();

unsigned char* mapViewSurfaceBuffer();
int mapViewSurfaceWindow();
int mapViewSurfaceWidth();
int mapViewSurfaceHeight();

// The engine drew surface rectangle `rect` (the tile refresh proc): it is
// composed and shown in the map window.
void mapViewSurfaceChanged(const Rect* rect);

// The engine moved the content of the rendered region by (dx, dy) in place
// (a scroll step, see mapScroll); `strips` are the rectangles drawn anew.
void mapViewSurfaceScrolled(int dx, int dy, const Rect* strips, int stripCount);

// The centre changed: the content moved by (dx, dy) surface pixels.
void mapViewContentMoved(int dx, int dy);

// A new map is being loaded: the next centre change snaps.
void mapViewReset();

// The region of the surface that is drawn (surface coordinates).
void mapViewGetRegion(Rect* rect);

// Milliseconds between scroll steps (the original 33 at scroll_speed=100).
int mapViewScrollInterval();

// Smooth scrolling (map.cc): mapScroll hands its (throttled) requests to
// mapViewScroll, which makes the engine's steps through the step proc (the
// original scroll step) as the view travels; returns as mapScroll does
// (0, or -1 when blocked).
typedef int(MapViewScrollStepProc)(int dx, int dy);
void mapViewSetScrollStepProc(MapViewScrollStepProc* proc);
bool mapViewSmoothScrolling();
int mapViewScroll(int dx, int dy);

// Per-frame update (pan and zoom animation), a ticker.
void mapViewTick();

// Map window pixel -> surface pixel (identity when the view is off).
void mapViewScreenToSurface(int* x, int* y);
// Surface point -> map window point.
void mapViewSurfaceToScreen(float* x, float* y);
// Current transform (identity sized to the window when off).
void mapViewGetTransform(MapViewTransform* transform);
float mapViewGetZoom();

// Zoom: steps through the zoom levels (+1 in, -1 out), resets, or sets a
// level (animated or at once). No-ops unless `[graphics] zoom=1`.
bool mapViewZoomEnabled();
void mapViewZoomStep(int direction);
void mapViewZoomReset();
void mapViewSetZoom(float zoom, bool animate);
float mapViewGetTargetZoom();

// While a snapshot is taken (the dialogue's picture of the map), the view is
// the plain surface (zoom 1, no pan) and centre changes do not pan.
void mapViewBeginSnapshot();
void mapViewEndSnapshot();

// Whether the view is still moving (pan or zoom).
bool mapViewIsMoving();

// Timing counters (ms), for the harness `perf` command.
struct MapViewStats {
    int presents = 0;
    double presentMs = 0.0;
    double presentMaxMs = 0.0;
    // Of presentMs, the resampling (the rest is the window manager and
    // presenter work for the map window).
    double resampleMs = 0.0;
    int composes = 0;
    double composeMs = 0.0;
    int scrolls = 0;
    double scrollMs = 0.0;
};

void mapViewGetStats(MapViewStats* stats);
void mapViewResetStats();

} // namespace fallout

#endif /* FALLOUT_MAP_VIEW_H_ */
