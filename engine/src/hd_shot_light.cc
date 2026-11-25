#include "hd_shot_light.h"

#include <math.h>

#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "animation.h"
#include "art.h"
#include "color.h"
#include "input.h"
#include "map.h"
#include "map_view.h"
#include "object.h"
#include "proto_types.h"
#include "settings.h"
#include "tile.h"

namespace fallout {

namespace {

    // Envelope (ms since the shot): light 1 -> 0.6 -> 0.3 -> 0.1 -> 0 over
    // 150 ms, the flash 1 -> 0.4 -> 0 over 67 ms; explosions last longer.
    constexpr unsigned int kShotMs = 150;
    constexpr unsigned int kExplosionMs = 450;

    float envelope(const float* points, const float* values, int count, float t)
    {
        if (t <= points[0]) {
            return values[0];
        }
        for (int index = 1; index < count; index++) {
            if (t <= points[index]) {
                float f = (t - points[index - 1]) / (points[index] - points[index - 1]);
                return values[index - 1] + (values[index] - values[index - 1]) * f;
            }
        }
        return values[count - 1];
    }

    float shotLight(float ms)
    {
        static const float points[] = { 0.0f, 33.0f, 67.0f, 100.0f, 150.0f };
        static const float values[] = { 1.0f, 0.6f, 0.3f, 0.1f, 0.0f };
        return envelope(points, values, 5, ms);
    }

    float shotFlash(float ms)
    {
        static const float points[] = { 0.0f, 33.0f, 67.0f };
        static const float values[] = { 1.0f, 0.4f, 0.0f };
        return envelope(points, values, 3, ms);
    }

    float explosionLight(float ms)
    {
        static const float points[] = { 0.0f, 60.0f, 150.0f, 300.0f, 450.0f };
        static const float values[] = { 1.0f, 0.9f, 0.5f, 0.15f, 0.0f };
        return envelope(points, values, 5, ms);
    }

    void damageColour(int damageType, float* colour)
    {
        static const float colours[][3] = {
            { 1.0f, 0.80f, 0.50f }, // normal: warm muzzle flame
            { 1.0f, 0.22f, 0.15f }, // laser
            { 1.0f, 0.55f, 0.20f }, // fire
            { 0.45f, 1.0f, 0.35f }, // plasma
            { 0.55f, 0.75f, 1.0f }, // electrical
            { 0.45f, 0.60f, 1.0f }, // EMP
            { 1.0f, 0.65f, 0.35f }, // explosion
        };
        int index = damageType >= 0 && damageType < DAMAGE_TYPE_COUNT ? damageType : 0;
        std::copy(colours[index], colours[index] + 3, colour);
    }

    // Screen direction of each rotation (a hex step: NE, E, SE, SW, W, NW).
    const float kFacing[ROTATION_COUNT][2] = {
        { 16.0f, -12.0f }, { 32.0f, 0.0f }, { 16.0f, 12.0f }, { -16.0f, 12.0f }, { -32.0f, 0.0f }, { -16.0f, -12.0f }
    };

    // Per frame of a fire animation (one direction): whether a round leaves
    // the muzzle on it, and the muzzle relative to the frame's anchor (the
    // point the engine places the sprite by: bottom centre).
    struct FrameInfo {
        bool shot = false;
        bool fromFlash = false;
        float mx = 0.0f;
        float my = 0.0f;
        int flashPixels = 0;
    };

    struct ArtInfo {
        std::vector<FrameInfo> frames;
    };

    std::unordered_map<uint64_t, ArtInfo> gArtInfo;

    bool brightIndex(unsigned char index)
    {
        if (index == 0) {
            return false;
        }
        int r = _cmap[index * 3] * 4;
        int g = _cmap[index * 3 + 1] * 4;
        int b = _cmap[index * 3 + 2] * 4;
        float lum = 0.30f * r + 0.59f * g + 0.11f * b;
        return lum > 170.0f && r > 200;
    }

    struct Pixel {
        int x;
        int y;
    };

    const ArtInfo& analyseArt(int fid, int rotation, int animation)
    {
        uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(fid)) << 8) | static_cast<uint64_t>(rotation & 0xFF);
        auto found = gArtInfo.find(key);
        if (found != gArtInfo.end()) {
            return found->second;
        }

