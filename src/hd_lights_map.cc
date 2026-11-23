#include "hd_lights_map.h"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "art.h"
#include "critter.h"
#include "game_mouse.h"
#include "input.h"
#include "light.h"
#include "map.h"
#include "object.h"
#include "settings.h"
#include "tile.h"
#include "window_manager.h"

namespace fallout {

namespace {

    enum class LightKind {
        Neutral,
        Lamp,
        Fire,
        Monitor,
        Slime,
        Alarm,
    };

    const char* kindName(LightKind kind)
    {
        switch (kind) {
        case LightKind::Lamp:
            return "lamp";
        case LightKind::Fire:
            return "fire";
        case LightKind::Monitor:
            return "monitor";
        case LightKind::Slime:
            return "slime";
        case LightKind::Alarm:
            return "alarm";
        default:
            return "neutral";
        }
    }

    // Colour-cycling groups (see cycle.cc): slime 229..232, monitors
    // 233..237, fire 238..247 (slow 238..242, fast 243..247), shoreline
    // 248..253 (not a light), alarm 254.
    enum CycleGroup {
        kGroupNone = -1,
        kGroupSlime,
        kGroupMonitor,
        kGroupFire,
        kGroupAlarm,
        kGroupCount,
    };

    int cycleGroup(int index)
    {
        if (index >= 229 && index <= 232) {
            return kGroupSlime;
        }
        if (index >= 233 && index <= 237) {
            return kGroupMonitor;
        }
        if (index >= 238 && index <= 247) {
            return kGroupFire;
        }
        if (index == 254) {
            return kGroupAlarm;
        }
        return kGroupNone;
    }

    // Light colours (normalised to the brightest channel) and strengths
    // (extra light factor at the light's own hex, at night).
    struct KindStyle {
        float colour[3];
        float strength;
    };

    KindStyle kindStyle(LightKind kind)
    {
        switch (kind) {
        case LightKind::Fire:
            return { { 1.0f, 0.50f, 0.16f }, 1.20f };
        case LightKind::Monitor:
            return { { 0.35f, 0.62f, 1.0f }, 0.60f };
        case LightKind::Slime:
            return { { 0.40f, 1.0f, 0.30f }, 0.50f };
        case LightKind::Alarm:
            return { { 1.0f, 0.15f, 0.10f }, 0.60f };
        case LightKind::Lamp:
            return { { 1.0f, 0.78f, 0.50f }, 0.55f };
        default:
            return { { 0.0f, 0.0f, 0.0f }, 0.0f };
        }
    }

    float luminance(const unsigned char* rgb)
    {
        return 0.30f * rgb[0] + 0.59f * rgb[1] + 0.11f * rgb[2];
    }

    // Emissive weight per palette index (glow and light pools): the
    // colour-cycling lights always; at night also bright, strongly saturated
    // colours (neon signs, lamps, lit windows: at night only what a light
    // reaches or what glows by itself is bright) and near-white pixels.
    // `neonWeights` (optional) receives the neon part alone: the signs that
    // also spill their colour onto the surfaces around them.
    void emissiveWeights(const unsigned char* palette8, float night, float* weights, float* neonWeights)
    {
        if (neonWeights != nullptr) {
            std::fill(neonWeights, neonWeights + 256, 0.0f);
        }
        for (int index = 0; index < 256; index++) {
            switch (cycleGroup(index)) {
            case kGroupFire:
            case kGroupAlarm:
                weights[index] = 1.0f;
                continue;
            case kGroupMonitor:
                weights[index] = 0.8f;
                continue;
            case kGroupSlime:
                weights[index] = 0.6f;
                continue;
            default:
                break;
            }

            weights[index] = 0.0f;
            if (index == 0 || index >= 229 || night <= 0.0f) {
                continue;
            }
            const unsigned char* rgb = palette8 + index * 3;
            float high = std::max({ rgb[0], rgb[1], rgb[2] });
            float low = std::min({ rgb[0], rgb[1], rgb[2] });
            float neon = std::clamp((high - 140.0f) / 60.0f, 0.0f, 1.0f) * std::clamp(((high - low) / std::max(high, 1.0f) - 0.65f) / 0.2f, 0.0f, 1.0f);
            float white = std::clamp((luminance(rgb) - 215.0f) / 30.0f, 0.0f, 1.0f);
            weights[index] = night * std::max(neon, 0.35f * white);
            if (neonWeights != nullptr) {
                neonWeights[index] = night * neon;
            }
        }
    }

