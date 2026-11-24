#include "hd_dev_automation.h"

#include <lodepng.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "animation.h"
#include "combat.h"
#include "critter.h"
#include "interface.h"
#include "inventory.h"
#include "item.h"
#include "art.h"
#include "color.h"
#include "debug.h"
#include "dev_automation.h"
#include "display_monitor.h"
#include "game.h"
#include "game_mouse.h"
#include "hd_art.h"
#include "hd_lights.h"
#include "hd_lights_map.h"
#include "hd_shot_light.h"
#include "hd_wind.h"
#include "hd_text.h"
#include "input.h"
#include "map.h"
#include "map_view.h"
#include "mouse.h"
#include "object.h"
#include "party_member.h"
#include "scripts.h"
#include "svga.h"
#include "text_object.h"
#include "tile.h"
#include "window_manager.h"
#include "worldmap.h"

namespace fallout {

namespace {

    struct FadeCapture {
        std::string name;
        int percent;
        int direction; // 0 = any, 1 = fade in, -1 = fade out (to black)
    };

    // `frames`: crops of consecutive presented frames, kept in memory and
    // written when the last one is taken (writing PNGs would stall the game
    // between frames).
    struct FrameCapture {
        std::string name;
        int count = 0;
        int every = 1;
        int seen = 0;
        int left = 0;
        int top = 0;
        int width = 0;
        int height = 0;
        std::vector<std::vector<unsigned char>> images;
        std::vector<std::string> meta;
        unsigned int started = 0;
    };

    struct HdAutomationState {
        double perfStartMs = 0; // `perf reset`
        std::vector<FadeCapture> fadeCaptures;
        std::vector<FrameCapture> frameCaptures;
        unsigned int untilDeadline = 0; // `viewsettle`

        // `scrollhold`: scroll every poll until the deadline.
        bool scrolling = false;
        int scrollDx = 0;
        int scrollDy = 0;
        unsigned int scrollUntil = 0;
    };

    HdAutomationState gHd;

    // Writes the logical index plane through the 6-bit system palette, scaled
    // the way the pre-CE presenter did (`<< 2`), for before/after comparisons.
    bool writeLegacyPalettePng(const std::string& path)
    {
        const unsigned char* indices = screenGetIndexPlane();
        if (indices == nullptr) {
            return false;
        }

        const unsigned char* palette = _getSystemPalette();
        int width = screenGetWidth();
        int height = screenGetHeight();
        size_t pixelCount = static_cast<size_t>(width) * height;
        std::vector<unsigned char> image(pixelCount * 3);
        for (size_t i = 0; i < pixelCount; i++) {
            const unsigned char* entry = palette + indices[i] * 3;
            image[i * 3] = _currentGammaTable[entry[0] & 0x3F] << 2;
            image[i * 3 + 1] = _currentGammaTable[entry[1] & 0x3F] << 2;
            image[i * 3 + 2] = _currentGammaTable[entry[2] & 0x3F] << 2;
        }
        return lodepng::encode(path, image, width, height, LCT_RGB, 8) == 0;
    }

    void jsonString(std::ostream& out, const char* value)
    {
        out << '"';
        for (const char* p = value != nullptr ? value : ""; *p != '\0'; p++) {
            unsigned char ch = static_cast<unsigned char>(*p);
            if (ch == '"' || ch == '\\') {
                out << '\\' << *p;
            } else if (ch < 0x20) {
                char buffer[8];
                snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                out << buffer;
            } else {
                out << *p;
            }
        }
        out << '"';
    }

    // The map window position of an object's sprite centre (through the map
    // view's pan and zoom).
    bool objectScreenCentre(Object* object, float* x, float* y)
    {
        if (object == nullptr || object->tile == -1) {
            return false;
        }
        Rect rect;
        objectGetRect(object, &rect);
        *x = (rect.left + rect.right + 1) * 0.5f;
        *y = (rect.top + rect.bottom + 1) * 0.5f;
        mapViewSurfaceToScreen(x, y);
        return true;
    }

    // The map view and the player as JSON (`viewdump`, `frames` metadata).
    std::string describeView()
    {
        std::ostringstream out;
        MapViewTransform t;
        mapViewGetTransform(&t);
        Rect region = { 0, 0, -1, -1 };
        if (mapViewIsActive()) {
            mapViewGetRegion(&region);
        }
        out << "{\"active\": " << (mapViewIsActive() ? "true" : "false") << ", \"zoom\": " << t.zoom
            << ", \"target_zoom\": " << mapViewGetTargetZoom() << ", \"pan_x\": " << t.panX << ", \"pan_y\": " << t.panY
            << ", \"screen\": [" << t.screenWidth << ", " << t.screenHeight << "], \"surface\": [" << t.surfaceWidth << ", "
            << t.surfaceHeight << "], \"region\": [" << region.left << ", " << region.top << ", " << region.right << ", "
            << region.bottom << "], \"centre_tile\": " << gCenterTile << ", \"ticks\": " << getRealTicks();
        if (gDude != nullptr && gDude->tile != -1) {
            int hexX;
            int hexY;
            tileToScreenXY(gDude->tile, &hexX, &hexY);
            float sx = hexX + 16.0f + gDude->x;
            float sy = hexY + 8.0f + gDude->y;
            mapViewSurfaceToScreen(&sx, &sy);
            float hx = hexX + 16.0f;
            float hy = hexY + 8.0f;
            mapViewSurfaceToScreen(&hx, &hy);
            out << ", \"dude\": {\"tile\": " << gDude->tile << ", \"offset\": [" << gDude->x << ", " << gDude->y
                << "], \"hex_screen\": [" << hx << ", " << hy << "], \"feet_screen\": [" << sx << ", " << sy << "]}";
        }
        out << ", \"moving_lights\": " << hdLightsMapDescribeMoving();
        out << ", \"party\": [";
        bool first = true;
        for (Object* member : get_all_party_members_objects(false)) {
            float x;
            float y;
            if (!objectScreenCentre(member, &x, &y)) {
                continue;
            }
            if (!first) {
                out << ", ";
            }
            first = false;
            out << "{\"pid\": " << member->pid << ", \"tile\": " << member->tile << ", \"screen\": [" << x << ", " << y << "]}";
        }
        out << "]}";
        return out.str();
    }