        ArtInfo info;
        CacheEntry* handle;
        Art* art = artLock(FrmId(fid), &handle);
        if (art == nullptr) {
            return gArtInfo.emplace(key, info).first->second;
        }

        Rotation rot = static_cast<Rotation>(rotation);
        int count = artGetFrameCount(art);
        int action = artGetActionFrame(art);
        info.frames.resize(std::max(count, 0));

        // Bright pixels per frame, in anchor coordinates (frames aligned as
        // the engine moves the sprite by each frame's offset).
        std::vector<std::vector<Pixel>> bright(count);
        std::vector<int> anchorX(count);
        std::vector<int> anchorY(count);
        int cx = 0;
        int cy = 0;
        for (int frame = 0; frame < count; frame++) {
            int xo = 0;
            int yo = 0;
            artGetFrameOffsets(art, frame, rot, &xo, &yo);
            cx += xo;
            cy += yo;
            anchorX[frame] = cx;
            anchorY[frame] = cy;
            int width = artGetWidth(art, frame, rot);
            int height = artGetHeight(art, frame, rot);
            unsigned char* data = artGetFrameData(art, frame, rot);
            if (data == nullptr) {
                continue;
            }
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    if (brightIndex(data[y * width + x])) {
                        bright[frame].push_back({ cx - width / 2 + x, cy - (height - 1) + y });
                    }
                }
            }
        }

        // The quietest frame (fewest bright pixels) has no flash.
        int base = 0;
        int peak = 0;
        for (int frame = 0; frame < count; frame++) {
            if (bright[frame].size() < bright[base].size()) {
                base = frame;
            }
            peak = std::max(peak, static_cast<int>(bright[frame].size()));
        }
        std::unordered_set<int64_t> quiet;
        auto keyOf = [](int x, int y) { return (static_cast<int64_t>(x) << 32) ^ static_cast<uint32_t>(y); };
        for (const Pixel& pixel : bright[base]) {
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    quiet.insert(keyOf(pixel.x + dx, pixel.y + dy));
                }
            }
        }

        float fx = kFacing[rotation][0];
        float fy = kFacing[rotation][1];
        float norm = sqrtf(fx * fx + fy * fy);
        fx /= norm;
        fy /= norm;

        std::vector<std::vector<Pixel>> flash(count);
        for (int frame = 0; frame < count; frame++) {
            for (const Pixel& pixel : bright[frame]) {
                if (quiet.find(keyOf(pixel.x, pixel.y)) == quiet.end()) {
                    flash[frame].push_back(pixel);
                }
            }
            info.frames[frame].flashPixels = static_cast<int>(flash[frame].size());
        }

        // Shot frames: flash counts that peak well above the quietest frame.
        int baseCount = static_cast<int>(bright[base].size());
        int threshold = baseCount + std::max(6, (peak - baseCount) * 2 / 5);
        std::vector<int> candidates;
        for (int frame = 0; frame < count; frame++) {
            int value = static_cast<int>(bright[frame].size());
            int before = frame > 0 ? static_cast<int>(bright[frame - 1].size()) : -1;
            int after = frame + 1 < count ? static_cast<int>(bright[frame + 1].size()) : -1;
            if (value >= threshold && value >= before && value >= after && flash[frame].size() >= 3) {
                candidates.push_back(frame);
            }
        }
        std::vector<int> shots;
        if (animation == ANIM_FIRE_SINGLE || candidates.empty()) {
            int best = -1;
            for (int frame : candidates) {
                if (abs(frame - action) <= 1 && (best == -1 || abs(frame - action) < abs(best - action))) {
                    best = frame;
                }
            }
            shots.push_back(best != -1 ? best : std::clamp(action, 0, std::max(count - 1, 0)));
        } else {
            shots = candidates;
        }

        for (int frame : shots) {
            if (frame < 0 || frame >= count) {
                continue;
            }
            FrameInfo& out = info.frames[frame];
            out.shot = true;

            // The flash drawn in the frame: the cluster around its front
            // end within reach of the hands (at most kReach px sideways from
            // the critter's centre, so a flamer's long stream or a rocket's
            // trail does not count; stray bright pixels of the body that
            // moved are not connected to it); the muzzle is the cluster's
            // near end.
            constexpr int kReach = 36;
            // The feet: where the sprite is anchored before the frames'
            // offsets move it (wide frames, such as a flamer's stream, are
            // shifted by their offsets and the art's per-direction offset).
            const int feetX = -art->xOffsets[rot];
            const std::vector<Pixel>& pixels = flash[frame];
            int tip = -1;
            for (size_t index = 0; index < pixels.size(); index++) {
                if (abs(pixels[index].x - feetX) > kReach) {
                    continue;
                }
                if (tip == -1 || pixels[index].x * fx + pixels[index].y * fy > pixels[tip].x * fx + pixels[tip].y * fy) {
                    tip = static_cast<int>(index);
                }
            }
            if (tip != -1) {
                std::vector<bool> inCluster(pixels.size(), false);
                std::vector<size_t> stack = { static_cast<size_t>(tip) };
                inCluster[tip] = true;
                while (!stack.empty()) {
                    size_t current = stack.back();
                    stack.pop_back();
                    for (size_t index = 0; index < pixels.size(); index++) {
                        if (!inCluster[index] && abs(pixels[index].x - feetX) <= kReach
                            && abs(pixels[index].x - pixels[current].x) <= 2 && abs(pixels[index].y - pixels[current].y) <= 2) {
                            inCluster[index] = true;
                            stack.push_back(index);
                        }
                    }
                }
                float nearest = 1e9f;
                for (size_t index = 0; index < pixels.size(); index++) {
                    if (inCluster[index]) {
                        nearest = std::min(nearest, pixels[index].x * fx + pixels[index].y * fy);
                    }
                }
                float sumX = 0.0f;
                float sumY = 0.0f;
                int n = 0;
                for (size_t index = 0; index < pixels.size(); index++) {
                    if (inCluster[index] && pixels[index].x * fx + pixels[index].y * fy <= nearest + 2.0f) {
                        sumX += pixels[index].x;
                        sumY += pixels[index].y;
                        n++;
                    }
                }
                out.fromFlash = true;
                out.mx = sumX / n - anchorX[frame];
                out.my = sumY / n - anchorY[frame];
                continue;
            }

            // No flash drawn: the frontmost pixel at weapon height.
            int width = artGetWidth(art, frame, rot);
            int height = artGetHeight(art, frame, rot);
            unsigned char* data = artGetFrameData(art, frame, rot);
            float best = -1e9f;
            float bestX = 0.0f;
            float bestY = -(height * 0.6f);
            if (data != nullptr) {
                for (int y = 0; y < height; y++) {
                    float fromBottom = static_cast<float>(height - 1 - y) / std::max(height, 1);
                    if (fromBottom < 0.45f || fromBottom > 0.75f) {
                        continue;
                    }
                    for (int x = 0; x < width; x++) {
                        if (data[y * width + x] == 0 || abs(anchorX[frame] - width / 2 + x - feetX) > kReach) {
                            continue;
                        }
                        float lx = static_cast<float>(x - width / 2);
                        float ly = static_cast<float>(y - (height - 1));
                        float projection = lx * fx + ly * fy;
                        if (projection > best) {
                            best = projection;
                            bestX = lx;
                            bestY = ly;
                        }
                    }
                }
            }
            out.mx = bestX;
            out.my = bestY;
        }

        artUnlock(handle);
        if (gArtInfo.size() > 512) {
            gArtInfo.clear();
        }
        return gArtInfo.emplace(key, info).first->second;
    }

    struct Pending {
        Object* attacker = nullptr;
        int targetTile = -1;
        int damageType = 0;
        int animation = 0;
        unsigned int registered = 0;
        int lastFid = -1;
        int lastFrame = -1;
        bool entered = false;
        int rounds = 0;
    };

    struct PendingExplosion {
        Object* object = nullptr;
        int fid = -1;
        int damageType = DAMAGE_TYPE_EXPLOSION;
        unsigned int registered = 0;
    };

    struct Shot {
        // The muzzle: tile centre + offset (surface pixels, so the light
        // stays put while the map scrolls), or screen coordinates.
        int tile = -1;
        int elevation = 0;
        float offX = 0.0f;
        float offY = 0.0f;
        int targetTile = -1;
        int rotation = 0;
        bool screen = false;
        float sx = 0.0f;
        float sy = 0.0f;
        float tx = 0.0f;
        float ty = 0.0f;
        int damageType = 0;
        bool omni = false;
        unsigned int start = 0;
        bool fromFlash = false;
    };

    struct State {
        bool initialized = false;
        bool enabled = false;
        std::vector<Pending> pending;
        std::vector<PendingExplosion> explosions;
        std::vector<Shot> shots;
        bool hadShots = false;
        int firedCount = 0;
        bool hold = false;
        unsigned int holdAt = 0;
        unsigned int holdOffset = 0;
        // The last shots fired (harness dumps): muzzle and target on screen.
        struct Fired {
            float mx;
            float my;
            float tx;
            float ty;
            int damageType;
            bool omni;
            bool fromFlash;
            int frame;
            unsigned int time;
        };
        std::vector<Fired> fired;
    };

    State gState;

    // The shots' clock: the time, less the time the harness held it.
    unsigned int shotClock(unsigned int now);

    void initialize()
    {
        if (gState.initialized) {
            return;
        }
        gState.initialized = true;
        gState.enabled = settings.graphics.hd && settings.graphics.hd_shot_light;
    }

    unsigned int shotClock(unsigned int now)
    {
        return gState.hold ? gState.holdAt : now - gState.holdOffset;
    }

    bool tileSurfaceCentre(int tile, float* x, float* y)
    {
        int sx;
        int sy;
        if (tile < 0 || tileToScreenXY(tile, &sx, &sy) != 0) {
            return false;
        }
        *x = static_cast<float>(sx + 16);
        *y = static_cast<float>(sy + 8);
        return true;
    }

    // Muzzle and target of a shot in map window coordinates.
    bool shotScreen(const Shot& shot, float* mx, float* my, float* tx, float* ty)
    {
        if (shot.screen) {
            *mx = shot.sx;
            *my = shot.sy;
            *tx = shot.tx;
            *ty = shot.ty;
            return true;
        }
        if (shot.elevation != gElevation) {
            return false;
        }
        float cx;
        float cy;
        if (!tileSurfaceCentre(shot.tile, &cx, &cy)) {
            return false;
        }
        float x = cx + shot.offX;
        float y = cy + shot.offY;
        float targetX;
        float targetY;
        if (shot.targetTile != shot.tile && tileSurfaceCentre(shot.targetTile, &targetX, &targetY)) {
            // Aimed at the target's body at the muzzle's height.
            targetY += shot.offY;
        } else {
            targetX = x + kFacing[shot.rotation][0] * 4.0f;
            targetY = y + kFacing[shot.rotation][1] * 4.0f;
        }
        *mx = x;
        *my = y;
        *tx = targetX;
        *ty = targetY;
        mapViewSurfaceToScreen(mx, my);
        mapViewSurfaceToScreen(tx, ty);
        return true;
    }

    void remember(const Shot& shot, int frame)
    {
        float mx;
        float my;
        float tx;
        float ty;
        if (!shotScreen(shot, &mx, &my, &tx, &ty)) {
            return;
        }
        gState.fired.push_back({ mx, my, tx, ty, shot.damageType, shot.omni, shot.fromFlash, frame, shot.start });
        gState.firedCount++;
        if (gState.fired.size() > 32) {
            gState.fired.erase(gState.fired.begin());
        }
    }

    struct VisitContext {
        std::unordered_set<Object*> wanted;
        std::unordered_set<Object*> found;
    };

    void visitObject(Object* object, void* context)
    {
        VisitContext* visit = static_cast<VisitContext*>(context);
        if (visit->wanted.find(object) != visit->wanted.end()) {
            visit->found.insert(object);
        }
    }

    // Follows the registered attacks and explosions: new shots on the shot
    // frames as they are drawn.
    void observe(unsigned int now)
    {
        if (gState.pending.empty() && gState.explosions.empty()) {
            return;
        }

        // Only objects that still exist on this elevation are looked at.
        VisitContext visit;
        for (const Pending& pending : gState.pending) {
            visit.wanted.insert(pending.attacker);
        }
        for (const PendingExplosion& explosion : gState.explosions) {
            visit.wanted.insert(explosion.object);
        }
        objectVisitAtElevation(gElevation, visitObject, &visit);

        for (size_t index = 0; index < gState.pending.size();) {
            Pending& pending = gState.pending[index];
            bool done = getTicksBetween(now, pending.registered) > 8000 || visit.found.find(pending.attacker) == visit.found.end();
            if (!done) {
                Object* attacker = pending.attacker;
                FrmId frmId(attacker->fid);
                bool firing = frmId.animationType() == pending.animation;
                if (firing) {
                    pending.entered = true;
                    if (attacker->fid != pending.lastFid || attacker->frame != pending.lastFrame) {
                        pending.lastFid = attacker->fid;
                        pending.lastFrame = attacker->frame;
                        const ArtInfo& info = analyseArt(attacker->fid, attacker->rotation, pending.animation);
                        if (attacker->frame >= 0 && attacker->frame < static_cast<int>(info.frames.size())
                            && info.frames[attacker->frame].shot) {
                            const FrameInfo& frame = info.frames[attacker->frame];
                            CacheEntry* handle;
                            Art* art = artLock(frmId, &handle);
                            if (art != nullptr) {
                                Shot shot;
                                shot.tile = attacker->tile;
                                shot.elevation = attacker->elevation;
                                shot.offX = art->xOffsets[attacker->rotation] + attacker->x + frame.mx;
                                shot.offY = art->yOffsets[attacker->rotation] + attacker->y + frame.my;
                                shot.targetTile = pending.targetTile;
                                shot.rotation = attacker->rotation;
                                shot.damageType = pending.damageType;
                                shot.start = shotClock(now);
                                shot.fromFlash = frame.fromFlash;
                                artUnlock(handle);
                                gState.shots.push_back(shot);
                                remember(shot, attacker->frame);
                                pending.rounds++;
                            }
                        }
                    }
                } else if (pending.entered) {
                    done = true;
                }
            }
            if (done) {
                gState.pending.erase(gState.pending.begin() + index);
            } else {
                index++;
            }
        }

        for (size_t index = 0; index < gState.explosions.size();) {
            PendingExplosion& explosion = gState.explosions[index];
            bool done = getTicksBetween(now, explosion.registered) > 15000 || visit.found.find(explosion.object) == visit.found.end();
            if (!done) {
                Object* object = explosion.object;
                if ((object->flags & OBJECT_HIDDEN) == 0 && (explosion.fid == -1 || object->fid == explosion.fid)) {
                    Shot shot;
                    shot.tile = object->tile;
                    shot.elevation = object->elevation;
                    shot.offX = static_cast<float>(object->x);
                    shot.offY = static_cast<float>(object->y) - 10.0f;
                    shot.targetTile = object->tile;
                    shot.damageType = explosion.damageType;
                    shot.omni = true;
                    shot.start = shotClock(now);
                    gState.shots.push_back(shot);
                    remember(shot, object->frame);
                    done = true;
                }
            }
            if (done) {
                gState.explosions.erase(gState.explosions.begin() + index);
            } else {
                index++;
            }
        }
    }

} // namespace

