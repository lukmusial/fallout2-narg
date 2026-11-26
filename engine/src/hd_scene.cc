#include "hd_scene.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "animation.h"
#include "art.h"
#include "color.h"
#include "db.h"
#include "debug.h"
#include "dev_automation.h"
#include "draw.h"
#include "game.h"
#include "game_config.h"
#include "hd_art.h"
#include "input.h"
#include "inventory.h"
#include "item.h"
#include "map.h"
#include "memory.h"
#include "object.h"
#include "palette.h"
#include "platform_compat.h"
#include "settings.h"
#include "svga.h"
#include "tile.h"
#include "window_manager.h"

namespace fallout {

namespace {

    using Clock = std::chrono::steady_clock;

    double msSince(Clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    // Priorities: on-screen distance (pixels) for the scene; background work
    // after it.
    constexpr float kBackgroundPriority = 1.0e6f;

    constexpr int kFidRotationMask = 0x70000000;
    constexpr int kFidAnimationMask = 0x00FF0000;

    struct BackgroundItem {
        int fid = -1; // art to lock, or -1 for `path`
        std::string path; // art read from its file (with `palette`)
        std::string palette; // .pal file for `path` ("" = game palette)
        float priority = kBackgroundPriority;
    };

    struct SceneState {
        bool tickerAdded = false;
        HdSceneStats stats;
        std::vector<BackgroundItem> background;
        size_t backgroundNext = 0;
        bool backgroundStatic = false; // world map etc. queued once per session
        std::function<void()> loadingHook;
    };

    SceneState gScene;

    bool sceneEnabled()
    {
        return settings.graphics.hd && settings.graphics.hd_upscale_preload && hdArtActive()
            && compat_stricmp(settings.graphics.hd_upscale.c_str(), "scripted") == 0;
    }

    // Art a map shows through the HD layer (critters only with
    // hd_upscale_critters).
    bool hdObjectType(int type)
    {
        return objectHdIsHdType(type) || type == OBJ_TYPE_MISC;
    }

    // --- collecting ---------------------------------------------------------

    struct Collector {
        // fid -> best priority; locked and queued in priority order.
        std::unordered_map<int, float> fids;
        std::set<std::string> paths; // art files queued (critters: one file per direction)

        void add(int fid, float priority)
        {
            auto found = fids.find(fid);
            if (found == fids.end() || priority < found->second) {
                fids[fid] = priority;
            }
        }
    };

    float screenDistance(int x, int y, int originX, int originY)
    {
        float dx = static_cast<float>(x - originX);
        float dy = static_cast<float>(y - originY);
        return std::sqrt(dx * dx + dy * dy);
    }

    int withAnimation(int fid, int anim)
    {
        return (fid & ~kFidAnimationMask) | ((anim << 16) & kFidAnimationMask);
    }

    int withRotation(int fid, int rotation)
    {
        return (fid & ~kFidRotationMask) | ((rotation << 28) & kFidRotationMask);
    }

    // Animations a critter shows without warning: standing, walking,
    // running (and whatever it is doing now).
    constexpr int kSceneAnimations[] = { ANIM_STAND, ANIM_WALK, ANIM_RUNNING };

    // Locks `fid` (each direction file of critter art) and queues its frames.
    int lockAndQueue(int fid, float priority, int group, std::set<std::string>* paths, int* arts)
    {
        int queued = 0;
        FrmId base(fid);
        int rotations = base.objectType() == OBJ_TYPE_CRITTER ? ROTATION_COUNT : 1;
        for (int rotation = 0; rotation < rotations; rotation++) {
            FrmId frmId(rotations > 1 ? withRotation(fid, rotation) : fid);
            const char* path = frmId.filePath();
            if (path == nullptr || !paths->insert(path).second) {
                continue;
            }
            CacheEntry* handle = nullptr;
            Art* art = artLock(frmId, &handle);
            if (art == nullptr) {
                continue;
            }
            queued += hdArtQueueArt(path, art, priority, group);
            (*arts)++;
            artUnlock(handle);
        }
        return queued;
    }

    template <typename Put>
    void collectObject(Object* object, Put& put, std::vector<BackgroundItem>* background, int originX, int originY, bool scene)
    {
        int type = FrmId(object->fid).objectType();
        if (!hdObjectType(type)) {
            return;
        }
        int x = 0;
        int y = 0;
        tileToScreenXY(object->tile, &x, &y);
        float distance = screenDistance(x, y, originX, originY);
        if (type != OBJ_TYPE_CRITTER) {
            put(object->fid, distance);
            return;
        }
        put(object->fid, distance);
        for (int anim : kSceneAnimations) {
            put(withAnimation(object->fid, anim), distance + 1.0f);
        }
        // Critters holding a weapon have their own art for it (the
        // player's is set after the map loads): the weapons in their
        // hands, and the pictures the interface bar shows of them.
        for (Object* weapon : { critterGetItem2(object), critterGetItem1(object) }) {
            if (weapon == nullptr) {
                continue;
            }
            int code = static_cast<int>(weaponGetAnimationCode(weapon));
            if (code > 0) {
                int armed = (object->fid & ~0x0000F000) | ((code << 12) & 0x0000F000);
                put(armed, distance + 1.0f);
                for (int anim : kSceneAnimations) {
                    put(withAnimation(armed, anim), distance + 1.0f);
                }
            }
            if (object == gDude) {
                FrmId inventory = itemGetInventoryFrmId(weapon);
                if (inventory.valid() && inventory.fid() != -1) {
                    put(inventory.fid(), 0.0f);
                }
            }
        }
        // Everything else it can do, in idle time.
        for (int anim = ANIM_FIRST; anim < ANIM_COUNT; anim++) {
            bool sceneAnim = anim == FrmId(object->fid).animationType();
            for (int known : kSceneAnimations) {
                sceneAnim = sceneAnim || anim == known;
            }
            if (!sceneAnim) {
                BackgroundItem item;
                item.fid = withAnimation(object->fid, anim);
                item.priority = kBackgroundPriority + distance + 1000.0f * (scene ? 0 : 1);
                background->push_back(item);
            }
        }
    }

    void collectElevation(MapElevation elevation, bool scene, int originX, int originY, Collector* collector,
        std::vector<BackgroundItem>* background)
    {
        auto put = [&](int fid, float priority) {
            if (scene) {
                collector->add(fid, priority);
            } else {
                BackgroundItem item;
                item.fid = fid;
                item.priority = kBackgroundPriority + priority;
                background->push_back(item);
            }
        };

        // Floor and roof tiles.
        if (_square[elevation] != nullptr) {
            for (int square = 0; square < SQUARE_GRID_SIZE; square++) {
                int tileFids = _square[elevation]->tileFid[square];
                int x = 0;
                int y = 0;
                squareTileToScreenXY(square, &x, &y, elevation);
                float distance = screenDistance(x + 40, y + 18, originX, originY);
                for (int half = 0; half < 2; half++) {
                    TileFrameId frameId = half == 0 ? FloorTileFrmId(tileFids).frameId<TileFrameId>()
                                                    : RoofTileFrmId(tileFids).frameId<TileFrameId>();
                    if (frameId == TileFrameId::Invalid || static_cast<int>(frameId) <= 1) {
                        continue;
                    }
                    put(FrmId(frameId).fid(), distance);
                }
            }
        }

        // Objects.
        for (Object* object = objectFindFirstAtElevation(elevation); object != nullptr; object = objectFindNextAtElevation()) {
            collectObject(object, put, background, originX, originY, scene);
        }
    }
    // Floor and roof tiles are upscaled with their real neighbours: per tile
    // art, at each of the 8 nearest lattice positions, the tile that is
    // there most often on this map (all elevations), when it is there in at
    // least a quarter of the placements; otherwise (and where it is the tile
    // itself) the tile's own copy, as the offline pack did.
    int prepareTileContexts()
    {
        hdArtClearTileContexts();
        if (!settings.graphics.hd_upscale_tile_neighbours) {
            return 0;
        }
        constexpr int W = SQUARE_GRID_WIDTH;
        constexpr int H = SQUARE_GRID_HEIGHT;
        auto frameAt = [](int elevation, int square, int half) {
            int tileFids = _square[elevation]->tileFid[square];
            TileFrameId frameId = half == 0 ? FloorTileFrmId(tileFids).frameId<TileFrameId>()
                                            : RoofTileFrmId(tileFids).frameId<TileFrameId>();
            int id = static_cast<int>(frameId);
            return frameId == TileFrameId::Invalid || id <= 1 ? -1 : id;
        };

        struct Counts {
            int placements = 0;
            std::unordered_map<int, int> neighbour[9];
        };
        std::unordered_map<int, Counts> counts[2];
        for (int elevation = 0; elevation < ELEVATION_COUNT; elevation++) {
            if (_square[elevation] == nullptr) {
                continue;
            }
            for (int square = 0; square < SQUARE_GRID_SIZE; square++) {
                int row = square / W;
                int col = W - 1 - square % W; // screen column
                for (int half = 0; half < 2; half++) {
                    int id = frameAt(elevation, square, half);
                    if (id == -1) {
                        continue;
                    }
                    Counts& entry = counts[half][id];
                    entry.placements++;
                    for (int r = -1; r <= 1; r++) {
                        for (int c = -1; c <= 1; c++) {
                            if ((r == 0 && c == 0) || row + r < 0 || row + r >= H || col + c < 0 || col + c >= W) {
                                continue;
                            }
                            int other = frameAt(elevation, (row + r) * W + (W - 1 - (col + c)), half);
                            entry.neighbour[(r + 1) * 3 + (c + 1)][other]++;
                        }
                    }
                }
            }
        }

        int withContext = 0;
        for (int half = 0; half < 2; half++) {
            for (const auto& tile : counts[half]) {
                int chosen[9];
                bool any = false;
                for (int slot = 0; slot < 9; slot++) {
                    chosen[slot] = -1;
                    int best = 0;
                    for (const auto& candidate : tile.second.neighbour[slot]) {
                        if (candidate.second > best) {
                            best = candidate.second;
                            chosen[slot] = candidate.first;
                        }
                    }
                    if (chosen[slot] == tile.first || best * 4 < tile.second.placements) {
                        chosen[slot] = -1;
                    }
                    any = any || chosen[slot] != -1;
                }
                if (!any) {
                    continue;
                }
                CacheEntry* handles[10] = {};
                FrmId self(static_cast<TileFrameId>(tile.first));
                Art* art = artLock(self, &handles[9]);
                const char* path = self.filePath();
                if (art == nullptr || path == nullptr) {
                    if (art != nullptr) {
                        artUnlock(handles[9]);
                    }
                    continue;
                }
                int width = artGetWidth(art, 0, ROTATION_NE);
                int height = artGetHeight(art, 0, ROTATION_NE);
                const unsigned char* neighbours[3][3] = {};
                for (int slot = 0; slot < 9; slot++) {
                    if (chosen[slot] == -1) {
                        continue;
                    }
                    Art* other = artLock(FrmId(static_cast<TileFrameId>(chosen[slot])), &handles[slot]);
                    if (other != nullptr && artGetWidth(other, 0, ROTATION_NE) == width && artGetHeight(other, 0, ROTATION_NE) == height) {
                        neighbours[slot / 3][slot % 3] = artGetFrameData(other, 0, ROTATION_NE);
                    }
                }
                hdArtSetTileContext(path, neighbours, width, height);
                withContext++;
                for (CacheEntry* handle : handles) {
                    if (handle != nullptr) {
                        artUnlock(handle);
                    }
                }
            }
        }
        return withContext;
    }

    // The world map, its town maps and other full-screen art shown later:
    // queued once per session, last.
    void addStaticBackground(std::vector<BackgroundItem>* background)
    {
        if (gScene.backgroundStatic) {
            return;
        }
        gScene.backgroundStatic = true;
        auto put = [&](const std::string& path, const char* palette, float priority) {
            BackgroundItem item;
            item.path = path;
            item.palette = palette;
            item.priority = 2.0f * kBackgroundPriority + priority;
            background->push_back(item);
        };
        char path[64];
        for (int index = 0; index < 30; index++) {
            snprintf(path, sizeof(path), "art\\intrface\\wrldmp%02d.frm", index);
            put(path, "", static_cast<float>(index));
        }
        static const char* const kWorldMapArt[] = { "wmscreen", "wmdial", "wmglobe", "wmapbox", "wmaploc", "wmaptarg",
            "wmaptrg2", "wmcarmve", "wminfce0", "wminfce1", "wminfce2", "wminfce3", "wmaplabs", "wmapfgt0", "wmapfgt1" };
        for (const char* name : kWorldMapArt) {
            snprintf(path, sizeof(path), "art\\intrface\\%s.frm", name);
            put(path, "", 50.0f);
        }
        static const char* const kTownMaps[] = { "wm_abbey", "wm_army", "wm_arroy", "wm_colus", "wm_den", "wm_enclv", "wm_epa",
            "wm_gecko", "wm_hills", "wm_klam", "wm_mltry", "wm_modoc", "wm_navar", "wm_ncr", "wm_red", "wm_reno",
            "wm_sanfn", "wm_tribe", "wm_vlt13", "wm_vlt15", "wm_vltct" };
        for (const char* name : kTownMaps) {
            snprintf(path, sizeof(path), "art\\intrface\\%s.frm", name);
            put(path, "", 100.0f);
        }
        // Screens with their own palettes.
        put("art\\intrface\\helpscrn.frm", "art\\intrface\\helpscrn.pal", 200.0f);
        put("art\\intrface\\death.frm", "art\\intrface\\death.pal", 200.0f);
    }

    // The player's inventory pictures (the inventory screen).
    void addInventory(std::vector<BackgroundItem>* background)
    {
        if (gDude == nullptr) {
            return;
        }
        Inventory* inventory = &(gDude->data.inventory);
        for (int index = 0; index < inventory->length; index++) {
            Object* item = inventory->items[index].item;
            if (item == nullptr) {
                continue;
            }
            FrmId frmId = itemGetInventoryFrmId(item);
            int fid = frmId.fid();
            if (!frmId.valid() || fid == -1) {
                continue;
            }
            BackgroundItem entry;
            entry.fid = fid;
            entry.priority = kBackgroundPriority * 0.5f + index;
            background->push_back(entry);
        }
    }

    // --- loading screen ----------------------------------------------------

    struct LoadingScreen {
        int window = -1;
        std::vector<unsigned char> splash; // 1x
        int splashWidth = 0;
        int splashHeight = 0;
        int x = 0; // splash position in the window
        int y = 0;
        unsigned char palette6[256 * 3];
        unsigned char savedCmap[256 * 3];
        unsigned char savedSystem[256 * 3];
        std::shared_ptr<const HdImage> hd;
        int barX = 0;
        int barY = 0;
        int barWidth = 0;
        int barHeight = 0;
        unsigned char colorBorder = 0;
        unsigned char colorFill = 0;
        unsigned char colorEmpty = 0;
        int lastFilled = -1;
    };

    unsigned char nearestColor(const unsigned char* palette6, int r, int g, int b)
    {
        int best = 0;
        int bestDistance = 1 << 30;
        for (int index = 1; index < 229; index++) {
            int dr = palette6[index * 3] * 4 - r;
            int dg = palette6[index * 3 + 1] * 4 - g;
            int db = palette6[index * 3 + 2] * 4 - b;
            int distance = dr * dr + dg * dg + db * db;
            if (distance < bestDistance) {
                bestDistance = distance;
                best = index;
            }
        }
        return static_cast<unsigned char>(best);
    }

    // The splash picture (art/splash/splashN.rix, its own palette) as the
    // start-up does it, with palette index 0 and 255 swapped.
    bool loadSplash(LoadingScreen* screen)
    {
        const char* language = settings.system.language.c_str();
        char dir[64];
        if (compat_stricmp(language, ENGLISH) != 0) {
            snprintf(dir, sizeof(dir), "art\\%s\\splash\\", language);
        } else {
            snprintf(dir, sizeof(dir), "art\\splash\\");
        }
        File* stream = nullptr;
        int splash = std::max(settings.system.splash - 1, 0);
        for (int attempt = 0; attempt < 10 && stream == nullptr; attempt++) {
            char path[96];
            snprintf(path, sizeof(path), "%ssplash%d.rix", dir, (splash + attempt) % 10);
            stream = fileOpen(path, "rb");
        }
        if (stream == nullptr) {
            return false;
        }
        int version = 0;
        fileReadInt32(stream, &version);
        short width = 0;
        short height = 0;
        fileRead(&width, sizeof(width), 1, stream);
        fileRead(&height, sizeof(height), 1, stream);
        if (version != 'RIX3' || width <= 0 || height <= 0 || width > 2048 || height > 2048) {
            fileClose(stream);
            return false;
        }
        fileSeek(stream, 10, SEEK_SET);
        fileRead(screen->palette6, 1, 768, stream);
        screen->splash.resize(static_cast<size_t>(width) * height);
        fileRead(screen->splash.data(), 1, screen->splash.size(), stream);
        fileClose(stream);
        memcpy(screen->palette6 + 255 * 3, screen->palette6, 3);
        memset(screen->palette6, 0, 3);
        for (unsigned char& index : screen->splash) {
            index = index == 0 ? 255 : (index == 255 ? 0 : index);
        }
        screen->splashWidth = width;
        screen->splashHeight = height;
        return true;
    }

    void drawLoadingScreen(LoadingScreen* screen, int total, int done)
    {
        unsigned char* buffer = windowGetBuffer(screen->window);
        int pitch = windowGetWidth(screen->window);
        int filled = total > 0 ? static_cast<int>(static_cast<long long>(screen->barWidth - 4) * done / total) : 0;
        if (filled == screen->lastFilled) {
            return;
        }
        bool first = screen->lastFilled < 0;
        screen->lastFilled = filled;

        if (first) {
            blitBufferToBuffer(screen->splash.data(), screen->splashWidth, screen->splashHeight, screen->splashWidth,
                buffer + static_cast<size_t>(screen->y) * pitch + screen->x, pitch);
        }
        // Bar: border, filled part, empty part (legacy pixels over the HD
        // picture: they differ from the splash there).
        unsigned char* bar = buffer + static_cast<size_t>(screen->barY) * pitch + screen->barX;
        bufferFill(bar, screen->barWidth, screen->barHeight, pitch, static_cast<Color>(screen->colorBorder));
        bufferFill(bar + pitch * 2 + 2, screen->barWidth - 4, screen->barHeight - 4, pitch, static_cast<Color>(screen->colorEmpty));
        if (filled > 0) {
            bufferFill(bar + pitch * 2 + 2, filled, screen->barHeight - 4, pitch, static_cast<Color>(screen->colorFill));
        }
        if (screen->hd != nullptr) {
            if (first) {
                hdArtDrawRegion(screen->window, *screen->hd, screen->splash.data(), screen->splashWidth, screen->splashHeight,
                    0, 0, screen->splashWidth, screen->splashHeight, screen->x, screen->y);
            } else {
                hdArtDrawRegion(screen->window, *screen->hd, screen->splash.data(), screen->splashWidth, screen->splashHeight,
                    screen->barX - screen->x, screen->barY - screen->y, screen->barWidth, screen->barHeight, screen->barX, screen->barY);
            }
        }
        if (first) {
            windowRefresh(screen->window);
        } else {
            Rect rect = { screen->barX, screen->barY, screen->barX + screen->barWidth - 1, screen->barY + screen->barHeight - 1 };
            windowRefreshRect(screen->window, &rect);
        }
        renderPresent();
    }

    bool showLoadingScreen(LoadingScreen* screen)
    {
        if (!loadSplash(screen)) {
            return false;
        }
        int screenWidth = screenGetWidth();
        int screenHeight = screenGetHeight();
        screen->window = windowCreate(0, 0, screenWidth, screenHeight, COLOR_BLACK, static_cast<WindowFlags>(WINDOW_MODAL | WINDOW_MOVE_ON_TOP));
        if (screen->window == -1) {
            return false;
        }
        screen->x = std::max((screenWidth - screen->splashWidth) / 2, 0);
        screen->y = std::max((screenHeight - screen->splashHeight) / 2, 0);
        screen->splashWidth = std::min(screen->splashWidth, screenWidth);
        screen->splashHeight = std::min(screen->splashHeight, screenHeight);

        // The splash's palette is the base palette while it is shown.
        memcpy(screen->savedCmap, _cmap, sizeof(screen->savedCmap));
        memcpy(screen->savedSystem, _getSystemPalette(), sizeof(screen->savedSystem));
        memcpy(_cmap, screen->palette6, sizeof(screen->palette6));
        paletteSetEntries(screen->palette6);

        unsigned char palette8[256 * 3];
        for (int index = 0; index < 256 * 3; index++) {
            palette8[index] = colorExpand6(screen->palette6[index]);
        }
        screen->hd = hdArtUpscaleImage("loading/splash", screen->splash.data(), screen->splashWidth, screen->splashHeight,
            screen->splashWidth, palette8);

        screen->colorBorder = nearestColor(screen->palette6, 120, 100, 40);
        screen->colorFill = nearestColor(screen->palette6, 252, 200, 60);
        screen->colorEmpty = nearestColor(screen->palette6, 24, 20, 12);
        // A thin bar along the bottom edge (the splash pictures have their
        // own "loading" lettering above it).
        screen->barWidth = screen->splashWidth - 40;
        screen->barHeight = 8;
        screen->barX = screen->x + 20;
        screen->barY = screen->y + screen->splashHeight - 11;
        gScene.stats.loadingScreens++;
        return true;
    }

    void hideLoadingScreen(LoadingScreen* screen)
    {
        if (screen->window == -1) {
            return;
        }
        windowDestroy(screen->window);
        screen->window = -1;
        memcpy(_cmap, screen->savedCmap, sizeof(screen->savedCmap));
        paletteSetEntries(screen->savedSystem);
    }

    // --- idle time ---------------------------------------------------------

    void backgroundTicker()
    {
        if (gScene.backgroundNext >= gScene.background.size() || !sceneEnabled()) {
            return;
        }
        // Not while the art cache is busy, nor beyond 3/4 of the HD cache.
        if (artIsLoading() || hdArtCacheBytes() > hdArtCacheBudget() / 4 * 3 || hdArtUpscalePending() > 64) {
            return;
        }
        Clock::time_point start = Clock::now();
        std::set<std::string> paths;
        int arts = 0;
        while (gScene.backgroundNext < gScene.background.size() && msSince(start) < 2.0) {
            const BackgroundItem& item = gScene.background[gScene.backgroundNext++];
            if (item.fid != -1) {
                lockAndQueue(item.fid, item.priority, kHdUpscaleGroupBackground, &paths, &arts);
            } else {
                hdArtQueueArtFile(item.path.c_str(), item.palette.empty() ? nullptr : item.palette.c_str(), item.priority,
                    kHdUpscaleGroupBackground);
                arts++;
            }
        }
        gScene.stats.backgroundArts += arts;
        gScene.stats.backgroundLeft = static_cast<int>(gScene.background.size() - gScene.backgroundNext);
    }

} // namespace

void hdSceneInit()
{
    if (!gScene.tickerAdded) {
        tickersAdd(backgroundTicker);
        gScene.tickerAdded = true;
    }
}

void hdSceneExit()
{
    if (gScene.tickerAdded) {
        tickersRemove(backgroundTicker);
        gScene.tickerAdded = false;
    }
}

void hdScenePrepareMap()
{
    if (!sceneEnabled()) {
        return;
    }

    Clock::time_point start = Clock::now();
    hdArtSceneBegin();
    hdArtUpscaleCancel(kHdUpscaleGroupScene);
    hdArtUpscaleCancel(kHdUpscaleGroupBackground);
    hdArtUpscaleResetGroup(kHdUpscaleGroupScene);
    gScene.background.clear();
    gScene.backgroundNext = 0;
    gScene.stats.backgroundArts = 0;

    // Distances from the player on screen (or the screen's middle).
    int originX = 0;
    int originY = 0;
    if (gDude != nullptr && gDude->tile != -1) {
        tileToScreenXY(gDude->tile, &originX, &originY);
    }

    int tileContexts = prepareTileContexts();

    Collector collector;
    std::vector<BackgroundItem> background;
    collectElevation(gElevation, true, originX, originY, &collector, &background);
    for (int elevation = 0; elevation < ELEVATION_COUNT; elevation++) {
        if (elevation != gElevation) {
            collectElevation(static_cast<MapElevation>(elevation), false, originX, originY, &collector, &background);
        }
    }
    addInventory(&background);
    addStaticBackground(&background);
    std::stable_sort(background.begin(), background.end(),
        [](const BackgroundItem& a, const BackgroundItem& b) { return a.priority < b.priority; });
    gScene.background = std::move(background);

    // Lock and queue the scene, nearest first.
    std::vector<std::pair<float, int>> order;
    order.reserve(collector.fids.size());
    for (const auto& entry : collector.fids) {
        order.emplace_back(entry.second, entry.first);
    }
    std::sort(order.begin(), order.end());
    int arts = 0;
    int frames = 0;
    for (const auto& entry : order) {
        frames += lockAndQueue(entry.second, entry.first, kHdUpscaleGroupScene, &collector.paths, &arts);
    }
    double collectMs = msSince(start);

    // Help the threads until the scene is upscaled; a loading screen when it
    // takes a moment.
    Clock::time_point waitStart = Clock::now();
    LoadingScreen screen;
    bool shown = false;
    bool triedScreen = false;
    Clock::time_point lastPump = Clock::now();
    while (true) {
        int total = 0;
        int done = 0;
        hdArtUpscaleProgress(kHdUpscaleGroupScene, &total, &done);
        if (done >= total) {
            break;
        }
        if (!triedScreen && settings.graphics.hd_loading_screen && msSince(waitStart) >= settings.graphics.hd_loading_screen_min_ms) {
            triedScreen = true;
            shown = showLoadingScreen(&screen);
        }
        if (shown) {
            drawLoadingScreen(&screen, total, done);
            if (gScene.loadingHook) {
                std::function<void()> hook = std::move(gScene.loadingHook);
                gScene.loadingHook = nullptr;
                hook();
            }
        }
        if (msSince(lastPump) > 100.0) {
            // Keep the window responsive during long preparations.
            _GNW95_process_message();
            lastPump = Clock::now();
        }
        if (!hdArtUpscaleHelp()) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }
    if (shown) {
        drawLoadingScreen(&screen, 1, 1);
        hideLoadingScreen(&screen);
    }
    double waitMs = msSince(waitStart);
    hdArtSceneEnd(collectMs);

    gScene.stats.scenes++;
    gScene.stats.sceneArts = arts;
    gScene.stats.sceneFrames = frames;
    gScene.stats.collectMs = collectMs;
    gScene.stats.waitMs = waitMs;
    gScene.stats.backgroundLeft = static_cast<int>(gScene.background.size());
    debugPrint("HD: scene of map %s: %d arts (%d tiles with their neighbours), %d frames queued in %.1f ms, upscaled in %.1f ms%s; %zu arts for idle time\n",
        gMapHeader.name, arts, tileContexts, frames, collectMs, waitMs, shown ? " (loading screen)" : "", gScene.background.size());
}

void hdScenePrepareDude()
{
    if (!sceneEnabled() || gDude == nullptr) {
        return;
    }
    Clock::time_point start = Clock::now();
    Collector collector;
    std::vector<BackgroundItem> background;
    auto put = [&](int fid, float priority) { collector.add(fid, priority); };
    int originX = 0;
    int originY = 0;
    if (gDude->tile != -1) {
        tileToScreenXY(gDude->tile, &originX, &originY);
    }
    collectObject(gDude, put, &background, originX, originY, true);
    hdArtUpscaleResetGroup(kHdUpscaleGroupScene);
    int arts = 0;
    int frames = 0;
    for (const auto& entry : collector.fids) {
        frames += lockAndQueue(entry.first, entry.second, kHdUpscaleGroupScene, &collector.paths, &arts);
    }
    while (true) {
        int total = 0;
        int done = 0;
        hdArtUpscaleProgress(kHdUpscaleGroupScene, &total, &done);
        if (done >= total) {
            break;
        }
        if (!hdArtUpscaleHelp()) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }
    hdArtSceneEnd(gScene.stats.collectMs);
    debugPrint("HD: the player as loaded: %d arts, %d frames upscaled in %.1f ms\n", arts, frames, msSince(start));
}

namespace {