    // Takes the due `frames` captures (called every poll).
    void captureFrames()
    {
        auto& captures = gHd.frameCaptures;
        for (auto it = captures.begin(); it != captures.end();) {
            if (it->seen++ % it->every != 0) {
                ++it;
                continue;
            }
            const uint32_t* framebuffer = screenGetFramebuffer();
            int fbWidth = screenGetFramebufferWidth();
            int fbHeight = screenGetFramebufferHeight();
            if (framebuffer != nullptr) {
                int left = std::clamp(it->left, 0, fbWidth);
                int top = std::clamp(it->top, 0, fbHeight);
                int width = std::clamp(it->width, 0, fbWidth - left);
                int height = std::clamp(it->height, 0, fbHeight - top);
                std::vector<unsigned char> image(static_cast<size_t>(width) * height * 3);
                for (int y = 0; y < height; y++) {
                    const uint32_t* row = framebuffer + static_cast<size_t>(top + y) * fbWidth + left;
                    for (int x = 0; x < width; x++) {
                        unsigned char* pixel = &image[(static_cast<size_t>(y) * width + x) * 3];
                        pixel[0] = (row[x] >> 16) & 0xFF;
                        pixel[1] = (row[x] >> 8) & 0xFF;
                        pixel[2] = row[x] & 0xFF;
                    }
                }
                it->images.push_back(std::move(image));
                it->meta.push_back(describeView());
                it->width = width;
                it->height = height;
                it->left = left;
                it->top = top;
            }
            if (static_cast<int>(it->images.size()) < it->count) {
                ++it;
                continue;
            }
            std::ofstream meta(devAutomationOutputPath(it->name, ".json"));
            meta << "{\"crop\": [" << it->left << ", " << it->top << ", " << it->width << ", " << it->height << "], \"frames\": [\n";
            for (size_t index = 0; index < it->images.size(); index++) {
                char suffix[16];
                snprintf(suffix, sizeof(suffix), "_%02d", static_cast<int>(index));
                lodepng::encode(devAutomationOutputPath(it->name + suffix, ".png"), it->images[index], it->width, it->height, LCT_RGB, 8);
                meta << it->meta[index] << (index + 1 < it->images.size() ? ",\n" : "\n");
            }
            meta << "]}\n";
            debugPrint("AUTOMATION: wrote %d frames %s\n", static_cast<int>(it->images.size()), it->name.c_str());
            it = captures.erase(it);
        }
    }

    bool commandLegacyshot(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // The 8-bit window composition (640x480 at the logical size),
        // also in HD mode: what HD drawing must never change.
        if (args.empty()) {
            devAutomationFail(command, "missing name");
        }
        std::string path = devAutomationOutputPath(args[0], ".png");
        if (!screenshotSaveLegacyPng(path.c_str())) {
            devAutomationFail(command, "screenshot failed");
        }
        return true;
    }

    bool commandFbshot(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        if (args.empty()) {
            devAutomationFail(command, "missing name");
        }
        std::string path = devAutomationOutputPath(args[0], ".png");
        if (!screenshotSaveFramebufferPng(path.c_str())) {
            devAutomationFail(command, "framebuffer screenshot failed");
        }
        debugPrint("AUTOMATION: wrote %s\n", path.c_str());
        return true;
    }

    bool commandHdpack(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // Switches the HD pack live ("-" = none); later HD draws use it.
        if (args.empty()) {
            devAutomationFail(command, "missing pack name");
        }
        hdArtSetPack(args[0] == "-" ? "" : args[0].c_str());
        if (strcmp(devAutomationGetState(), "game") == 0) {
            // Floors and roofs draw with the new pack.
            tileWindowRefresh();
        }
        return true;
    }

    bool commandHdtext(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // Switches HD text live: off, sharp or smooth (text drawn later
        // uses it; screens redraw their text when they are reopened).
        HdGlyphStyle style = hdTextGetStyle();
        if (args.empty() || (args[0] != "off" && !hdGlyphStyleFromName(args[0].c_str(), &style))) {
            devAutomationFail(command, "usage: hdtext off|sharp|smooth");
        }
        hdTextSetMode(args[0] != "off", style);
        return true;
    }

    bool commandHdtextstats(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // HD text counters (strings drawn at 2x, remembered strings
        // drawn at a blit, glyphs made, time spent) to <name>.json.
        if (args.empty()) {
            devAutomationFail(command, "missing name");
        }
        HdTextStats stats;
        hdTextGetStats(&stats);
        std::ofstream out(devAutomationOutputPath(args[0], ".json"));
        out << "{\"active\": " << (hdTextIsActive() ? "true" : "false")
            << ", \"style\": ";
        jsonString(out, hdGlyphStyleName(hdTextGetStyle()));
        out << ", \"draws\": " << stats.draws
            << ", \"replays\": " << stats.replays
            << ", \"glyphs_cached\": " << stats.glyphsCached
            << ", \"total_ms\": " << stats.totalMs << "}\n";
        return true;
    }

    bool commandFloattext(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // Floating text over the player (as the float_msg script
        // function shows it), white with a black outline.
        if (args.empty() || strcmp(devAutomationGetState(), "game") != 0 || gDude == nullptr) {
            devAutomationFail(command, "usage: floattext <text> (in a game)");
        }
        std::string text = command.rest;
        Rect rect;
        if (textObjectAdd(gDude, &text[0], 101, COLOR_WHITE, COLOR_BLACK, &rect) == 0) {
            tileWindowRefreshRect(&rect, gElevation);
        }
        return true;
    }

    bool commandLoot(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // Opens the loot window on a party member the way the steal
        // skill does (companions let the player take and give freely),
        // or starts a dialogue with one; the game loop handles the
        // request (party index 1 = the first companion).
        int index = args.empty() ? 1 : atoi(args[0].c_str());
        if (strcmp(devAutomationGetState(), "game") != 0 || gDude == nullptr) {
            devAutomationFail(command, "usage: loot|talk [<party index>] (in a game)");
        }
        std::vector<Object*> party = get_all_party_members_objects(false);
        if (index < 0 || index >= static_cast<int>(party.size())) {
            devAutomationFail(command, "no such party member");
        }
        if (name == "loot") {
            scriptsRequestStealing(gDude, party[index]);
        } else {
            scriptsRequestDialog(party[index]);
        }
        return true;
    }