bool hdShotLightEnabled()
{
    initialize();
    return gState.enabled;
}

void hdShotLightSetEnabled(bool enabled)
{
    initialize();
    gState.enabled = enabled && settings.graphics.hd;
    if (!gState.enabled) {
        gState.pending.clear();
        gState.explosions.clear();
    }
}

void hdShotLightAttack(Object* attacker, int targetTile, Object* weapon, int damageType, int animation)
{
    (void)weapon;
    if (!hdShotLightEnabled() || attacker == nullptr) {
        return;
    }
    if (animation != ANIM_FIRE_SINGLE && animation != ANIM_FIRE_BURST && animation != ANIM_FIRE_CONTINUOUS) {
        return;
    }
    // A new attack by the same critter replaces an unfinished one.
    gState.pending.erase(std::remove_if(gState.pending.begin(), gState.pending.end(),
                             [attacker](const Pending& pending) { return pending.attacker == attacker; }),
        gState.pending.end());
    Pending pending;
    pending.attacker = attacker;
    pending.targetTile = targetTile;
    pending.damageType = damageType;
    pending.animation = animation;
    pending.registered = getTicks();
    gState.pending.push_back(pending);
}

void hdShotLightExplosion(Object* explosion, int fid, int damageType)
{
    if (!hdShotLightEnabled() || explosion == nullptr) {
        return;
    }
    PendingExplosion pending;
    pending.object = explosion;
    pending.fid = fid;
    pending.damageType = damageType;
    pending.registered = getTicks();
    gState.explosions.push_back(pending);
}