    // Light cells of blur for the glow (tight) and the light pools (wide,
    // flattened like the ground), and their gains.
    constexpr int kGlowRadius = 1;
    constexpr int kHaloRadius = 3;
    constexpr float kHaloGain = 3.0f;
    constexpr int kPoolRadiusX = 9;
    constexpr int kPoolRadiusY = 5;
    constexpr float kGlowGain = 1.2f;
    constexpr float kPoolGain = 28.0f;
    // Neon spill: the sign's colour on the wall it hangs on and nearby
    // surfaces (radius about 35 px), as extra light and a little additive
    // light so it shows on dark walls too.
    constexpr int kSpillRadiusX = 8;
    constexpr int kSpillRadiusY = 6;
    constexpr float kSpillMulGain = 40.0f;
    constexpr float kSpillAddGain = 6.0f;

    // Pixel counts of each cycling group in a frame, and the most used
    // index of each (the light flickers with that index's colour).
    struct ArtClass {
        int counts[kGroupCount] = {};
        int dominant[kGroupCount] = {};
        int opaque = 0;
    };

    struct Light {
        int pid = 0;
        int fid = 0;
        int tile = 0;
        int radius = 0;
        int intensity = 0;
        LightKind kind = LightKind::Neutral;
        // Whether the kind came from nearby art (an invisible light source).
        bool fromNeighbour = false;
        int syncIndex = -1;
        float x = 0.0f;
        float y = 0.0f;
        HdLightFootprint footprint;
    };

    struct State {
        bool initialized = false;
        unsigned effects = 0;

        // What the lights were built for.
        bool built = false;
        int elevation = -1;
        int originX = 0;
        int originY = 0;
        unsigned int lightGeneration = 0;
        int screenWidth = 0;
        int screenHeight = 0;
        std::vector<Light> lights;
        std::unordered_map<uint64_t, ArtClass> artClasses;

        // What the field was last assembled from.
        bool hasContent = false;
        // Presented colours of the light-giving cycling entries 229..247
        // (the alarm entry 254 cycles at 30 Hz; it follows with the periodic
        // update).
        unsigned char palette[(248 - 229) * 3] = {};
        uint64_t critterKey = 0;
        unsigned int assembledAt = 0;
        int ambient = -1;

        std::vector<float> emissive;
        HdLightBox emissiveBox;
        // Neon pixels only (night), for the spill.
        std::vector<float> neon;
        HdLightBox neonBox;
        std::vector<float> blurred;

        HdLightsStats stats;
    };

    State gState;

    // Ground point (hex centre) of a tile in iso window coordinates.
    bool tileCentre(int tile, float* x, float* y)
    {
        int sx;
        int sy;
        if (tileToScreenXY(tile, &sx, &sy) != 0) {
            return false;
        }
        *x = static_cast<float>(sx + 16);
        *y = static_cast<float>(sy + 8);
        return true;
    }

    const ArtClass& classifyArt(Object* object)
    {
        uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(object->fid)) << 32)
            | (static_cast<uint64_t>(object->frame & 0xFFFF) << 8) | static_cast<uint64_t>(object->rotation & 0xFF);
        auto found = gState.artClasses.find(key);
        if (found != gState.artClasses.end()) {
            return found->second;
        }

        ArtClass result;
        CacheEntry* handle;
        Art* art = artLock(FrmId(object), &handle);
        if (art != nullptr) {
            int width;
            int height;
            int xOffset;
            int yOffset;
            unsigned char* data = artGetFrameData(art, object->frame, object->rotation, &width, &height, &xOffset, &yOffset);
            if (data != nullptr) {
                int perIndex[256] = {};
                for (int index = 0; index < width * height; index++) {
                    perIndex[data[index]]++;
                }
                for (int index = 1; index < 256; index++) {
                    if (perIndex[index] == 0) {
                        continue;
                    }
                    result.opaque += perIndex[index];
                    int group = cycleGroup(index);
                    if (group != kGroupNone) {
                        if (perIndex[index] > (result.dominant[group] != 0 ? perIndex[result.dominant[group]] : 0)) {
                            result.dominant[group] = index;
                        }
                        result.counts[group] += perIndex[index];
                    }
                }
            }
            artUnlock(handle);
        }