    bool commandMonitor(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // A line in the interface bar's message monitor.
        displayMonitorAddMessage(command.rest.c_str());
        return true;
    }

    bool commandHdprobe(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // Looks up the HD image for frame 0 of an art file (with the 2x
        // legacy fallback) and writes it as <name>.png plus <name>.json.
        if (args.size() < 2) {
            devAutomationFail(command, "usage: hdprobe <art path> <name>");
        }
        std::string artPath = args[0];
        std::replace(artPath.begin(), artPath.end(), '/', '\\');
        FrmImage legacy;
        bool haveLegacy = legacy.lock(artPath.c_str());
        bool fromLegacy = false;
        std::shared_ptr<const HdImage> image = hdArtGetFrame(args[0].c_str(), 0, 0,
            haveLegacy ? legacy.getData() : nullptr,
            haveLegacy ? legacy.getWidth() : 0,
            haveLegacy ? legacy.getHeight() : 0,
            haveLegacy ? legacy.getWidth() : 0,
            &fromLegacy);

        if (image != nullptr) {
            std::vector<unsigned char> rgba(image->pixels.size() * 4);
            for (size_t i = 0; i < image->pixels.size(); i++) {
                uint32_t pixel = image->pixels[i];
                rgba[i * 4] = (pixel >> 16) & 0xFF;
                rgba[i * 4 + 1] = (pixel >> 8) & 0xFF;
                rgba[i * 4 + 2] = pixel & 0xFF;
                rgba[i * 4 + 3] = pixel >> 24;
            }
            lodepng::encode(devAutomationOutputPath(args[1], ".png"), rgba, image->width, image->height, LCT_RGBA, 8);
        }

        std::ofstream out(devAutomationOutputPath(args[1], ".json"));
        out << "{\"found\": " << (image != nullptr ? "true" : "false")
            << ", \"from_legacy\": " << (fromLegacy ? "true" : "false")
            << ", \"width\": " << (image != nullptr ? image->width : 0)
            << ", \"height\": " << (image != nullptr ? image->height : 0)
            << ", \"legacy_width\": " << (haveLegacy ? legacy.getWidth() : 0)
            << ", \"legacy_height\": " << (haveLegacy ? legacy.getHeight() : 0)
            << ", \"pack\": ";
        jsonString(out, hdArtGetPack().c_str());
        out << "}\n";
        return true;
    }

    bool commandHdlights(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // `hdlights off|colour|glow|shadows|all|<mask>` switches HD lights
        // live; `hdlights dump <name>` writes the emitters to <name>.json.
        if (args.empty()) {
            devAutomationFail(command, "usage: hdlights off|colour|glow|shadows|all|<mask> | dump <name>");
        }
        if (args[0] == "field") {
            // The applied field at light-cell resolution: <name>_mul.png
            // (extra light, 1.0 = 255), <name>_add.png (glow) and
            // <name>_shade.png (shadow cells, darkness as grey).
            if (args.size() < 2) {
                devAutomationFail(command, "usage: hdlights field <name>");
            }
            const HdLightField* field = presenterGetLightField();
            if (field != nullptr) {
                for (int kind = 0; kind < 3; kind++) {
                    int width = kind == 2 ? field->shadeX : field->cellsX;
                    int height = kind == 2 ? field->shadeY : field->cellsY;
                    std::vector<unsigned char> rgb(static_cast<size_t>(width) * height * 3);
                    for (size_t index = 0; index < rgb.size(); index++) {
                        float value = kind == 0 ? field->mul[index] * 255.0f
                                                : (kind == 1 ? field->add[index] : field->shade[index / 3] * 255.0f);
                        rgb[index] = static_cast<unsigned char>(std::clamp(value, 0.0f, 255.0f));
                    }
                    static const char* suffix[3] = { "_mul", "_add", "_shade" };
                    lodepng::encode(devAutomationOutputPath(args[1] + suffix[kind], ".png"), rgb, width, height, LCT_RGB, 8);
                }
            }
            return true;
        }
        if (args[0] == "dump") {
            if (args.size() < 2) {
                devAutomationFail(command, "usage: hdlights dump <name>");
            }
            std::ofstream out(devAutomationOutputPath(args[1], ".json"));
            out << hdLightsMapDescribe() << "\n";
            return true;
        }
        unsigned effects;
        if (args[0] == "off") {
            effects = 0;
        } else if (args[0] == "colour") {
            effects = kHdLightColour;
        } else if (args[0] == "glow") {
            effects = kHdLightColour | kHdLightGlow;
        } else if (args[0] == "shadows") {
            effects = kHdLightShadows;
        } else if (args[0] == "all") {
            effects = kHdLightAll;
        } else {
            effects = static_cast<unsigned>(atoi(args[0].c_str()));
        }
        hdLightsMapSetEffects(effects);
        return true;
    }

    struct NearestCritter {
        std::vector<std::pair<int, Object*>> found;
        std::vector<Object*> party;
    };

    void visitNearestCritter(Object* object, void* context)
    {
        NearestCritter* nearest = static_cast<NearestCritter*>(context);
        if (object == gDude || FrmId(object).objectType() != OBJ_TYPE_CRITTER || (object->flags & OBJECT_HIDDEN) != 0
            || critterIsDead(object)
            || std::find(nearest->party.begin(), nearest->party.end(), object) != nearest->party.end()) {
            return;
        }
        nearest->found.push_back({ objectGetDistanceBetween(gDude, object), object });
    }