void hdShotLightFire(float muzzleX, float muzzleY, float targetX, float targetY, int damageType, bool explosion)
{
    initialize();
    Shot shot;
    shot.screen = true;
    shot.sx = muzzleX;
    shot.sy = muzzleY;
    shot.tx = targetX;
    shot.ty = targetY;
    shot.damageType = damageType;
    shot.omni = explosion;
    shot.start = shotClock(getTicks());
    gState.shots.push_back(shot);
    remember(shot, -1);
}

bool hdShotLightCollect(float night, std::vector<HdShotLight>* shots, std::vector<HdLightFlash>* flashes)
{
    initialize();
    unsigned int now = getTicks();
    if (gState.enabled) {
        observe(now);
    }
    // The shots' clock (held by the harness: the light stays as it is).
    unsigned int clock = shotClock(now);

    const float zoom = mapViewGetZoom();
    for (size_t index = 0; index < gState.shots.size();) {
        const Shot& shot = gState.shots[index];
        float ms = static_cast<float>(getTicksBetween(clock, shot.start));
        if (ms >= (shot.omni ? kExplosionMs : kShotMs)) {
            gState.shots.erase(gState.shots.begin() + index);
            continue;
        }
        index++;

        float mx;
        float my;
        float tx;
        float ty;
        if (!shotScreen(shot, &mx, &my, &tx, &ty)) {
            continue;
        }

        HdShotLight light;
        light.mx = mx;
        light.my = my;
        damageColour(shot.damageType, light.colour);
        float scale = 0.45f + 0.55f * night;
        HdLightFlash flash;
        flash.x = mx;
        flash.y = my;
        for (int c = 0; c < 3; c++) {
            flash.colour[c] = 0.45f + 0.55f * light.colour[c];
        }
        if (shot.omni) {
            light.omni = true;
            light.reach = 90.0f * zoom;
            light.spill = 30.0f * zoom;
            light.strength = explosionLight(ms) * scale;
            flash.length = 0.0f;
            flash.core = 9.0f * zoom;
            flash.intensity = 0.8f * shotFlash(ms * 0.5f);
        } else {
            float dx = tx - mx;
            float dy = (ty - my) * 2.0f;
            float length = sqrtf(dx * dx + dy * dy);
            if (length < 1e-3f) {
                dx = 1.0f;
                dy = 0.0f;
                length = 1.0f;
            }
            light.dirX = dx / length;
            light.dirY = dy / length;
            light.reach = 70.0f * zoom;
            light.spill = 14.0f * zoom;
            light.strength = shotLight(ms) * scale;
            flash.dirX = light.dirX;
            flash.dirY = light.dirY;
            flash.length = 13.0f * zoom;
            flash.width = 1.5f * zoom;
            flash.core = 2.0f * zoom;
            flash.intensity = shotFlash(ms);
        }
        if (light.strength > 0.0f) {
            shots->push_back(light);
        }
        if (flash.intensity > 0.0f) {
            flashes->push_back(flash);
        }
    }

    bool active = !gState.shots.empty();
    bool result = active || gState.hadShots;
    gState.hadShots = active;
    return result;
}

