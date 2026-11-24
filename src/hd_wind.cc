#include "hd_wind.h"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <unordered_map>

#include "art.h"
#include "input.h"
#include "map.h"
#include "map_view.h"
#include "object.h"
#include "settings.h"
#include "tile.h"
#include "window_manager.h"

namespace fallout {

namespace {

    constexpr float kPi = 3.14159265f;
    // Redraws per second at most (the ticker runs every input poll).
    constexpr unsigned int kTickMs = 55;
    // The base (bottom 35%) never moves.
    constexpr float kFixedShare = 0.35f;

    // Plants by scenery art name (lower case, without extension) and how
    // freely they move (1 = leaves and grass). Picked from the 1863 names of
    // art/scenery/scenery.lst and checked on a contact sheet: the weeds that
    // are mostly rocks, dirt mounds or junk (weed05-09, 11-13, 15-23) and
    // the cave weeds (cvweed*, no wind underground) are left out.
    float stiffnessForName(const char* name)
    {
        static const char* const weeds[] = { "weed01", "weed02", "weed03", "weed04", "weed10", "weed14", "weed24", "weed25", "weed26" };
        for (const char* weed : weeds) {
            if (strcmp(name, weed) == 0) {
                return 1.0f;
            }
        }
        if (strncmp(name, "tree", 4) == 0 || strcmp(name, "tre1000") == 0 || strncmp(name, "bush", 4) == 0
            || strncmp(name, "corn", 4) == 0) {
            return 1.0f;
        }
        if (strncmp(name, "maryj", 5) == 0) {
            return 0.8f;
        }
        if (strncmp(name, "cac", 3) == 0) {
            return 0.35f;
        }
        return 0.0f;
    }

    struct Stats {
        int ticks = 0;
        int refreshes = 0;
        int recomposes = 0;
        int plants = 0;
        double keysMs = 0.0;
        double tickMs = 0.0;
        double tickMaxMs = 0.0;
    };

    struct State {
        bool initialized = false;
        bool enabled = false;
        bool ticker = false;
        unsigned int time = 0;
        bool frozen = false;
        // The ticker has set `time` (the draws use it from then on).
        bool ticked = false;
        unsigned int lastTick = 0;
        double lastCostMs = 0.0;
        int elevation = -1;
        std::unordered_map<int, float> stiffness;
        // Per plant object: the sway the ticker last redrew it with.
        std::unordered_map<Object*, std::vector<int>> drawn;
        // The plants of the elevation.
        std::vector<Object*> plants;
        bool listValid = false;
        unsigned int listTime = 0;
        Stats stats;
        // Last tick's plants on screen (harness dumps).
        struct Seen {
            int tile;
            int fid;
            int height;
            float sway;
            int margin;
            Rect rect;
        };
        std::vector<Seen> seen;
    };

    State gState;

    void initialize()
    {
        if (gState.initialized) {
            return;
        }
        gState.initialized = true;
        gState.enabled = settings.graphics.hd && settings.graphics.hd_wind;
    }

    float plantStiffness(int fid)
    {
        auto found = gState.stiffness.find(fid);
        if (found != gState.stiffness.end()) {
            return found->second;
        }
        float value = 0.0f;
        FrmId frmId(fid);
        if (frmId.objectType() == OBJ_TYPE_SCENERY) {
            const char* path = frmId.filePath();
            if (path != nullptr) {
                const char* base = path;
                for (const char* p = path; *p != '\0'; p++) {
                    if (*p == '\\' || *p == '/') {
                        base = p + 1;
                    }
                }
                char name[64];
                size_t length = 0;
                for (; base[length] != '\0' && base[length] != '.' && length + 1 < sizeof(name); length++) {
                    name[length] = static_cast<char>(tolower(static_cast<unsigned char>(base[length])));
                }
                name[length] = '\0';
                value = stiffnessForName(name);
            }
        }
        gState.stiffness.emplace(fid, value);
        return value;
    }

    // Output pixels of sway at the top of a plant `height` logical rows tall.
    float amplitude(float stiffness, int height)
    {
        return stiffness * std::clamp(1.2f + 0.04f * height, 1.5f, 5.5f);
    }

    int marginFor(float stiffness, int height)
    {
        return static_cast<int>(ceilf(amplitude(stiffness, height) * 0.5f)) + 1;
    }

    // The sway (output pixels at the top) of a plant now: a sway of about a
    // second, out of step per tile, under slow gusts.
    float swayNow(float stiffness, int height, int tile, unsigned int timeMs)
    {
        uint32_t hash = static_cast<uint32_t>(tile) * 2654435761u;
        float phase = (hash >> 8) * (2.0f * kPi / 16777216.0f);
        float t = timeMs / 1000.0f;
        float gust = 0.55f + 0.45f * (0.5f + 0.5f * sinf(2.0f * kPi * t / 6.3f + phase * 0.37f)) * (0.75f + 0.25f * sinf(2.0f * kPi * t / 9.7f + 1.3f));
        return amplitude(stiffness, height) * gust * sinf(2.0f * kPi * t / 1.1f + phase);
    }