    bool commandShoot(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // `shoot <weapon pid> nearest|party <n> [secondary]`: the player gets
        // the weapon (loaded) in the right hand and attacks the nearest
        // critter outside the party (or a party member) as a click would in
        // combat (primary or secondary mode, e.g. a burst).
        if (args.size() < 2 || strcmp(devAutomationGetState(), "game") != 0 || gDude == nullptr) {
            devAutomationFail(command, "usage: shoot <weapon pid> nearest|party <n> [secondary] (in a game)");
        }
        // Targets in order of preference (a critter in the middle of an
        // uninterruptible animation, e.g. using a door, cannot be attacked).
        std::vector<Object*> targets;
        size_t next = 2;
        if (args[1] == "party") {
            std::vector<Object*> party = get_all_party_members_objects(false);
            int index = args.size() > 2 ? atoi(args[2].c_str()) : 1;
            next = 3;
            if (index >= 0 && index < static_cast<int>(party.size())) {
                targets.push_back(party[index]);
            }
        } else {
            NearestCritter nearest;
            nearest.party = get_all_party_members_objects(false);
            objectVisitAtElevation(gElevation, visitNearestCritter, &nearest);
            std::sort(nearest.found.begin(), nearest.found.end(),
                [](const std::pair<int, Object*>& a, const std::pair<int, Object*>& b) { return a.first < b.first; });
            for (const auto& entry : nearest.found) {
                targets.push_back(entry.second);
            }
        }
        if (targets.empty()) {
            devAutomationFail(command, "no target");
        }
        int pid = atoi(args[0].c_str());
        // The hand the interface uses (the sprite shows its weapon).
        Hand hand = interfaceGetCurrentHand();
        Object* weapon = hand == HAND_RIGHT ? critterGetItem2(gDude) : critterGetItem1(gDude);
        if (weapon == nullptr || weapon->pid != pid) {
            if (objectCreateWithProtoId(&weapon, ProtoId(pid)) == -1) {
                devAutomationFail(command, "cannot create the weapon");
            }
            itemAdd(gDude, weapon, 1);
            inventoryEquipFunc(gDude, weapon, hand, false);
            interfaceUpdateItems(false, INTERFACE_ITEM_ACTION_DEFAULT, INTERFACE_ITEM_ACTION_DEFAULT);
        }
        ammoSetQuantity(weapon, std::max(ammoGetCapacity(weapon), 1));
        // As in combat, neither side is in the middle of another animation
        // (an NPC walking about would make the attack sequence fail).
        bool secondary = args.size() > next && args[next] == "secondary";
        HitMode hitMode = hand == HAND_RIGHT ? (secondary ? HIT_MODE_RIGHT_WEAPON_SECONDARY : HIT_MODE_RIGHT_WEAPON_PRIMARY)
                                             : (secondary ? HIT_MODE_LEFT_WEAPON_SECONDARY : HIT_MODE_LEFT_WEAPON_PRIMARY);
        for (size_t index = 0; index < targets.size() && index < 5; index++) {
            Object* target = targets[index];
            // The target survives (a kill ends the scripted scene); neither
            // side is in the middle of another animation, as in combat.
            target->data.critter.hp = std::max(target->data.critter.hp, 999);
            reg_anim_clear(target);
            reg_anim_clear(gDude);
            if (_combat_attack(gDude, target, hitMode, HIT_LOCATION_UNCALLED) == 0) {
                break;
            }
        }
        return true;
    }

    bool commandHdfx(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // Phase 2.15 effects: `hdfx shot on|off`, `hdfx wind on|off`,
        // `hdfx dump <name>` (shots and wind state to <name>.json),
        // `hdfx shotlight <mx> <my> <tx> <ty> [<damage type> [explosion]]`
        // (a shot at once, map window coordinates).
        if (args.empty()) {
            devAutomationFail(command, "usage: hdfx shot|wind on|off | dump <name> | shotlight <mx> <my> <tx> <ty> [<type> [explosion]]");
        }
        if ((args[0] == "shot" || args[0] == "wind") && args.size() > 1) {
            bool on = args[1] == "on";
            if (args[0] == "shot") {
                hdShotLightSetEnabled(on);
                // Map pixels carry the lit flag from their next composition.
                windowHdSetLitWindow(hdLightsMapEnabled() ? gIsoWindow : -1);
                tileWindowRefresh();
            } else {
                hdWindSetEnabled(on);
            }
            return true;
        }
        if (args[0] == "dump" && args.size() > 1) {
            std::ofstream out(devAutomationOutputPath(args[1], ".json"));
            out << "{\"shots\": " << hdShotLightDescribe() << ", \"wind\": " << hdWindDescribe() << "}\n";
            return true;
        }
        if (args[0] == "shotlight" && args.size() > 4) {
            int type = args.size() > 5 ? atoi(args[5].c_str()) : 0;
            hdShotLightFire(static_cast<float>(atof(args[1].c_str())), static_cast<float>(atof(args[2].c_str())),
                static_cast<float>(atof(args[3].c_str())), static_cast<float>(atof(args[4].c_str())), type,
                args.size() > 6 && args[6] == "explosion");
            return true;
        }
        if (args[0] == "waitshot" && args.size() > 1) {
            // `hdfx waitshot <n>`: wait until n more shots have been fired,
            // then hold the shots' clock (the light stays as it is);
            // `hdfx shotclock run` lets it run again.
            static int target = 0;
            static unsigned int started = 0;
            if (first) {
                target = hdShotLightFiredCount() + atoi(args[1].c_str());
                started = now;
                hdShotLightHold(false);
            }
            if (hdShotLightFiredCount() >= target) {
                hdShotLightHold(true);
                return true;
            }
            if (getTicksBetween(now, started) > 15000) {
                devAutomationFail(command, "no shot fired");
            }
            return false;
        }
        if (args[0] == "shotclock" && args.size() > 1) {
            hdShotLightHold(args[1] == "hold");
            return true;
        }
        if (args[0] == "windtime" && args.size() > 1) {
            hdWindSetTime(args[1] == "run" ? -1 : atoi(args[1].c_str()));
            return true;
        }
        if (args[0] == "legacycheck" && args.size() > 1) {
            // The map's 8-bit buffer is the same after whole redraws with the
            // wind held at other times and with the wind off (all within
            // this command, so nothing else animates meanwhile).
            unsigned char* buffer = mapViewIsActive() ? mapViewSurfaceBuffer() : windowGetBuffer(gIsoWindow);
            int width = mapViewIsActive() ? mapViewSurfaceWidth() : windowGetWidth(gIsoWindow);
            int height = mapViewIsActive() ? mapViewSurfaceHeight() : windowGetHeight(gIsoWindow);
            // The reference: a whole redraw now (the buffer may hold
            // something not redrawn since its state changed).
            tileWindowRefresh();
            std::vector<unsigned char> before(buffer, buffer + static_cast<size_t>(width) * height);
            auto differing = [&]() {
                size_t count = 0;
                for (size_t index = 0; index < before.size(); index++) {
                    count += before[index] != buffer[index];
                }
                return count;
            };
            bool wasOn = hdWindEnabled();
            std::ofstream out(devAutomationOutputPath(args[1], ".json"));
            out << "{\"pixels\": " << before.size() << ", \"wind_was_on\": " << (wasOn ? "true" : "false") << ", \"differing\": [";
            const int times[] = { 0, 275, 550, 825 };
            for (int index = 0; index < 4; index++) {
                hdWindSetTime(times[index]);
                tileWindowRefresh();
                out << differing() << ", ";
            }
            hdWindSetEnabled(false);
            tileWindowRefresh();
            out << differing() << "]}\n";
            hdWindSetEnabled(wasOn);
            hdWindSetTime(-1);
            return true;
        }
        devAutomationFail(command, "bad hdfx arguments");
        return true;
    }