int hdShotLightFiredCount()
{
    return gState.firedCount;
}

void hdShotLightHold(bool hold)
{
    initialize();
    unsigned int now = getTicks();
    if (hold && !gState.hold) {
        gState.hold = true;
        gState.holdAt = now - gState.holdOffset;
    } else if (!hold && gState.hold) {
        gState.hold = false;
        // Shots keep their age: the clock resumes where it was held.
        gState.holdOffset = now - gState.holdAt;
    }
}

std::string hdShotLightDescribe()
{
    initialize();
    std::ostringstream out;
    out << "{\"enabled\": " << (gState.enabled ? "true" : "false") << ", \"pending\": " << gState.pending.size()
        << ", \"pending_explosions\": " << gState.explosions.size() << ", \"alive\": " << gState.shots.size()
        << ", \"now\": " << getTicks() << ", \"fired\": [";
    bool first = true;
    for (const auto& fired : gState.fired) {
        if (!first) {
            out << ", ";
        }
        first = false;
        out << "{\"muzzle_x\": " << fired.mx << ", \"muzzle_y\": " << fired.my << ", \"target_x\": " << fired.tx
            << ", \"target_y\": " << fired.ty << ", \"damage_type\": " << fired.damageType
            << ", \"explosion\": " << (fired.omni ? "true" : "false") << ", \"from_flash\": " << (fired.fromFlash ? "true" : "false")
            << ", \"frame\": " << fired.frame << ", \"time\": " << fired.time << "}";
    }
    out << "]}";
    return out.str();
}

} // namespace fallout