        return gState.artClasses.emplace(key, result).first->second;
    }

    // The light kind an art class suggests (Neutral when it has no glowing
    // pixels); `syncIndex` receives the index to flicker with.
    LightKind kindFromArt(const ArtClass& art, int* syncIndex)
    {
        constexpr int kMinPixels = 6;
        static const struct {
            int group;
            LightKind kind;
        } order[] = {
            { kGroupFire, LightKind::Fire },
            { kGroupMonitor, LightKind::Monitor },
            { kGroupAlarm, LightKind::Alarm },
            { kGroupSlime, LightKind::Slime },
        };
        int best = -1;
        for (int index = 0; index < 4; index++) {
            int group = order[index].group;
            if (art.counts[group] >= kMinPixels && (best == -1 || art.counts[group] > art.counts[order[best].group])) {
                best = index;
            }
        }
        if (best == -1) {
            return LightKind::Neutral;
        }
        *syncIndex = art.dominant[order[best].group];
        return order[best].kind;
    }

    // Lights up to 8 hexes off screen can reach it.
    constexpr float kNearbyMargin = 10 * 32.0f;

    struct NearbyArt {
        float x;
        float y;
        LightKind kind;
        int syncIndex;
    };

    struct CollectContext {
        std::vector<Object*> lit;
        std::vector<NearbyArt> glowing;
        int width;
        int height;
    };

    void collectObject(Object* object, void* context)
    {
        CollectContext* collect = static_cast<CollectContext*>(context);
        if ((object->flags & OBJECT_HIDDEN) != 0 || object->tile == -1) {
            return;
        }

        if ((object->flags & OBJECT_LIGHTING) != 0 && object->lightIntensity > 0) {
            collect->lit.push_back(object);
        }

        // Glowing art near the screen (where lights that reach it can be).
        float x;
        float y;
        if (FrmId(object).objectType() != OBJ_TYPE_CRITTER && tileCentre(object->tile, &x, &y)
            && x > -kNearbyMargin && y > -kNearbyMargin && x < collect->width + kNearbyMargin && y < collect->height + kNearbyMargin) {
            int syncIndex = -1;
            LightKind kind = kindFromArt(classifyArt(object), &syncIndex);
            if (kind != LightKind::Neutral) {
                collect->glowing.push_back({ x, y, kind, syncIndex });
            }
        }
    }

    std::vector<std::pair<int, int>>* gVisitedTiles = nullptr;

    void visitTile(MapElevation elevation, int tile, int intensity)
    {
        (void)elevation;
        gVisitedTiles->push_back({ tile, intensity });
    }

    void rebuildLights(int screenWidth, int screenHeight)
    {
        auto start = std::chrono::steady_clock::now();

        gState.lights.clear();
        if (gState.artClasses.size() > 4096) {
            gState.artClasses.clear();
        }

        CollectContext collect;
        collect.width = screenWidth;
        collect.height = screenHeight;
        objectVisitAtElevation(gElevation, collectObject, &collect);

        std::vector<std::pair<int, int>> visited;
        std::vector<HdLightSample> samples;
        std::unordered_map<int, int> values;
        std::unordered_set<int> ring;

        for (Object* object : collect.lit) {
            Light light;
            light.pid = object->pid;
            light.fid = object->fid;
            light.tile = object->tile;
            light.radius = object->lightDistance;
            light.intensity = object->lightIntensity;
            if (!tileCentre(object->tile, &light.x, &light.y)) {
                continue;
            }

            // Off-screen lights cannot reach the screen.
            float reach = (light.radius + 2) * 32.0f;
            if (light.x < -reach || light.y < -reach || light.x > screenWidth + reach || light.y > screenHeight + reach) {
                continue;
            }

            if (FrmId(object).objectType() == OBJ_TYPE_CRITTER) {
                light.kind = LightKind::Neutral;
            } else {
                light.kind = kindFromArt(classifyArt(object), &light.syncIndex);
                if (light.kind == LightKind::Neutral) {
                    // An invisible or plain light source: take the kind of
                    // the closest glowing art within two hexes.
                    float best = 64.0f * 64.0f;
                    for (const NearbyArt& art : collect.glowing) {
                        float dx = art.x - light.x;
                        float dy = (art.y - light.y) * 2.0f;
                        float distance = dx * dx + dy * dy;
                        if (distance < best) {
                            best = distance;
                            light.kind = art.kind;
                            light.syncIndex = art.syncIndex;
                            light.fromNeighbour = true;
                        }
                    }
                    if (light.kind == LightKind::Neutral) {
                        light.kind = LightKind::Lamp;
                    }
                }
            }

            if (light.kind == LightKind::Neutral) {
                gState.lights.push_back(std::move(light));
                continue;
            }

            visited.clear();
            gVisitedTiles = &visited;
            objectVisitLightTiles(object, visitTile);
            gVisitedTiles = nullptr;

            values.clear();
            for (const auto& entry : visited) {
                values[entry.first] = std::max(values[entry.first], entry.second);
            }

            // Hexes next to lit ones that the light does not reach are 0, so
            // the footprint fades out at its edge and behind walls.
            ring.clear();
            for (const auto& entry : values) {
                for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
                    int neighbour = tileGetTileInDirection(entry.first, static_cast<Rotation>(rotation), 1);
                    if (values.find(neighbour) == values.end()) {
                        ring.insert(neighbour);
                    }
                }
            }

            samples.clear();
            float scale = 1.0f / std::max(light.intensity, 1);
            for (const auto& entry : values) {
                float x;
                float y;
                if (tileCentre(entry.first, &x, &y)) {
                    samples.push_back({ x, y, std::clamp(entry.second * scale, 0.0f, 1.0f) });
                }
            }
            for (int tile : ring) {
                float x;
                float y;
                if (tileCentre(tile, &x, &y)) {
                    samples.push_back({ x, y, 0.0f });
                }
            }

            hdLightFootprintBuild(samples, 26.0f, 16.0f, 1.3f, &light.footprint);
            gState.lights.push_back(std::move(light));
        }

        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        gState.stats.rebuilds++;
        gState.stats.rebuildMs += ms;
    }

    // 0 at full daylight, 1 at night.
    float darkness()
    {
        float ambient = lightGetAmbientIntensity() / 65536.0f;
        return std::clamp((1.0f - ambient) / 0.55f, 0.0f, 1.0f);
    }

    // Flicker factor of a light: its sync index's presented brightness
    // relative to the brightest entry of the same cycle.
    float flicker(const Light& light, const unsigned char* palette8)
    {
        if (light.syncIndex < 0) {
            return 1.0f;
        }

        int first;
        int last;
        float depth;
        switch (cycleGroup(light.syncIndex)) {
        case kGroupFire:
            first = light.syncIndex <= 242 ? 238 : 243;
            last = first + 4;
            depth = 0.30f;
            break;
        case kGroupMonitor:
            first = 233;
            last = 237;
            depth = 0.15f;
            break;
        default:
            return 1.0f;
        }

        float brightest = 0.0f;
        for (int index = first; index <= last; index++) {
            brightest = std::max(brightest, luminance(palette8 + index * 3));
        }
        if (brightest <= 0.0f) {
            return 1.0f - depth;
        }
        return 1.0f - depth + depth * luminance(palette8 + light.syncIndex * 3) / brightest;
    }

    // Weight of a light's footprint at a logical point.
    float footprintAt(const HdLightFootprint& footprint, float x, float y)
    {
        int cx = static_cast<int>(floorf(x / kHdLightCell)) - footprint.cellX;
        int cy = static_cast<int>(floorf(y / kHdLightCell)) - footprint.cellY;
        if (cx < 0 || cy < 0 || cx >= footprint.cellsX || cy >= footprint.cellsY) {
            return 0.0f;
        }
        return footprint.weight[static_cast<size_t>(cy) * footprint.cellsX + cx];
    }

    struct Feet {
        float x;
        float y;
        bool dead;
    };

    struct CritterContext {
        std::vector<Feet> feet;
        uint64_t key = 1469598103934665603ull;
        int width;
        int height;
    };

    void collectCritter(Object* object, void* context)
    {
        CritterContext* critters = static_cast<CritterContext*>(context);
        if ((object->flags & OBJECT_HIDDEN) != 0 || object->tile == -1 || FrmId(object).objectType() != OBJ_TYPE_CRITTER) {
            return;
        }

        float x;
        float y;
        if (!tileCentre(object->tile, &x, &y)) {
            return;
        }
        x += object->x;
        y += object->y;
        if (x < -64 || y < -64 || x > critters->width + 64 || y > critters->height + 64) {
            return;
        }

        bool dead = critterIsDead(object);
        critters->feet.push_back({ x, y, dead });
        uint64_t values[3] = { static_cast<uint64_t>(static_cast<int64_t>(x * 4)), static_cast<uint64_t>(static_cast<int64_t>(y * 4)), dead ? 1u : 0u };
        for (uint64_t value : values) {
            critters->key = (critters->key ^ value) * 1099511628211ull;
        }
    }

    void addShadows(HdLightField* field, const CritterContext& critters, const unsigned char* palette8, float night)
    {
        for (const Feet& feet : critters.feet) {
            if (feet.dead) {
                continue;
            }

            // Contact shadow (ambient occlusion under the feet).
            hdLightFieldAddShadow(field, feet.x, feet.y + 1.0f, 13.0f, 0.0f, 5.0f, 0.30f + 0.10f * night);

            if (night <= 0.0f) {
                continue;
            }

            // Cast away from the strongest coloured light that reaches the
            // critter (one shadow: several crossing ones look like a star).
            const Light* strongest = nullptr;
            float strongestWeight = 0.15f;
            for (const Light& light : gState.lights) {
                if (light.kind == LightKind::Neutral || light.footprint.weight.empty()) {
                    continue;
                }
                float weight = footprintAt(light.footprint, feet.x, feet.y) * kindStyle(light.kind).strength * flicker(light, palette8);
                if (weight > strongestWeight) {
                    strongestWeight = weight;
                    strongest = &light;
                }
            }
            if (strongest == nullptr) {
                continue;
            }
            // Direction on the ground: screen y is foreshortened about 2:1.
            float dx = feet.x - strongest->x;
            float dy = (feet.y - strongest->y) * 2.0f;
            float distance = sqrtf(dx * dx + dy * dy);
            if (distance < 8.0f) {
                continue;
            }
            float ux = dx / distance;
            float uy = dy / distance;
            float length = std::clamp(distance * 0.35f, 12.0f, 24.0f);
            float axisX = ux * length;
            float axisY = uy * length * 0.5f;
            hdLightFieldAddShadow(field, feet.x + axisX * 0.8f, feet.y + axisY * 0.8f, axisX, axisY, 5.5f,
                std::min(0.5f, strongestWeight * 0.5f) * night);
        }
    }

    // The hex cursor is drawn into the map window; its red outline is not a
    // light.
    void excludeHexCursor(std::vector<float>* grid, int cellsX, int cellsY)
    {
        if (gGameMouseHexCursor == nullptr || (gGameMouseHexCursor->flags & OBJECT_HIDDEN) != 0
            || gGameMouseHexCursor->elevation != gElevation) {
            return;
        }
        Rect rect;
        objectGetRect(gGameMouseHexCursor, &rect);
        int x0 = std::max(rect.left / kHdLightCell, 0);
        int y0 = std::max(rect.top / kHdLightCell, 0);
        int x1 = std::min(rect.right / kHdLightCell, cellsX - 1);
        int y1 = std::min(rect.bottom / kHdLightCell, cellsY - 1);
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                float* cell = &(*grid)[(static_cast<size_t>(y) * cellsX + x) * 3];
                cell[0] = cell[1] = cell[2] = 0.0f;
            }
        }
    }

    bool mapShown()
    {
        Window* window = windowGetWindow(gIsoWindow);
        return window != nullptr && (window->flags & WINDOW_HIDDEN) == 0 && elevationIsValid(gElevation);
    }

    void initialize()
    {
        if (gState.initialized) {
            return;
        }
        gState.initialized = true;
        gState.effects = settings.graphics.hd_lights ? (static_cast<unsigned>(settings.graphics.hd_lights_effects) & kHdLightAll) : 0;
    }

} // namespace