    bool commandWalk(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // `walk <rotation 0..5> <hexes> [run]`: the player walks (or
        // runs) that far in a straight line, as a click would make him.
        if (args.size() < 2 || strcmp(devAutomationGetState(), "game") != 0 || gDude == nullptr) {
            devAutomationFail(command, "usage: walk <rotation> <hexes> [run] (in a game)");
        }
        int tile = tileGetTileInDirection(gDude->tile, static_cast<Rotation>(atoi(args[0].c_str()) % ROTATION_COUNT), atoi(args[1].c_str()));
        reg_anim_begin(ANIMATION_REQUEST_RESERVED);
        if (args.size() > 2 && args[2] == "run") {
            animationRegisterRunToTile(gDude, tile, gDude->elevation, -1, 0);
        } else {
            animationRegisterMoveToTile(gDude, tile, gDude->elevation, -1, 0);
        }
        reg_anim_end();
        return true;
    }

    bool commandScrollhold(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // `scrollhold <dx> <dy> <ms>`: scroll the map as a held arrow key
        // or the mouse at the screen edge would (a step whenever the
        // scroll interval allows); later commands run meanwhile.
        if (args.size() < 3) {
            devAutomationFail(command, "usage: scrollhold <dx> <dy> <ms>");
        }
        gHd.scrolling = true;
        gHd.scrollDx = atoi(args[0].c_str());
        gHd.scrollDy = atoi(args[1].c_str());
        gHd.scrollUntil = now + static_cast<unsigned int>(atoi(args[2].c_str()));
        return true;
    }

    bool commandFrames(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // `frames <name> <count> [<x> <y> <w> <h> [<every>]]`: the next
        // <count> presented frames (every <every>th; a crop in output
        // pixels, default all) to
        // <name>_NN.png, with the view and player per frame in
        // <name>.json; later commands run meanwhile.
        if (args.size() < 2) {
            devAutomationFail(command, "usage: frames <name> <count> [<x> <y> <w> <h>]");
        }
        FrameCapture capture;
        capture.name = args[0];
        capture.count = std::max(atoi(args[1].c_str()), 1);
        capture.width = screenGetFramebufferWidth();
        capture.height = screenGetFramebufferHeight();
        if (args.size() >= 6) {
            capture.left = atoi(args[2].c_str());
            capture.top = atoi(args[3].c_str());
            capture.width = atoi(args[4].c_str());
            capture.height = atoi(args[5].c_str());
        }
        if (args.size() >= 7) {
            capture.every = std::max(atoi(args[6].c_str()), 1);
        }
        capture.started = now;
        gHd.frameCaptures.push_back(std::move(capture));
        return true;
    }

    bool commandFramesdone(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // Waits until every `frames` capture has been written.
        return gHd.frameCaptures.empty();
    }

    bool commandZoom(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // `zoom <level> [now]` (map view, [graphics] zoom=1) or
        // `zoom in|out|reset` as the keys do.
        if (args.empty()) {
            devAutomationFail(command, "usage: zoom <level> [now] | in | out | reset");
        }
        if (args[0] == "in") {
            mapViewZoomStep(1);
        } else if (args[0] == "out") {
            mapViewZoomStep(-1);
        } else if (args[0] == "reset") {
            mapViewZoomReset();
        } else {
            mapViewSetZoom(static_cast<float>(atof(args[0].c_str())), !(args.size() > 1 && args[1] == "now"));
        }
        return true;
    }

    bool commandViewsettle(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // Waits until the map view stops moving (pan and zoom), at most
        // 5 s.
        if (first) {
            gHd.untilDeadline = now + 5000;
        }
        if (mapViewIsMoving()) {
            if (now >= gHd.untilDeadline) {
                std::string view = describeView();
                devAutomationFail(command, ("the map view keeps moving: " + view).c_str());
            }
            return false;
        }
        return true;
    }

    bool commandViewdump(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        if (args.empty()) {
            devAutomationFail(command, "missing name");
        }
        std::ofstream out(devAutomationOutputPath(args[0], ".json"));
        out << describeView() << "\n";
        return true;
    }

    bool commandMousetoparty(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // `mousetoparty <n>`: the cursor to the middle of party member
        // n's sprite as shown (through pan and zoom).
        std::vector<Object*> party = get_all_party_members_objects(false);
        int index = args.empty() ? 1 : atoi(args[0].c_str());
        float x;
        float y;
        if (index < 0 || index >= static_cast<int>(party.size()) || !objectScreenCentre(party[index], &x, &y)) {
            devAutomationFail(command, "no such party member on the map");
        }
        // The opaque sprite pixel nearest the middle of the sprite (the
        // middle itself can be between the legs).
        Rect rect;
        objectGetRect(party[index], &rect);
        int middleX = (rect.left + rect.right) / 2;
        int middleY = (rect.top + rect.bottom) / 2;
        bool found = false;
        for (int radius = 0; radius < 24 && !found; radius++) {
            for (int dy = -radius; dy <= radius && !found; dy++) {
                for (int dx = -radius; dx <= radius && !found; dx++) {
                    if (std::max(std::abs(dx), std::abs(dy)) == radius
                        && _obj_intersects_with(party[index], middleX + dx, middleY + dy) != 0) {
                        // The middle of that surface pixel, as shown.
                        x = middleX + dx + 0.5f;
                        y = middleY + dy + 0.5f;
                        mapViewSurfaceToScreen(&x, &y);
                        found = true;
                    }
                }
            }
        }
        devAutomationSetMouse(static_cast<int>(x), static_cast<int>(y));
        return true;
    }

