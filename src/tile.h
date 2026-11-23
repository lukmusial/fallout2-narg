#ifndef TILE_H
#define TILE_H

#include <vector>

#include "geometry.h"
#include "map.h"
#include "map_defs.h"
#include "obj_types.h"

namespace fallout {

#define TILE_SET_CENTER_REFRESH_WINDOW 0x01
#define TILE_SET_CENTER_FLAG_IGNORE_SCROLL_RESTRICTIONS 0x02

#define TILE_MAX_DISTANCE 9999

typedef void(TileWindowRefreshProc)(Rect* rect);
typedef void(TileWindowRefreshElevationProc)(Rect* rect, MapElevation elevation);

// Optional overlay drawn over the mapper iso view each refresh (e.g. the edge editor).
// clip is the region being refreshed. Set to nullptr to disable.
typedef void(TileMapperOverlayProc)(unsigned char* buffer, int pitch, MapElevation elevation, const Rect* clip);
void tileSetMapperOverlayProc(TileMapperOverlayProc* proc);
void tileMapperOverlayRender(unsigned char* buffer, int pitch, MapElevation elevation, const Rect* clip);

extern const int _off_tile[ROTATION_COUNT];
extern const int dword_51D984[ROTATION_COUNT];
extern int gHexGridSize;
extern int gCenterTile;

extern bool gTileBorderInitialized;
extern int gTileBorderMinX;
extern int gTileBorderMinY;
extern int gTileBorderMaxX;
extern int gTileBorderMaxY;

int tileInit(TileData** squareGrid, int squareGridWidth, int squareGridHeight, int hexGridWidth, int hexGridHeight, unsigned char* buf, int windowWidth, int windowHeight, int windowPitch, TileWindowRefreshProc* windowRefreshProc);
void _tile_reset_();
void tileReset();
void tileExit();
void tileDisable();
void tileEnable();
void tileWindowRefreshRect(Rect* rect, MapElevation elevation);
void tileWindowRefresh();
int tileSetCenter(int tile, int flags);
void tile_toggle_roof(bool refresh);
int tileRoofIsVisible();
int tileToScreenXY(int tile, int* x, int* y);
int tileFromScreenXY(int x, int y, bool ignoreBounds = false);
int squareTileFromTile(int tile);
int tileDistanceBetween(int tile1, int tile2);
bool tileIsInFrontOf(int tile1, int tile2);
bool tileIsToRightOf(int tile1, int tile2);
int tileGetTileInDirection(int tile, Rotation rotation, int distance);
Rotation tileGetRotationTo(int tile1, int tile2);
int _tile_num_beyond(int from, int to, int distance);
bool tileIsEdge(int tile);
void tileScrollBlockingEnable();
void tileScrollBlockingDisable();
bool tileScrollBlockingIsEnabled();
void tileScrollLimitingEnable();
void tileScrollLimitingDisable();
bool tileScrollLimitingIsEnabled();
int squareTileToScreenXY(int squareTile, int* coordX, int* coordY, MapElevation elevation);
int squareTileToRoofScreenXY(int squareTile, int* screenX, int* screenY, MapElevation elevation);
int squareTileFromScreenXY(int screenX, int screenY, MapElevation elevation);
void squareTileScreenToCoord(int screenX, int screenY, MapElevation elevation, int* coordX, int* coordY);
void squareTileScreenToCoordRoof(int screenX, int screenY, MapElevation elevation, int* coordX, int* coordY);
void tileRenderRoofsInRect(Rect* rect, MapElevation elevation);
void tile_fill_roof(int x, int y, MapElevation elevation, bool on);
void tileRenderFloorsInRect(Rect* rect, MapElevation elevation);
void tileRenderEdgeBlackSquares(Rect* rect, MapElevation elevation, bool drawOnTop);

// CE: HD floors and roofs (Phase 2.7). With [graphics] hd=1 and an HD pack,
// floor and roof tiles also draw their pack image (or a 2x expansion of the
// legacy tile) into the iso window's HD layer, lit in RGB from the same
// intensities the legacy draw uses. These keep that layer in step with the
// legacy buffer; they do nothing without an HD layer.
//
// The rectangle (iso window coordinates, inclusive) is about to be redrawn.
void tileHdInvalidateRect(const Rect* rect);
// Legacy art (objects) was drawn at (x, y): its non-zero pixels stay legacy.
void tileHdInvalidateMasked(const unsigned char* src, int srcPitch, int x, int y, int width, int height);
// The iso window buffer was scrolled: pixel (x, y) now holds (x + dx, y + dy).
void tileHdScroll(int dx, int dy);

// Self-check for tests (dev automation `hdcheck`), all in one tick so nothing
// animates in between: the current iso window is compared with a fresh full
// redraw (stale HD pixels), then redrawn without objects to find the pixels
// objects cover, which must not show HD.
struct TileHdCheck {
    int pixels = 0; // iso window logical pixels
    int hdShown = 0; // showing HD
    int stalePixels = 0; // same legacy index, but HD shown/colour differs from a fresh redraw
    int staleTilePixels = 0; // of those, on floor/roof pixels (not objects)
    int legacyChanged = 0; // legacy index itself differs from a fresh redraw
    int objectPixels = 0; // drawn by objects
    int objectPixelsHd = 0; // of those, showing HD (must be 0)
    int tilePixels = 0; // floor/roof only (not black, not colour-cycling, no object drawn there)
    int tilePixelsHd = 0; // of those, showing HD
    int cyclingHd = 0; // colour-cycling indices showing HD (must be 0)
    std::vector<unsigned char> mask; // per pixel: 0 legacy, 1 HD tile, 2 object, 3 object with HD, 4 stale, 5 legacy changed
};
bool tileHdCheck(TileHdCheck* check);

// Timing of map redraws (dev automation `perf`): full or partial redraws of
// the iso window (tileRefreshGame) and scroll steps (mapScroll: move, strip
// redraws, window refresh).
struct TileRenderStats {
    unsigned long long refreshes = 0;
    double refreshMs = 0;
    double refreshMaxMs = 0;
    unsigned long long scrolls = 0;
    double scrollMs = 0;
    double scrollMaxMs = 0;
    double hdTilesMs = 0; // of all that, drawing HD floor and roof tiles
};
double tileRenderClockMs();
void tileRenderStatsAdd(bool scroll, double ms);
void tileGetRenderStats(TileRenderStats* stats);
void tileResetRenderStats();
bool _square_roof_intersect(int x, int y, MapElevation elevation);
void _grid_render(Rect* rect, MapElevation elevation);
int _tile_scroll_to(int tile, int flags);

static bool tileIsValid(int tile)
{
    return tile >= 0 && tile < gHexGridSize;
}

// used to decide whether to clear (blacken) a rect.
bool checkRectNeedsClear(const Rect* rect, bool hasVisArea, const Rect* visArea);

} // namespace fallout

#endif /* TILE_H */