bool hdLightsMapEnabled()
{
    initialize();
    return gState.effects != 0;
}

void hdLightsMapSetEffects(unsigned effects)
{
    initialize();
    effects &= kHdLightAll;
    if (effects == gState.effects) {
        return;
    }
    gState.effects = effects;
    gState.built = false;
    windowHdSetLitWindow(effects != 0 ? gIsoWindow : -1);
    if (mapShown()) {
        // Recompose the map window so its pixels carry (or lose) the lit flag.
        tileWindowRefresh();
    }
}

unsigned hdLightsMapGetEffects()
{
    initialize();
    return gState.effects;
}

bool hdLightsMapUpdate(const HdLightScreen& screen, HdLightField* field)
{
    initialize();

    bool sizeChanged = field->width != screen.width || field->height != screen.height;
    if (sizeChanged) {
        hdLightFieldInit(field, screen.width, screen.height);
        hdLightFieldFinish(field);
        gState.hasContent = false;
    }

    if (gState.effects == 0 || !mapShown()) {
        windowHdSetLitWindow(gState.effects != 0 ? gIsoWindow : -1);
        if (gState.hasContent) {
            hdLightFieldClear(field);
            hdLightFieldFinish(field);
            gState.hasContent = false;
            gState.built = false;
            return true;
        }
        return sizeChanged;
    }

    windowHdSetLitWindow(gIsoWindow);

    auto start = std::chrono::steady_clock::now();

    int originX;
    int originY;
    tileToScreenXY(0, &originX, &originY);
    unsigned int generation = lightGetGeneration();
    bool rebuild = !gState.built || gState.elevation != gElevation || gState.originX != originX || gState.originY != originY
        || gState.lightGeneration != generation || gState.screenWidth != screen.width || gState.screenHeight != screen.height;
    if (rebuild) {
        rebuildLights(screen.width, screen.height);
        gState.built = true;
        gState.elevation = gElevation;
        gState.originX = originX;
        gState.originY = originY;
        gState.lightGeneration = generation;
        gState.screenWidth = screen.width;
        gState.screenHeight = screen.height;
    }

    CritterContext critters;
    critters.width = screen.width;
    critters.height = screen.height;
    if ((gState.effects & kHdLightShadows) != 0) {
        objectVisitAtElevation(gElevation, collectCritter, &critters);
    }

    const unsigned char* cycling = screen.palette8 + 229 * 3;
    bool paletteChanged = memcmp(gState.palette, cycling, sizeof(gState.palette)) != 0;
    unsigned int now = getTicks();
    int ambient = lightGetAmbientIntensity();
    // Animated art and critters walking in front of glowing pixels change
    // the emissive pixels without any of the above changing.
    bool stale = getTicksBetween(now, gState.assembledAt) >= 100;
    if (!rebuild && !sizeChanged && !paletteChanged && critters.key == gState.critterKey && ambient == gState.ambient && !stale
        && gState.hasContent) {
        return false;
    }

    memcpy(gState.palette, cycling, sizeof(gState.palette));
    gState.critterKey = critters.key;
    gState.assembledAt = now;
    gState.ambient = ambient;

    hdLightFieldClear(field);
    float night = darkness();
    unsigned effects = gState.effects;

    if ((effects & (kHdLightColour | kHdLightGlow)) != 0) {
        float weights[256];
        float neonWeights[256];
        emissiveWeights(screen.palette8, night, weights, neonWeights);
        hdLightEmissive(screen.index, screen.mask, screen.width, screen.width, screen.height, screen.palette8, weights, &gState.emissive,
            &gState.emissiveBox);
        excludeHexCursor(&gState.emissive, field->cellsX, field->cellsY);
        gState.neonBox = HdLightBox();
        if ((effects & kHdLightColour) != 0 && night > 0.0f) {
            hdLightEmissive(screen.index, screen.mask, screen.width, screen.width, screen.height, screen.palette8, neonWeights, &gState.neon,
                &gState.neonBox);
            excludeHexCursor(&gState.neon, field->cellsX, field->cellsY);
        }
    }

    if ((effects & kHdLightColour) != 0) {
        float lightScale = 0.30f + 0.70f * night;
        for (const Light& light : gState.lights) {
            if (light.kind == LightKind::Neutral || light.footprint.weight.empty()) {
                continue;
            }
            KindStyle style = kindStyle(light.kind);
            // Lamps (mostly invisible light sources the maps place for the
            // night) only show at night; things that glow by themselves keep
            // a little light by day.
            float scale = light.kind == LightKind::Lamp ? night : lightScale;
            hdLightFieldAddFootprint(field, light.footprint, style.colour, style.strength * scale * flicker(light, screen.palette8));
        }

        gState.blurred = gState.emissive;
        HdLightBox box = gState.emissiveBox;
        hdLightBlur(&gState.blurred, field->cellsX, field->cellsY, kPoolRadiusX, kPoolRadiusY, 3, &box);
        hdLightFieldAddGrid(field, gState.blurred, box, kPoolGain * lightScale, false);

        if (!gState.neonBox.empty()) {
            box = gState.neonBox;
            hdLightBlur(&gState.neon, field->cellsX, field->cellsY, kSpillRadiusX, kSpillRadiusY, 3, &box);
            hdLightFieldAddGrid(field, gState.neon, box, kSpillMulGain, false);
            hdLightFieldAddGrid(field, gState.neon, box, kSpillAddGain * 255.0f, true);
        }
    }

    if ((effects & kHdLightGlow) != 0) {
        // A tight core and a wider halo.
        float scale = 255.0f * (0.45f + 0.55f * night);
        gState.blurred = gState.emissive;
        HdLightBox box = gState.emissiveBox;
        hdLightBlur(&gState.blurred, field->cellsX, field->cellsY, kGlowRadius, kGlowRadius, 3, &box);
        hdLightFieldAddGrid(field, gState.blurred, box, kGlowGain * scale, true);
        gState.blurred = gState.emissive;
        box = gState.emissiveBox;
        hdLightBlur(&gState.blurred, field->cellsX, field->cellsY, kHaloRadius, kHaloRadius, 3, &box);
        hdLightFieldAddGrid(field, gState.blurred, box, kHaloGain * scale, true);
    }

    if ((effects & kHdLightShadows) != 0) {
        addShadows(field, critters, screen.palette8, night);
    }

    hdLightFieldFinish(field);
    gState.hasContent = true;

    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    gState.stats.updates++;
    gState.stats.updateMs += ms;
    gState.stats.updateMaxMs = std::max(gState.stats.updateMaxMs, ms);
    return true;
}