    bool commandMousetotile(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // `mousetotile <tile|dude>`: the cursor to the centre of a hex as
        // shown (through pan and zoom).
        if (args.empty() || gDude == nullptr) {
            devAutomationFail(command, "usage: mousetotile <tile|dude>");
        }
        int tile = args[0] == "dude" ? gDude->tile : atoi(args[0].c_str());
        int hexX;
        int hexY;
        if (tileToScreenXY(tile, &hexX, &hexY) != 0) {
            devAutomationFail(command, "invalid tile");
        }
        float x = hexX + 16.0f;
        float y = hexY + 8.0f;
        mapViewSurfaceToScreen(&x, &y);
        devAutomationSetMouse(static_cast<int>(x), static_cast<int>(y));
        return true;
    }

    bool commandPick(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // `pick <name>`: what the game finds under the cursor (the
        // object the player would click, the hex under the cursor) to
        // <name>.json.
        if (args.empty() || strcmp(devAutomationGetState(), "game") != 0) {
            devAutomationFail(command, "usage: pick <name> (in a game)");
        }
        int x;
        int y;
        mouseGetPosition(&x, &y);
        int mapX = x;
        int mapY = y;
        mapViewScreenToSurface(&mapX, &mapY);
        Object* object = gameMouseGetObjectUnderCursor(OBJ_TYPE_INVALID, true, gElevation);
        Object* critter = gameMouseGetObjectUnderCursor(OBJ_TYPE_CRITTER, true, gElevation);
        std::ofstream out(devAutomationOutputPath(args[0], ".json"));
        out << "{\"mouse\": [" << x << ", " << y << "], \"map\": [" << mapX << ", " << mapY << "], \"tile\": "
            << tileFromScreenXY(mapX, mapY) << ", \"hex_cursor_tile\": "
            << (gGameMouseHexCursor != nullptr ? gGameMouseHexCursor->tile : -1) << ", \"object\": ";
        if (object != nullptr) {
            out << "{\"pid\": " << object->pid << ", \"tile\": " << object->tile << ", \"type\": " << FrmId(object).objectType() << "}";
        } else {
            out << "null";
        }
        out << ", \"critter\": ";
        if (critter != nullptr) {
            out << "{\"pid\": " << critter->pid << ", \"tile\": " << critter->tile << "}";
        } else {
            out << "null";
        }
        out << "}\n";
        return true;
    }

    bool commandRefresh(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // Redraws the whole map window (timed by `perf`).
        if (strcmp(devAutomationGetState(), "game") == 0) {
            tileWindowRefresh();
        }
        return true;
    }

    bool commandBenchrefresh(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // Redraws the whole map window <count> times in a row and writes
        // the min/median/max time per redraw (and of that, the HD tile
        // drawing) to <name>.json.
        if (args.size() < 2) {
            devAutomationFail(command, "usage: benchrefresh <name> <count>");
        }
        int count = std::max(atoi(args[1].c_str()), 1);
        std::vector<double> totals;
        std::vector<double> tiles;
        std::vector<double> objects;
        if (strcmp(devAutomationGetState(), "game") == 0) {
            for (int index = 0; index < count; index++) {
                TileRenderStats before;
                tileGetRenderStats(&before);
                double start = tileRenderClockMs();
                tileWindowRefresh();
                totals.push_back(tileRenderClockMs() - start);
                TileRenderStats after;
                tileGetRenderStats(&after);
                tiles.push_back(after.hdTilesMs - before.hdTilesMs);
                objects.push_back(after.hdObjectsMs - before.hdObjectsMs);
            }
        }
        auto summary = [](std::vector<double> values) {
            std::sort(values.begin(), values.end());
            char text[128];
            if (values.empty()) {
                snprintf(text, sizeof(text), "null");
            } else {
                snprintf(text, sizeof(text), "{\"min\": %.3f, \"median\": %.3f, \"max\": %.3f}", values.front(), values[values.size() / 2], values.back());
            }
            return std::string(text);
        };
        std::ofstream out(devAutomationOutputPath(args[0], ".json"));
        out << "{\"count\": " << totals.size() << ", \"refresh_ms\": " << summary(totals) << ", \"hd_tiles_ms\": " << summary(tiles) << ", \"hd_objects_ms\": " << summary(objects) << "}\n";
        return true;
    }