    // Shift of output row `row` (0 = top) of a plant 2 * height output rows
    // tall, for a sway of `sway` at the top.
    int rowShift(float sway, int row, int height)
    {
        float fromBottom = (2.0f * height - row - 0.5f) / (2.0f * height);
        float k = std::max(0.0f, (fromBottom - kFixedShare) / (1.0f - kFixedShare));
        return static_cast<int>(lroundf(sway * k * k));
    }

    bool mapShown()
    {
        Window* window = windowGetWindow(gIsoWindow);
        return window != nullptr && (window->flags & WINDOW_HIDDEN) == 0 && elevationIsValid(gElevation) && !isoIsDisabled();
    }

    struct Collect {
        std::vector<Object*> plants;
    };

    void collectPlant(Object* object, void* context)
    {
        if ((object->flags & OBJECT_HIDDEN) != 0 || object->tile == -1 || FrmId(object).objectType() != OBJ_TYPE_SCENERY) {
            return;
        }
        if (plantStiffness(object->fid) > 0.0f) {
            static_cast<Collect*>(context)->plants.push_back(object);
        }
    }

    void windTick()
    {
        if (!gState.enabled) {
            return;
        }
        unsigned int now = getTicks();
        // At most kTickMs apart, and never more than about a fifth of the
        // time (a forest at 3440x1440 is slowed down rather than the game).
        if (getTicksBetween(now, gState.lastTick) < std::max(kTickMs, static_cast<unsigned int>(gState.lastCostMs * 5.0))) {
            return;
        }
        gState.lastTick = now;
        const unsigned char* basePalette8;
        if (!mapShown() || tileHdCurrentLayer(&basePalette8) == nullptr) {
            return;
        }

        auto start = std::chrono::steady_clock::now();
        if (gState.elevation != gElevation || gState.drawn.size() > 8192) {
            gState.drawn.clear();
            gState.listValid = false;
            gState.elevation = gElevation;
        }
        if (!gState.frozen) {
            gState.time = now;
        }
        gState.ticked = true;

        // The part of the map window's surface that is drawn.
        Rect region;
        if (mapViewIsActive()) {
            mapViewGetRegion(&region);
        } else {
            region.left = 0;
            region.top = 0;
            region.right = windowGetWidth(gIsoWindow) - 1;
            region.bottom = windowGetHeight(gIsoWindow) - 1;
        }

        // The plants of this elevation (looked up again every second; freed
        // objects leave the list at once, see hdWindForget).
        if (!gState.listValid || getTicksBetween(now, gState.listTime) >= 1000) {
            Collect collect;
            objectVisitAtElevation(gElevation, collectPlant, &collect);
            gState.plants = std::move(collect.plants);
            gState.listValid = true;
            gState.listTime = now;
        }

        gState.seen.clear();
        std::vector<Object*> dirty;
        std::vector<Rect> dirtyRects;
        std::vector<Rect> fullRects;
        std::vector<std::vector<int>> newShifts;
        std::vector<int> shifts;
        for (Object* object : gState.plants) {
            if ((object->flags & OBJECT_HIDDEN) != 0 || object->elevation != gElevation) {
                continue;
            }
            Rect rect;
            objectGetRect(object, &rect);
            // The frame's height, as the draw sees it.
            int height = 0;
            CacheEntry* handle;
            Art* art = artLock(FrmId(object), &handle);
            if (art != nullptr) {
                height = artGetHeight(art, object->frame, object->rotation);
                artUnlock(handle);
            }
            if (height <= 0) {
                continue;
            }
            float stiffness = plantStiffness(object->fid);
            int margin = marginFor(stiffness, height);
            rect.left -= margin;
            rect.right += margin;
            if (rect.right < region.left || rect.left > region.right || rect.bottom < region.top || rect.top > region.bottom) {
                continue;
            }
            float sway = swayNow(stiffness, height, object->tile, gState.time);
            gState.seen.push_back({ object->tile, object->fid, height, sway, margin, rect });
            // The rows' shifts as drawn: only rows whose shift changed are
            // recomposed (from the top down to the lowest such row; the
            // shift grows with the height).
            shifts.resize(static_cast<size_t>(height) * 2);
            for (int row = 0; row < height * 2; row++) {
                shifts[row] = rowShift(sway, row, height);
            }
            auto drawn = gState.drawn.find(object);
            Rect dirtyRect = rect;
            if (drawn != gState.drawn.end() && drawn->second.size() == shifts.size()) {
                int lowest = -1;
                for (int row = height * 2 - 1; row >= 0; row--) {
                    if (drawn->second[row] != shifts[row]) {
                        lowest = row;
                        break;
                    }
                }
                if (lowest == -1) {
                    continue;
                }
                dirtyRect.bottom = std::min(rect.bottom, rect.top + lowest / 2 + 1);
            }
            dirty.push_back(object);
            dirtyRects.push_back(dirtyRect);
            fullRects.push_back(rect);
            newShifts.push_back(shifts);
        }

        // Plants known to the wind scene move in the HD layer alone; the
        // others (new on screen, or their pixels redrawn in part) get a
        // redraw, which brings them into the scene.
        double keysMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        gState.stats.keysMs += keysMs;
        std::vector<unsigned char> done(dirty.size(), 0);
        int recomposed = dirty.empty() ? 0 : objectHdWindRecompose(dirty.data(), dirtyRects.data(), static_cast<int>(dirty.size()), done.data());
        int refreshes = 0;
        for (size_t index = 0; index < dirty.size(); index++) {
            gState.drawn[dirty[index]] = std::move(newShifts[index]);
            if (done[index] == 0) {
                tileWindowRefreshRect(&fullRects[index], gElevation);
                refreshes++;
            }
        }
        gState.stats.recomposes += recomposed;

        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        gState.stats.ticks++;
        gState.stats.refreshes += refreshes;
        gState.stats.plants = static_cast<int>(gState.seen.size());
        gState.stats.tickMs += ms;
        gState.lastCostMs = ms;
        gState.stats.tickMaxMs = std::max(gState.stats.tickMaxMs, ms);
    }

} // namespace

bool hdWindEnabled()
{
    initialize();
    return gState.enabled;
}

void hdWindSetEnabled(bool enabled)
{
    initialize();
    enabled = enabled && settings.graphics.hd;
    if (enabled == gState.enabled) {
        return;
    }
    gState.enabled = enabled;
    gState.drawn.clear();
    gState.listValid = false;
    objectHdWindClear();
    if (enabled) {
        hdWindInit();
    }
    if (mapShown()) {
        tileWindowRefresh();
    }
}

void hdWindInit()
{
    initialize();
    if (!gState.ticker && gState.enabled) {
        tickersAdd(windTick);
        gState.ticker = true;
    }
}

void hdWindExit()
{
    if (gState.ticker) {
        tickersRemove(windTick);
        gState.ticker = false;
    }
    gState.drawn.clear();
    gState.plants.clear();
    gState.listValid = false;
    objectHdWindClear();
}

bool hdWindSway(Object* object, int frameHeight, std::vector<int>* shifts, int* margin)
{
    if (!gState.enabled || frameHeight <= 0) {
        return false;
    }
    float stiffness = plantStiffness(object->fid);
    if (stiffness <= 0.0f) {
        return false;
    }
    unsigned int time = gState.frozen || gState.ticked ? gState.time : getTicks();
    float sway = swayNow(stiffness, frameHeight, object->tile, time);
    *margin = marginFor(stiffness, frameHeight);
    shifts->resize(static_cast<size_t>(frameHeight) * 2);
    for (int row = 0; row < frameHeight * 2; row++) {
        (*shifts)[row] = rowShift(sway, row, frameHeight);
    }
    return true;
}

void hdWindForget(Object* object)
{
    gState.drawn.erase(object);
    gState.plants.erase(std::remove(gState.plants.begin(), gState.plants.end(), object), gState.plants.end());
}

void hdWindSetTime(int ms)
{
    initialize();
    gState.frozen = ms >= 0;
    if (gState.frozen) {
        gState.time = static_cast<unsigned int>(ms);
    }
    // The next tick redraws every plant whose sway changed.
    gState.lastTick = 0;
}

std::string hdWindDescribe()
{
    initialize();
    std::ostringstream out;
    out << "{\"enabled\": " << (gState.enabled ? "true" : "false") << ", \"time\": " << gState.time
        << ", \"stats\": {\"ticks\": " << gState.stats.ticks << ", \"refreshes\": " << gState.stats.refreshes
        << ", \"recomposes\": " << gState.stats.recomposes << ", \"keys_ms\": " << gState.stats.keysMs << ", \"show_ms\": " << objectHdWindShowMs() << ", \"plants\": " << gState.stats.plants
        << ", \"tick_ms_avg\": " << (gState.stats.ticks > 0 ? gState.stats.tickMs / gState.stats.ticks : 0.0)
        << ", \"tick_ms_max\": " << gState.stats.tickMaxMs << "}, \"plants\": [";
    bool first = true;
    for (const auto& seen : gState.seen) {
        if (!first) {
            out << ", ";
        }
        first = false;
        const char* path = FrmId(seen.fid).filePath();
        std::string art = path != nullptr ? path : "";
        std::replace(art.begin(), art.end(), '\\', '/');
        out << "{\"tile\": " << seen.tile << ", \"art\": \"" << art << "\", \"height\": " << seen.height
            << ", \"sway\": " << seen.sway << ", \"margin\": " << seen.margin << ", \"rect\": [" << seen.rect.left << ", "
            << seen.rect.top << ", " << seen.rect.right << ", " << seen.rect.bottom << "]}";
    }
    out << "]}";
    return out.str();
}

} // namespace fallout