static std::string jsonPath(const char* path)
{
    std::string result = path != nullptr ? path : "";
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

std::string hdLightsMapDescribe()
{
    initialize();
    std::ostringstream out;
    out << "{\"effects\": " << gState.effects << ", \"ambient\": " << lightGetAmbientIntensity()
        << ", \"darkness\": " << darkness() << ", \"lights\": [";
    bool first = true;
    for (const Light& light : gState.lights) {
        if (!first) {
            out << ", ";
        }
        first = false;
        const char* path = FrmId(light.fid).filePath();
        out << "{\"pid\": " << light.pid << ", \"art\": \"" << jsonPath(path) << "\""
            << ", \"type\": " << FrmId(light.fid).objectType() << ", \"tile\": " << light.tile
            << ", \"x\": " << light.x << ", \"y\": " << light.y << ", \"radius\": " << light.radius
            << ", \"intensity\": " << light.intensity << ", \"kind\": \"" << kindName(light.kind) << "\""
            << ", \"from_neighbour\": " << (light.fromNeighbour ? "true" : "false")
            << ", \"sync_index\": " << light.syncIndex << ", \"footprint_cells\": " << light.footprint.weight.size() << "}";
    }
    out << "], \"stats\": {\"updates\": " << gState.stats.updates << ", \"rebuilds\": " << gState.stats.rebuilds
        << ", \"update_ms_avg\": " << (gState.stats.updates > 0 ? gState.stats.updateMs / gState.stats.updates : 0.0)
        << ", \"update_ms_max\": " << gState.stats.updateMaxMs
        << ", \"rebuild_ms_avg\": " << (gState.stats.rebuilds > 0 ? gState.stats.rebuildMs / gState.stats.rebuilds : 0.0) << "}}";
    return out.str();
}

void hdLightsMapGetStats(HdLightsStats* stats)
{
    *stats = gState.stats;
}

void hdLightsMapResetStats()
{
    gState.stats = HdLightsStats();
}

} // namespace fallout