    bool commandHdcheck(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // Phase 2.7 self-check of the iso window's HD floors and roofs
        // (see tileHdCheck): <name>.json with the counts, <name>.png
        // with the per-pixel classes (black legacy, grey HD tile, blue
        // legacy object, red HD on a critter or other legacy object,
        // green HD wall/scenery/item, yellow stale, cyan legacy pixel
        // differs from a fresh redraw).
        if (args.empty()) {
            devAutomationFail(command, "missing name");
        }
        TileHdCheck check;
        unsigned int cutOutsBefore = objectHdCutOutDraws();
        bool ok = tileHdCheck(&check);
        // (tileHdCheck redraws the map three times.)
        unsigned int cutOuts = (objectHdCutOutDraws() - cutOutsBefore) / 3;
        std::ofstream out(devAutomationOutputPath(args[0], ".json"));
        out << "{\"active\": " << (ok ? "true" : "false")
            << ", \"pixels\": " << check.pixels
            << ", \"hd_shown\": " << check.hdShown
            << ", \"stale_pixels\": " << check.stalePixels
            << ", \"stale_tile_pixels\": " << check.staleTilePixels
            << ", \"legacy_changed\": " << check.legacyChanged
            << ", \"object_pixels\": " << check.objectPixels
            << ", \"object_pixels_hd\": " << check.objectPixelsHd
            << ", \"scenery_pixels\": " << check.sceneryPixels
            << ", \"scenery_pixels_hd\": " << check.sceneryPixelsHd
            << ", \"legacy_object_pixels\": " << check.legacyObjectPixels
            << ", \"legacy_object_pixels_hd\": " << check.legacyObjectPixelsHd
            << ", \"tile_pixels\": " << check.tilePixels
            << ", \"tile_pixels_hd\": " << check.tilePixelsHd
            << ", \"cycling_hd\": " << check.cyclingHd
            << ", \"cut_out_draws\": " << cutOuts << "}\n";
        if (ok) {
            static const unsigned char kColors[7][3] = { { 0, 0, 0 }, { 128, 128, 128 }, { 0, 0, 255 }, { 255, 0, 0 }, { 255, 255, 0 }, { 0, 255, 255 }, { 0, 200, 0 } };
            int width = mapViewIsActive() ? mapViewSurfaceWidth() : windowGetWidth(gIsoWindow);
            int height = static_cast<int>(check.mask.size()) / std::max(width, 1);
            std::vector<unsigned char> rgb(check.mask.size() * 3);
            for (size_t i = 0; i < check.mask.size(); i++) {
                memcpy(&rgb[i * 3], kColors[check.mask[i]], 3);
            }
            lodepng::encode(devAutomationOutputPath(args[0], ".png"), rgb, width, height, LCT_RGB, 8);
        }
        return true;
    }

    bool commandHdstats(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        if (args.empty()) {
            devAutomationFail(command, "missing name");
        }
        HdArtStats stats;
        hdArtGetStats(&stats);
        std::ofstream out(devAutomationOutputPath(args[0], ".json"));
        out << "{\"assets_resolved\": " << stats.assetsResolved
            << ", \"assets_with_hd\": " << stats.assetsWithHd
            << ", \"frames_cached\": " << stats.framesCached
            << ", \"bytes_cached\": " << stats.bytesCached
            << ", \"hits\": " << stats.hits
            << ", \"misses\": " << stats.misses << "}\n";
        return true;
    }

    bool commandFadeshot(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        if (args.size() < 2) {
            devAutomationFail(command, "usage: fadeshot <name> <percent> [in|out]");
        }
        FadeCapture capture;
        capture.name = args[0];
        capture.percent = atoi(args[1].c_str());
        capture.direction = 0;
        if (args.size() > 2) {
            capture.direction = args[2] == "in" ? 1 : (args[2] == "out" ? -1 : 0);
        }
        gHd.fadeCaptures.push_back(capture);
        return true;
    }

    bool commandPerf(const DevAutomationCommand& command, bool first)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();
        (void)name;
        (void)args;
        (void)now;
        (void)first;

        // `perf reset` starts a measurement; `perf <name>` writes the
        // presenter counters gathered since then and starts a new one.
        if (args.empty()) {
            devAutomationFail(command, "missing name");
        }
        if (args[0] != "reset") {
            PresenterStats stats;
            presenterGetStats(&stats);
            std::string path = devAutomationOutputPath(args[0], ".json");
            FILE* stream = fopen(path.c_str(), "wt");
            if (stream == nullptr) {
                devAutomationFail(command, "cannot write perf file");
            }
            TileRenderStats render;
            tileGetRenderStats(&render);
            HdLightsStats lights;
            hdLightsMapGetStats(&lights);
            fprintf(stream,
                "{\n \"framebuffer\": [%d, %d],\n \"presents\": %llu,\n \"present_avg_ms\": %.4f,\n \"present_max_ms\": %.4f,\n"
                " \"convert_total_ms\": %.4f,\n \"converted_pixels\": %llu,\n \"palette_total_ms\": %.4f,\n \"palette_updates\": %llu,\n"
                " \"elapsed_ms\": %.1f,\n \"map_refreshes\": %llu,\n \"map_refresh_total_ms\": %.4f,\n \"map_refresh_max_ms\": %.4f,\n"
                " \"scrolls\": %llu,\n \"scroll_total_ms\": %.4f,\n \"scroll_max_ms\": %.4f,\n \"hd_tiles_total_ms\": %.4f,\n"
                " \"hd_objects_total_ms\": %.4f,\n \"uploaded_pixels\": %llu,\n \"light_total_ms\": %.4f,\n \"light_updates\": %llu,\n"
                " \"light_field_total_ms\": %.4f,\n \"light_field_max_ms\": %.4f,\n \"light_rebuilds\": %d,\n \"light_rebuild_total_ms\": %.4f\n}\n",
                stats.framebufferWidth, stats.framebufferHeight, static_cast<unsigned long long>(stats.presents), stats.presentMs, stats.presentMaxMs,
                stats.convertMs, static_cast<unsigned long long>(stats.convertedPixels), stats.paletteMs, static_cast<unsigned long long>(stats.paletteUpdates),
                tileRenderClockMs() - gHd.perfStartMs, render.refreshes, render.refreshMs, render.refreshMaxMs,
                render.scrolls, render.scrollMs, render.scrollMaxMs, render.hdTilesMs, render.hdObjectsMs,
                static_cast<unsigned long long>(stats.uploadedPixels), stats.lightMs, static_cast<unsigned long long>(stats.lightUpdates),
                lights.updateMs, lights.updateMaxMs, lights.rebuilds, lights.rebuildMs);
            fclose(stream);
            MapViewStats view;
            mapViewGetStats(&view);
            std::ofstream viewOut(devAutomationOutputPath(args[0] + "_view", ".json"));
            viewOut << "{\"presents\": " << view.presents << ", \"present_total_ms\": " << view.presentMs
                    << ", \"present_max_ms\": " << view.presentMaxMs << ", \"resample_total_ms\": " << view.resampleMs << ", \"composes\": " << view.composes
                    << ", \"compose_total_ms\": " << view.composeMs << ", \"scrolls\": " << view.scrolls
                    << ", \"scroll_total_ms\": " << view.scrollMs << "}\n";
            debugPrint("AUTOMATION: wrote %s\n", path.c_str());
        }
        mapViewResetStats();
        presenterResetStats();
        tileResetRenderStats();
        hdLightsMapResetStats();
        gHd.perfStartMs = tileRenderClockMs();
        return true;
    }