    struct SplashState {
        int window = -1;
        unsigned char savedCmap[256 * 3];
    };

    SplashState gSplash;

} // namespace

bool hdSceneShowSplash(const unsigned char* data, int width, int height, const unsigned char* palette6, const unsigned char* shown,
    int x, int y, int shownWidth, int shownHeight)
{
    if (!settings.graphics.hd || !windowHdIsActive() || !hdArtActive()
        || compat_stricmp(settings.graphics.hd_upscale.c_str(), "scripted") != 0) {
        debugPrint("HD: splash not upscaled (hd %d, layer %d, HD art %d)\n", settings.graphics.hd ? 1 : 0,
            windowHdIsActive() ? 1 : 0, hdArtActive() ? 1 : 0);
        return false;
    }
    unsigned char palette8[256 * 3];
    for (int index = 0; index < 256 * 3; index++) {
        palette8[index] = colorExpand6(palette6[index]);
    }
    std::shared_ptr<const HdImage> hd = hdArtUpscaleImage("splash", data, width, height, width, palette8);
    if (hd == nullptr) {
        return false;
    }
    int window = windowCreate(0, 0, screenGetWidth(), screenGetHeight(), COLOR_BLACK, WINDOW_MOVE_ON_TOP);
    if (window == -1) {
        return false;
    }
    gSplash.window = window;
    // Its palette is the base palette while it shows (HD colours follow it).
    memcpy(gSplash.savedCmap, _cmap, sizeof(gSplash.savedCmap));
    memcpy(_cmap, palette6, sizeof(gSplash.savedCmap));
    unsigned char* buffer = windowGetBuffer(window);
    int pitch = windowGetWidth(window);
    blitBufferToBuffer(shown, shownWidth, shownHeight, shownWidth, buffer + static_cast<size_t>(y) * pitch + x, pitch);
    windowHdDrawImage(window, x, y, shownWidth, shownHeight, *hd);
    windowRefresh(window);
    debugPrint("HD: splash %dx%d upscaled\n", width, height);
    return true;
}

void hdSceneSplashShown()
{
    // Dev automation (a hidden run): the splash as presented, for reviews.
    if (gSplash.window != -1 && devAutomationWantsHiddenWindow()) {
        renderPresent();
        screenshotSaveFramebufferPng(devAutomationOutputPath("splash", ".png").c_str());
    }
}

void hdSceneHideSplash()
{
    if (gSplash.window == -1) {
        return;
    }
    windowDestroy(gSplash.window);
    gSplash.window = -1;
    memcpy(_cmap, gSplash.savedCmap, sizeof(gSplash.savedCmap));
}

void hdSceneGetStats(HdSceneStats* stats)
{
    *stats = gScene.stats;
    stats->backgroundLeft = static_cast<int>(gScene.background.size() - std::min(gScene.backgroundNext, gScene.background.size()));
}

void hdSceneSetLoadingScreenHook(std::function<void()> hook)
{
    gScene.loadingHook = std::move(hook);
}

} // namespace fallout
