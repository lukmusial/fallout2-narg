#ifndef FALLOUT_HD_SCENE_H_
#define FALLOUT_HD_SCENE_H_

#include <functional>
#include <string>

namespace fallout {

// Scene preparation for the runtime upscale (Phase 2.23): when a map loads,
// every frame the map can show right away is upscaled before play starts,
// so nothing is upscaled when it is first drawn.
//
// The scene (current elevation): floor and roof tiles, walls, scenery,
// items, misc objects, and every critter in all directions of its current,
// standing, walking and running animations. Each art is locked once on the
// main thread (its pixels are copied into the jobs) and queued to the
// upscale threads ordered by its distance from the player on screen; the
// main thread then helps with the work while a loading screen (the game's
// splash picture, upscaled, with a progress bar) shows the progress.
//
// Then, in idle time (a ticker, a few milliseconds per call, stopped by the
// next map load and while the HD art cache is above half of hd_cache_mb):
// the other elevations, the critters' other animations (combat, death,
// using things), the player's inventory pictures, the world map and the
// town's maps.

// Called by mapLoad once the objects and the player are placed. Blocks
// until the scene is upscaled (hd=1, runtime upscale, hd_upscale_preload).
void hdScenePrepareMap();

// When a saved game restores the player (after its map, with the weapon in
// hand): the player's art the same way.
void hdScenePrepareDude();

// The start-up splash picture (its own 6-bit palette) shown upscaled in a
// window: `data` width x height, `shown` the picture as drawn (maybe
// scaled) at (x, y), shownWidth x shownHeight. False: not in HD (draw it
// as before). Hidden when the game's start is done.
bool hdSceneShowSplash(const unsigned char* data, int width, int height, const unsigned char* palette6, const unsigned char* shown,
    int x, int y, int shownWidth, int shownHeight);
void hdSceneHideSplash();
// After the splash faded in (dev automation writes it as splash.png).
void hdSceneSplashShown();

// Registers the idle-time ticker (game start).
void hdSceneInit();
void hdSceneExit();

struct HdSceneStats {
    int scenes = 0; // maps prepared
    int sceneArts = 0; // arts locked for the last scene
    int sceneFrames = 0; // frames queued for it
    double collectMs = 0; // main thread: finding and queueing them
    double waitMs = 0; // until they were all upscaled
    int loadingScreens = 0; // loading screens shown
    int backgroundArts = 0; // arts queued in idle time since the last map
    int backgroundLeft = 0; // still to do
};

void hdSceneGetStats(HdSceneStats* stats);

// Dev automation: called once while the next loading screen is shown.
void hdSceneSetLoadingScreenHook(std::function<void()> hook);

} // namespace fallout

#endif /* FALLOUT_HD_SCENE_H_ */