    // `worldmap`: opens the world map, as a script's request would (the
    // game loop handles it; the next commands run inside the world map).
    bool commandWorldmap(const DevAutomationCommand& command, bool first)
    {
        (void)first;
        if (strcmp(devAutomationGetState(), "game") != 0) {
            devAutomationFail(command, "usage: worldmap (in a game)");
        }
        scriptsRequestWorldMap();
        return true;
    }

    // `die`: the player dies (the game loop then shows the death scene; the
    // next commands run inside it).
    // gotomap <map> [<elevation> <tile>]: the party moves to another map (a
    // map transition, as through an exit grid), e.g. into a town's other
    // maps for review sheets; without a tile the player keeps its hex.
    bool commandGotomap(const DevAutomationCommand& command, bool first)
    {
        (void)first;
        const std::vector<std::string>& args = command.args;
        if (strcmp(devAutomationGetState(), "game") != 0 || (args.size() != 1 && args.size() != 3)) {
            devAutomationFail(command, "usage: gotomap <map> [<elevation> <tile>] (in a game)");
        }
        std::vector<char> name(args[0].begin(), args[0].end());
        name.push_back('\0');
        Map map = wmMapMatchNameToIdx(name.data());
        if (map == MAP_INVALID) {
            devAutomationFail(command, "unknown map");
        }
        MapTransition transition = {};
        transition.map = map;
        transition.elevation = args.size() == 3 ? static_cast<MapElevation>(atoi(args[1].c_str())) : gElevation;
        transition.tile = args.size() == 3 ? atoi(args[2].c_str()) : -1;
        transition.rotation = ROTATION_NE;
        mapSetTransition(&transition);
        return true;
    }

    bool commandDie(const DevAutomationCommand& command, bool first)
    {
        (void)first;
        if (strcmp(devAutomationGetState(), "game") != 0 || gDude == nullptr) {
            devAutomationFail(command, "usage: die (in a game)");
        }
        gDude->data.critter.combat.results |= DAM_DEAD;
        return true;
    }

    // `scrollhold` keeps scrolling while later commands run.
    void scrollTicker()
    {
        if (!gHd.scrolling) {
            return;
        }
        if (getTicks() >= gHd.scrollUntil || strcmp(devAutomationGetState(), "game") != 0) {
            gHd.scrolling = false;
        } else {
            mapScroll(gHd.scrollDx, gHd.scrollDy);
        }
    }

} // namespace

void hdDevAutomationInit()
{
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;

    // In HD mode the screen has more detail than the window buffers.
    devAutomationSetShotWriter(screenshotSavePng);
    devAutomationRegisterTicker(scrollTicker);
    devAutomationRegisterCommand("legacyshot", commandLegacyshot);
    devAutomationRegisterCommand("fbshot", commandFbshot);
    devAutomationRegisterCommand("hdpack", commandHdpack);
    devAutomationRegisterCommand("hdtext", commandHdtext);
    devAutomationRegisterCommand("hdtextstats", commandHdtextstats);
    devAutomationRegisterCommand("floattext", commandFloattext);
    devAutomationRegisterCommand("loot", commandLoot);
    devAutomationRegisterCommand("talk", commandLoot);
    devAutomationRegisterCommand("monitor", commandMonitor);
    devAutomationRegisterCommand("hdprobe", commandHdprobe);
    devAutomationRegisterCommand("hdlights", commandHdlights);
    devAutomationRegisterCommand("walk", commandWalk);
    devAutomationRegisterCommand("shoot", commandShoot);
    devAutomationRegisterCommand("hdfx", commandHdfx);
    devAutomationRegisterCommand("scrollhold", commandScrollhold);
    devAutomationRegisterCommand("frames", commandFrames);
    devAutomationRegisterCommand("framesdone", commandFramesdone);
    devAutomationRegisterCommand("zoom", commandZoom);
    devAutomationRegisterCommand("viewsettle", commandViewsettle);
    devAutomationRegisterCommand("viewdump", commandViewdump);
    devAutomationRegisterCommand("mousetoparty", commandMousetoparty);
    devAutomationRegisterCommand("mousetotile", commandMousetotile);
    devAutomationRegisterCommand("pick", commandPick);
    devAutomationRegisterCommand("refresh", commandRefresh);
    devAutomationRegisterCommand("benchrefresh", commandBenchrefresh);
    devAutomationRegisterCommand("hdcheck", commandHdcheck);
    devAutomationRegisterCommand("hdstats", commandHdstats);
    devAutomationRegisterCommand("fadeshot", commandFadeshot);
    devAutomationRegisterCommand("perf", commandPerf);
    devAutomationRegisterCommand("worldmap", commandWorldmap);
    devAutomationRegisterCommand("die", commandDie);
    devAutomationRegisterCommand("gotomap", commandGotomap);
}

void hdDevAutomationOnFadeStep(int step, int steps, bool toBlack)
{
    if (!devAutomationIsActive() || gHd.fadeCaptures.empty() || steps <= 0) {
        return;
    }

    auto& captures = gHd.fadeCaptures;
    for (auto it = captures.begin(); it != captures.end();) {
        bool directionMatches = it->direction == 0 || (it->direction == 1 && !toBlack) || (it->direction == -1 && toBlack);
        if (directionMatches && step * 100 >= it->percent * steps) {
            std::string path = devAutomationOutputPath(it->name, ".png");
            screenshotSaveFramebufferPng(path.c_str());
            writeLegacyPalettePng(devAutomationOutputPath(it->name + "_6bit", ".png"));
            debugPrint("AUTOMATION: wrote %s (fade step %d/%d)\n", path.c_str(), step, steps);
            it = captures.erase(it);
        } else {
            ++it;
        }
    }
}

void hdDevAutomationMark(const char* state)
{
    devAutomationSetState(state);
}

void hdDevAutomationOnPresent()
{
    if (devAutomationIsActive() && !gHd.frameCaptures.empty()) {
        captureFrames();
    }
}

} // namespace fallout
