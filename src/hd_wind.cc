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
    // freely they move (1 = leaves and grass; cacti are rigid and stay out). Picked from the 1863 names of
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
        // Cacti are rigid: they do not move in the wind.
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

    // Wind (Phase 2.15, revised): still air with gusts. A gust starts at a
    // random time and place, sweeps downwind over an area a few hexes wide for
    // a few seconds, bending the plants it reaches away from it (with a
    // flutter while it blows), and lets them spring back with a small
    // overshoot. Everything is a function of the map position and the time,
    // so every draw of a plant agrees with every other.
    //
    // Positions are in the map's own logical pixels (the screen position of
    // a tile minus that of tile 0, so panning does not move the gusts); the
    // map is cut into cells, and each cell may start one gust per time slot.
    constexpr int kGustCellWidth = 320;
    constexpr int kGustCellHeight = 240;
    constexpr unsigned int kGustSlotMs = 1700;
    constexpr int kGustChancePercent = 9;
    constexpr float kGustSpeed = 110.0f; // logical pixels per second
    constexpr unsigned int kGustRecoilMs = 1600;

    struct Gust {
        float x;
        float y;
        float dirX;
        float dirY;
        float radius;
        float strength;
        unsigned int start;
        unsigned int duration;
    };

    // Gusts made by the test mode (`hdfx gust`), on top of the natural ones.
    std::vector<Gust> gForcedGusts;

    uint32_t mix(uint32_t value)
    {
        value ^= value >> 16;
        value *= 0x7feb352du;
        value ^= value >> 15;
        value *= 0x846ca68bu;
        value ^= value >> 16;
        return value;
    }

    float unit(uint32_t value)
    {
        return (value & 0xFFFFFF) / 16777216.0f;
    }

    // The map's wind direction (screen axes, y pointing down): mostly
    // across the screen, so the lean shows; one per map.
    void windDirection(float* dirX, float* dirY)
    {
        uint32_t seed = mix(static_cast<uint32_t>(mapGetCurrentMap()) * 2654435761u + 0x9e3779b9u);
        float angle = (unit(seed) - 0.5f) * 1.2f; // within ~35 degrees of horizontal
        float sign = (seed & 0x80000000u) != 0 ? -1.0f : 1.0f;
        *dirX = sign * cosf(angle);
        *dirY = sinf(angle);
    }

    bool naturalGust(int cellX, int cellY, unsigned int slot, Gust* gust)
    {
        uint32_t seed = mix(static_cast<uint32_t>(cellX) * 73856093u ^ static_cast<uint32_t>(cellY) * 19349663u
            ^ slot * 83492791u ^ static_cast<uint32_t>(mapGetCurrentMap()) * 2654435761u);
        if (static_cast<int>(seed % 100) >= kGustChancePercent) {
            return false;
        }
        uint32_t a = mix(seed + 1);
        uint32_t b = mix(seed + 2);
        windDirection(&gust->dirX, &gust->dirY);
        gust->x = (cellX + unit(a)) * kGustCellWidth;
        gust->y = (cellY + unit(b)) * kGustCellHeight;
        gust->radius = 70.0f + 70.0f * unit(mix(seed + 3));
        gust->strength = 0.6f + 0.4f * unit(mix(seed + 4));
        gust->start = slot * kGustSlotMs + static_cast<unsigned int>(unit(mix(seed + 5)) * kGustSlotMs);
        gust->duration = 2200 + static_cast<unsigned int>(unit(mix(seed + 6)) * 1800.0f);
        return true;
    }

    float smoothstep(float value)
    {
        value = std::clamp(value, 0.0f, 1.0f);
        return value * value * (3.0f - 2.0f * value);
    }

    // How far a gust bends a plant at (x, y) now: downwind lean while it
    // blows, a damped swing back afterwards.
    float gustLean(const Gust& gust, float x, float y, unsigned int timeMs, float phase)
    {
        if (timeMs < gust.start) {
            return 0.0f;
        }
        float age = static_cast<float>(timeMs - gust.start);
        if (age > gust.duration + kGustRecoilMs) {
            return 0.0f;
        }
        float travelled = kGustSpeed * std::min(age, static_cast<float>(gust.duration)) / 1000.0f;
        float centreX = gust.x + gust.dirX * travelled;
        float centreY = gust.y + gust.dirY * travelled;
        float dx = x - centreX;
        float dy = (y - centreY) * 2.0f; // the map is foreshortened
        float reach = expf(-(dx * dx + dy * dy) / (gust.radius * gust.radius));
        if (reach < 0.02f) {
            return 0.0f;
        }
        float duration = static_cast<float>(gust.duration);
        if (age <= duration) {
            float envelope = smoothstep(age / 600.0f) * (1.0f - smoothstep((age - (duration - 800.0f)) / 800.0f));
            float flutter = 1.0f + 0.18f * sinf(2.0f * kPi * age / 380.0f + phase);
            return gust.strength * reach * envelope * flutter;
        }
        // Springing back: a little past upright, then settling.
        float after = age - duration;
        return -0.3f * gust.strength * reach * sinf(2.0f * kPi * after / 900.0f) * expf(-after / 550.0f);
    }

    // The sway (output pixels at the top; signed, downwind) of a plant now.
    float swayNow(float stiffness, int height, int tile, unsigned int timeMs)
    {
        int x;
        int y;
        int originX;
        int originY;
        if (tileToScreenXY(tile, &x, &y) != 0 || tileToScreenXY(0, &originX, &originY) != 0) {
            return 0.0f;
        }
        float px = static_cast<float>(x - originX);
        float py = static_cast<float>(y - originY);
        float phase = unit(mix(static_cast<uint32_t>(tile))) * 2.0f * kPi;

        float lean = 0.0f;
        int cellX = static_cast<int>(floorf(px / kGustCellWidth));
        int cellY = static_cast<int>(floorf(py / kGustCellHeight));
        unsigned int slot = timeMs / kGustSlotMs;
        // A gust lives up to ~5.6 s and travels up to ~440 px: look at the
        // cells around the plant and at the slots that can still reach now.
        for (unsigned int back = 0; back <= 4 && back <= slot; back++) {
            for (int cy = cellY - 2; cy <= cellY + 2; cy++) {
                for (int cx = cellX - 2; cx <= cellX + 2; cx++) {
                    Gust gust;
                    if (naturalGust(cx, cy, slot - back, &gust)) {
                        lean += gustLean(gust, px, py, timeMs, phase);
                    }
                }
            }
        }
        for (const Gust& gust : gForcedGusts) {
            lean += gustLean(gust, px, py, timeMs, phase);
        }
        if (lean == 0.0f) {
            return 0.0f;
        }
        float dirX;
        float dirY;
        windDirection(&dirX, &dirY);
        lean = std::clamp(lean, -0.4f, 1.2f);
        return amplitude(stiffness, height) * lean * (dirX >= 0.0f ? 1.0f : -1.0f);
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
        // While the map view pans or zooms the whole view is presented
        // every frame anyway; the plants wait (a fraction of a second).
        if (mapViewIsMoving()) {
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

void hdWindGust(int tile, unsigned int startMs)
{
    initialize();
    int x;
    int y;
    int originX;
    int originY;
    if (tileToScreenXY(tile, &x, &y) != 0 || tileToScreenXY(0, &originX, &originY) != 0) {
        return;
    }
    Gust gust;
    windDirection(&gust.dirX, &gust.dirY);
    // Starts a little upwind, so it reaches the tile while it blows hardest.
    gust.x = static_cast<float>(x - originX) - gust.dirX * 120.0f;
    gust.y = static_cast<float>(y - originY) - gust.dirY * 120.0f;
    // Covers the whole view at full strength (natural gusts reach 70-140 px).
    gust.radius = 4000.0f;
    gust.strength = 1.2f;
    gust.start = startMs;
    gust.duration = 3000;
    gForcedGusts.push_back(gust);
    gState.lastTick = 0;
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
