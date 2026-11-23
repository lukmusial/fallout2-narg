#include "dev_automation.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <lodepng.h>

#include <algorithm>

#include "animation.h"
#include "art.h"
#include "color.h"
#include "critter.h"
#include "debug.h"
#include "display_monitor.h"
#include "game.h"
#include "game_mouse.h"
#include "hd_art.h"
#include "hd_lights.h"
#include "hd_lights_map.h"
#include "hd_text.h"
#include "input.h"
#include "item.h"
#include "kb.h"
#include "map.h"
#include "map_view.h"
#include "mouse.h"
#include "object.h"
#include "party_member.h"
#include "perk.h"
#include "platform_compat.h"
#include "proto.h"
#include "scripts.h"
#include "skill.h"
#include "stat.h"
#include "svga.h"
#include "text_object.h"
#include "tile.h"
#include "trait.h"
#include "win32.h"
#include "window_manager.h"
#include "worldmap.h"

namespace fallout {

namespace {

    constexpr int kDefaultUntilTimeout = 30000;

    // A click first rests the cursor on the target (the window manager accepts a
    // press only on a button that was already hovered, and leaving the previous
    // button takes a poll of its own), then holds the button and releases it, so
    // that both the press and the release are seen by every screen.
    constexpr int kClickPollsSettle = 3;
    constexpr int kClickPollsDown = 2;
    constexpr int kClickPollsUp = 2;

    struct Command {
        std::string name;
        std::vector<std::string> args;
        std::string rest; // everything after the command name
        int line;
    };

    struct AutomationState {
        bool active = false;
        std::string outDir = "devout";
        std::vector<Command> commands;
        size_t next = 0;
        double perfStartMs = 0; // `perf reset`

        // Current command progress.
        bool waiting = false;
        unsigned int waitUntil = 0;
        unsigned int untilDeadline = 0;
        int clickPhase = 0; // 0 = idle, 1..n polls since start
        int keyRepeatDone = 0;
        // Set when a command queued a key: the rest waits for the next input
        // poll, so screens see one key per frame as with a real keyboard.
        bool inputSent = false;
        bool clickRight = false;

        // Synthetic mouse.
        int mouseX = 320;
        int mouseY = 240;
        bool buttonLeft = false;
        bool buttonRight = false;

        std::string engineState;

        // Pending `fadeshot` captures.
        struct FadeCapture {
            std::string name;
            int percent;
            int direction; // 0 = any, 1 = fade in, -1 = fade out (to black)
        };
        std::vector<FadeCapture> fadeCaptures;

        // `frames`: crops of consecutive presented frames, kept in memory and
        // written when the last one is taken (writing PNGs would stall the
        // game between frames).
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
        std::vector<FrameCapture> frameCaptures;

        // `scrollhold`: scroll every poll until the deadline.
        bool scrolling = false;
        int scrollDx = 0;
        int scrollDy = 0;
        unsigned int scrollUntil = 0;
    };

    AutomationState gAutomation;

    std::string trim(const std::string& value)
    {
        size_t begin = value.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) {
            return std::string();
        }
        size_t end = value.find_last_not_of(" \t\r\n");
        return value.substr(begin, end - begin + 1);
    }

    bool loadScript(const char* path)
    {
        std::ifstream stream(path);
        if (!stream.is_open()) {
            debugPrint("AUTOMATION: cannot open script %s\n", path);
            return false;
        }

        std::string raw;
        int lineNumber = 0;
        while (std::getline(stream, raw)) {
            lineNumber++;
            std::string line = trim(raw);
            if (line.empty() || line[0] == '#') {
                continue;
            }

            Command command;
            command.line = lineNumber;
            size_t space = line.find_first_of(" \t");
            command.name = line.substr(0, space);
            if (space != std::string::npos) {
                // `text` keeps inner spacing; everything else is split on whitespace.
                command.rest = line.substr(space + 1);
                std::istringstream words(command.rest);
                std::string word;
                while (words >> word) {
                    command.args.push_back(word);
                }
            }
            gAutomation.commands.push_back(command);
        }

        debugPrint("AUTOMATION: loaded %d commands from %s\n", static_cast<int>(gAutomation.commands.size()), path);
        return true;
    }

    int keyCodeFromName(const std::string& name)
    {
        struct NamedKey {
            const char* name;
            int code;
        };

        static const NamedKey kKeys[] = {
            { "esc", KEY_ESCAPE },
            { "enter", KEY_RETURN },
            { "space", KEY_SPACE },
            { "tab", KEY_TAB },
            { "backspace", KEY_BACKSPACE },
            { "delete", KEY_DELETE },
            { "up", KEY_ARROW_UP },
            { "down", KEY_ARROW_DOWN },
            { "left", KEY_ARROW_LEFT },
            { "right", KEY_ARROW_RIGHT },
            { "home", KEY_HOME },
            { "end", KEY_END },
            { "pageup", KEY_PAGE_UP },
            { "pagedown", KEY_PAGE_DOWN },
            { "f11", KEY_F11 },
            { "f12", KEY_F12 },
        };

        for (const NamedKey& key : kKeys) {
            if (compat_stricmp(name.c_str(), key.name) == 0) {
                return key.code;
            }
        }

        // f1..f10 are contiguous.
        if ((name[0] == 'f' || name[0] == 'F') && name.size() >= 2 && name.size() <= 3) {
            int index = atoi(name.c_str() + 1);
            if (index >= 1 && index <= 10) {
                return KEY_F1 + index - 1;
            }
        }

        if (name.size() == 1) {
            return static_cast<unsigned char>(name[0]);
        }

        char* end = nullptr;
        long code = strtol(name.c_str(), &end, 10);
        if (end != name.c_str() && *end == '\0') {
            return static_cast<int>(code);
        }

        return -1;
    }

    std::string outputPath(const std::string& name, const char* extension)
    {
        // compat_mkdir_recursive stops at the first directory that already exists,
        // so create each level here and ignore "already exists".
        std::string path = gAutomation.outDir;
        for (size_t pos = path.find('/', 1); pos != std::string::npos; pos = path.find('/', pos + 1)) {
            compat_mkdir(path.substr(0, pos).c_str());
        }
        compat_mkdir(path.c_str());
        return gAutomation.outDir + "/" + name + extension;
    }

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

    // Stops the run with exit code 2, leaving `failure.png` for diagnosis.
    [[noreturn]] void fail(const Command& command, const char* message)
    {
        debugPrint("AUTOMATION: line %d (%s): %s\n", command.line, command.name.c_str(), message);
        fprintf(stderr, "AUTOMATION: line %d (%s): %s\n", command.line, command.name.c_str(), message);
        screenshotSavePng(outputPath("failure", ".png").c_str());
        exit(2);
    }

    // JSON helpers: output is small and only ever read by the test tools.

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

    template <typename Fn>
    void jsonIntArray(std::ostream& out, int count, Fn value)
    {
        out << '[';
        for (int index = 0; index < count; index++) {
            if (index != 0) {
                out << ',';
            }
            out << value(index);
        }
        out << ']';
    }

    void dumpCritter(std::ostream& out, Object* critter)
    {
        out << "\"pid\":" << critter->pid;
        out << ",\"name\":";
        jsonString(out, critterGetName(critter));
        out << ",\"tile\":" << critter->tile;
        out << ",\"hp\":" << critterGetHitPoints(critter);
        out << ",\"stats\":";
        jsonIntArray(out, STAT_COUNT, [&](int i) { return critterGetStat(critter, static_cast<Stat>(i)); });
        out << ",\"base_stats\":";
        jsonIntArray(out, SAVEABLE_STAT_COUNT, [&](int i) { return critterGetBaseStat(critter, static_cast<Stat>(i)); });
        out << ",\"bonus_stats\":";
        jsonIntArray(out, SAVEABLE_STAT_COUNT, [&](int i) { return critterGetBonusStat(critter, static_cast<Stat>(i)); });
        out << ",\"skills\":";
        jsonIntArray(out, SKILL_COUNT, [&](int i) { return skillGetValue(critter, static_cast<Skill>(i)); });
        out << ",\"perks\":[";
        bool first = true;
        for (int perk = 0; perk < PERK_COUNT; perk++) {
            int rank = perkGetRank(critter, static_cast<Perk>(perk));
            if (rank > 0) {
                if (!first) {
                    out << ',';
                }
                first = false;
                out << "{\"id\":" << perk << ",\"rank\":" << rank << '}';
            }
        }
        out << ']';
    }

    bool writeStateDump(const std::string& path)
    {
        std::ofstream out(path);
        if (!out.is_open()) {
            return false;
        }

        out << "{\n\"loaded\":" << (gGameLoaded ? "true" : "false");

        if (gDude != nullptr) {
            out << ",\n\"player\":{";
            dumpCritter(out, gDude);
            out << ",\"pc_stats\":";
            jsonIntArray(out, PC_STAT_COUNT, [](int i) { return pcGetStat(static_cast<PcStat>(i)); });

            Skill tagged[NUM_TAGGED_SKILLS];
            skillsGetTagged(tagged, NUM_TAGGED_SKILLS);
            out << ",\"tagged_skills\":";
            jsonIntArray(out, NUM_TAGGED_SKILLS, [&](int i) { return static_cast<int>(tagged[i]); });

            Trait trait1;
            Trait trait2;
            traitsGetSelected(&trait1, &trait2);
            out << ",\"traits\":[" << static_cast<int>(trait1) << ',' << static_cast<int>(trait2) << ']';
            out << ",\"caps\":" << itemGetTotalCaps(gDude) << '}';
        }

        out << ",\n\"party\":[";
        bool first = true;
        for (Object* member : get_all_party_members_objects(true)) {
            if (member == gDude) {
                continue;
            }
            if (!first) {
                out << ',';
            }
            first = false;
            out << "\n{";
            dumpCritter(out, member);
            int level = -1;
            int maxLevel = -1;
            partyMemberGetLevelInfo(member, &level, &maxLevel);
            out << ",\"level\":" << level << ",\"max_level\":" << maxLevel;
            out << '}';
        }
        out << ']';

        int worldX = 0;
        int worldY = 0;
        wmGetPartyWorldPos(&worldX, &worldY);
        City area = static_cast<City>(-1);
        wmGetPartyCurArea(&area);

        out << ",\n\"world\":{\"x\":" << worldX << ",\"y\":" << worldY
            << ",\"area\":" << static_cast<int>(area)
            << ",\"map\":" << static_cast<int>(mapGetCurrentMap())
            << ",\"map_name\":";
        jsonString(out, gMapHeader.name);
        out << ",\"elevation\":" << static_cast<int>(gElevation)
            << ",\"time\":" << gameTimeGetTime() << '}';

        out << ",\n\"gvars\":";
        jsonIntArray(out, gGameGlobalVars != nullptr ? gGameGlobalVarsLength : 0, [](int i) { return gGameGlobalVars[i]; });
        out << "\n}\n";

        return out.good();
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
        auto& captures = gAutomation.frameCaptures;
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
            std::ofstream meta(outputPath(it->name, ".json"));
            meta << "{\"crop\": [" << it->left << ", " << it->top << ", " << it->width << ", " << it->height << "], \"frames\": [\n";
            for (size_t index = 0; index < it->images.size(); index++) {
                char suffix[16];
                snprintf(suffix, sizeof(suffix), "_%02d", static_cast<int>(index));
                lodepng::encode(outputPath(it->name + suffix, ".png"), it->images[index], it->width, it->height, LCT_RGB, 8);
                meta << it->meta[index] << (index + 1 < it->images.size() ? ",\n" : "\n");
            }
            meta << "]}\n";
            debugPrint("AUTOMATION: wrote %d frames %s\n", static_cast<int>(it->images.size()), it->name.c_str());
            it = captures.erase(it);
        }
    }

    // Returns true when the command has finished.
    bool runCommand(const Command& command)
    {
        const std::string& name = command.name;
        const std::vector<std::string>& args = command.args;
        unsigned int now = getTicks();

        if (name == "wait") {
            if (!gAutomation.waiting) {
                if (args.empty()) {
                    fail(command, "missing duration");
                }
                gAutomation.waiting = true;
                gAutomation.waitUntil = now + static_cast<unsigned int>(atoi(args[0].c_str()));
            }
            if (now < gAutomation.waitUntil) {
                return false;
            }
            gAutomation.waiting = false;
            return true;
        }

        if (name == "until") {
            if (args.empty()) {
                fail(command, "missing state");
            }
            if (!gAutomation.waiting) {
                int timeout = args.size() > 1 ? atoi(args[1].c_str()) : kDefaultUntilTimeout;
                gAutomation.waiting = true;
                gAutomation.untilDeadline = now + static_cast<unsigned int>(timeout);
            }
            if (gAutomation.engineState == args[0]) {
                gAutomation.waiting = false;
                return true;
            }
            if (now >= gAutomation.untilDeadline) {
                fail(command, ("timed out waiting for state '" + args[0] + "', current '" + gAutomation.engineState + "'").c_str());
            }
            return false;
        }

        if (name == "key") {
            if (args.empty()) {
                fail(command, "missing key");
            }
            int code = keyCodeFromName(args[0]);
            if (code < 0) {
                fail(command, "unknown key");
            }
            // Optional repeat count: one key per input poll, so every press is handled.
            int count = args.size() > 1 ? atoi(args[1].c_str()) : 1;
            enqueueInputEvent(code);
            gAutomation.inputSent = true;
            if (++gAutomation.keyRepeatDone < count) {
                return false;
            }
            gAutomation.keyRepeatDone = 0;
            return true;
        }

        if (name == "text") {
            // One character per input poll, like typing.
            if (gAutomation.keyRepeatDone < static_cast<int>(command.rest.size())) {
                enqueueInputEvent(static_cast<unsigned char>(command.rest[gAutomation.keyRepeatDone]));
                gAutomation.keyRepeatDone++;
                gAutomation.inputSent = true;
                return false;
            }
            gAutomation.keyRepeatDone = 0;
            return true;
        }

        if (name == "move" || name == "click") {
            if (args.size() < 2) {
                fail(command, "missing coordinates");
            }
            if (gAutomation.clickPhase == 0) {
                gAutomation.mouseX = atoi(args[0].c_str());
                gAutomation.mouseY = atoi(args[1].c_str());
                if (name == "move") {
                    return true;
                }
                gAutomation.clickRight = args.size() > 2 && args[2] == "right";
            }

            gAutomation.clickPhase++;
            bool down = gAutomation.clickPhase > kClickPollsSettle && gAutomation.clickPhase <= kClickPollsSettle + kClickPollsDown;
            gAutomation.buttonLeft = down && !gAutomation.clickRight;
            gAutomation.buttonRight = down && gAutomation.clickRight;
            if (gAutomation.clickPhase > kClickPollsSettle + kClickPollsDown + kClickPollsUp) {
                gAutomation.clickPhase = 0;
                return true;
            }
            return false;
        }

        if (name == "shot") {
            if (args.empty()) {
                fail(command, "missing name");
            }
            std::string path = outputPath(args[0], ".png");
            if (!screenshotSavePng(path.c_str())) {
                fail(command, "screenshot failed");
            }
            debugPrint("AUTOMATION: wrote %s\n", path.c_str());
            return true;
        }

        if (name == "legacyshot") {
            // The 8-bit window composition (640x480 at the logical size),
            // also in HD mode: what HD drawing must never change.
            if (args.empty()) {
                fail(command, "missing name");
            }
            std::string path = outputPath(args[0], ".png");
            if (!screenshotSaveLegacyPng(path.c_str())) {
                fail(command, "screenshot failed");
            }
            return true;
        }

        if (name == "fbshot") {
            if (args.empty()) {
                fail(command, "missing name");
            }
            std::string path = outputPath(args[0], ".png");
            if (!screenshotSaveFramebufferPng(path.c_str())) {
                fail(command, "framebuffer screenshot failed");
            }
            debugPrint("AUTOMATION: wrote %s\n", path.c_str());
            return true;
        }

        if (name == "hdpack") {
            // Switches the HD pack live ("-" = none); later HD draws use it.
            if (args.empty()) {
                fail(command, "missing pack name");
            }
            hdArtSetPack(args[0] == "-" ? "" : args[0].c_str());
            if (strcmp(devAutomationGetState(), "game") == 0) {
                // Floors and roofs draw with the new pack.
                tileWindowRefresh();
            }
            return true;
        }

        if (name == "hdtext") {
            // Switches HD text live: off, sharp or smooth (text drawn later
            // uses it; screens redraw their text when they are reopened).
            HdGlyphStyle style = hdTextGetStyle();
            if (args.empty() || (args[0] != "off" && !hdGlyphStyleFromName(args[0].c_str(), &style))) {
                fail(command, "usage: hdtext off|sharp|smooth");
            }
            hdTextSetMode(args[0] != "off", style);
            return true;
        }

        if (name == "hdtextstats") {
            // HD text counters (strings drawn at 2x, remembered strings
            // drawn at a blit, glyphs made, time spent) to <name>.json.
            if (args.empty()) {
                fail(command, "missing name");
            }
            HdTextStats stats;
            hdTextGetStats(&stats);
            std::ofstream out(outputPath(args[0], ".json"));
            out << "{\"active\": " << (hdTextIsActive() ? "true" : "false")
                << ", \"style\": ";
            jsonString(out, hdGlyphStyleName(hdTextGetStyle()));
            out << ", \"draws\": " << stats.draws
                << ", \"replays\": " << stats.replays
                << ", \"glyphs_cached\": " << stats.glyphsCached
                << ", \"total_ms\": " << stats.totalMs << "}\n";
            return true;
        }

        if (name == "floattext") {
            // Floating text over the player (as the float_msg script
            // function shows it), white with a black outline.
            if (args.empty() || strcmp(devAutomationGetState(), "game") != 0 || gDude == nullptr) {
                fail(command, "usage: floattext <text> (in a game)");
            }
            std::string text = command.rest;
            Rect rect;
            if (textObjectAdd(gDude, &text[0], 101, COLOR_WHITE, COLOR_BLACK, &rect) == 0) {
                tileWindowRefreshRect(&rect, gElevation);
            }
            return true;
        }

        if (name == "loot" || name == "talk") {
            // Opens the loot window on a party member the way the steal
            // skill does (companions let the player take and give freely),
            // or starts a dialogue with one; the game loop handles the
            // request (party index 1 = the first companion).
            int index = args.empty() ? 1 : atoi(args[0].c_str());
            if (strcmp(devAutomationGetState(), "game") != 0 || gDude == nullptr) {
                fail(command, "usage: loot|talk [<party index>] (in a game)");
            }
            std::vector<Object*> party = get_all_party_members_objects(false);
            if (index < 0 || index >= static_cast<int>(party.size())) {
                fail(command, "no such party member");
            }
            if (name == "loot") {
                scriptsRequestStealing(gDude, party[index]);
            } else {
                scriptsRequestDialog(party[index]);
            }
            return true;
        }

        if (name == "monitor") {
            // A line in the interface bar's message monitor.
            displayMonitorAddMessage(command.rest.c_str());
            return true;
        }

        if (name == "hdprobe") {
            // Looks up the HD image for frame 0 of an art file (with the 2x
            // legacy fallback) and writes it as <name>.png plus <name>.json.
            if (args.size() < 2) {
                fail(command, "usage: hdprobe <art path> <name>");
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
                lodepng::encode(outputPath(args[1], ".png"), rgba, image->width, image->height, LCT_RGBA, 8);
            }

            std::ofstream out(outputPath(args[1], ".json"));
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

        if (name == "hdlights") {
            // `hdlights off|colour|glow|shadows|all|<mask>` switches HD lights
            // live; `hdlights dump <name>` writes the emitters to <name>.json.
            if (args.empty()) {
                fail(command, "usage: hdlights off|colour|glow|shadows|all|<mask> | dump <name>");
            }
            if (args[0] == "field") {
                // The applied field at light-cell resolution: <name>_mul.png
                // (extra light, 1.0 = 255), <name>_add.png (glow) and
                // <name>_shade.png (shadow cells, darkness as grey).
                if (args.size() < 2) {
                    fail(command, "usage: hdlights field <name>");
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
                        lodepng::encode(outputPath(args[1] + suffix[kind], ".png"), rgb, width, height, LCT_RGB, 8);
                    }
                }
                return true;
            }
            if (args[0] == "dump") {
                if (args.size() < 2) {
                    fail(command, "usage: hdlights dump <name>");
                }
                std::ofstream out(outputPath(args[1], ".json"));
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

        if (name == "walk") {
            // `walk <rotation 0..5> <hexes> [run]`: the player walks (or
            // runs) that far in a straight line, as a click would make him.
            if (args.size() < 2 || strcmp(devAutomationGetState(), "game") != 0 || gDude == nullptr) {
                fail(command, "usage: walk <rotation> <hexes> [run] (in a game)");
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

        if (name == "scrollhold") {
            // `scrollhold <dx> <dy> <ms>`: scroll the map as a held arrow key
            // or the mouse at the screen edge would (a step whenever the
            // scroll interval allows); later commands run meanwhile.
            if (args.size() < 3) {
                fail(command, "usage: scrollhold <dx> <dy> <ms>");
            }
            gAutomation.scrolling = true;
            gAutomation.scrollDx = atoi(args[0].c_str());
            gAutomation.scrollDy = atoi(args[1].c_str());
            gAutomation.scrollUntil = now + static_cast<unsigned int>(atoi(args[2].c_str()));
            return true;
        }

        if (name == "frames") {
            // `frames <name> <count> [<x> <y> <w> <h> [<every>]]`: the next
            // <count> presented frames (every <every>th; a crop in output
            // pixels, default all) to
            // <name>_NN.png, with the view and player per frame in
            // <name>.json; later commands run meanwhile.
            if (args.size() < 2) {
                fail(command, "usage: frames <name> <count> [<x> <y> <w> <h>]");
            }
            AutomationState::FrameCapture capture;
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
            gAutomation.frameCaptures.push_back(std::move(capture));
            return true;
        }

        if (name == "framesdone") {
            // Waits until every `frames` capture has been written.
            return gAutomation.frameCaptures.empty();
        }

        if (name == "zoom") {
            // `zoom <level> [now]` (map view, [graphics] zoom=1) or
            // `zoom in|out|reset` as the keys do.
            if (args.empty()) {
                fail(command, "usage: zoom <level> [now] | in | out | reset");
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

        if (name == "viewsettle") {
            // Waits until the map view stops moving (pan and zoom), at most
            // 5 s.
            if (!gAutomation.waiting) {
                gAutomation.waiting = true;
                gAutomation.untilDeadline = now + 5000;
            }
            if (mapViewIsMoving()) {
                if (now >= gAutomation.untilDeadline) {
                    std::string view = describeView();
                    fail(command, ("the map view keeps moving: " + view).c_str());
                }
                return false;
            }
            gAutomation.waiting = false;
            return true;
        }

        if (name == "viewdump") {
            if (args.empty()) {
                fail(command, "missing name");
            }
            std::ofstream out(outputPath(args[0], ".json"));
            out << describeView() << "\n";
            return true;
        }

        if (name == "mousetoparty") {
            // `mousetoparty <n>`: the cursor to the middle of party member
            // n's sprite as shown (through pan and zoom).
            std::vector<Object*> party = get_all_party_members_objects(false);
            int index = args.empty() ? 1 : atoi(args[0].c_str());
            float x;
            float y;
            if (index < 0 || index >= static_cast<int>(party.size()) || !objectScreenCentre(party[index], &x, &y)) {
                fail(command, "no such party member on the map");
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
            gAutomation.mouseX = static_cast<int>(x);
            gAutomation.mouseY = static_cast<int>(y);
            return true;
        }

        if (name == "mousetotile") {
            // `mousetotile <tile|dude>`: the cursor to the centre of a hex as
            // shown (through pan and zoom).
            if (args.empty() || gDude == nullptr) {
                fail(command, "usage: mousetotile <tile|dude>");
            }
            int tile = args[0] == "dude" ? gDude->tile : atoi(args[0].c_str());
            int hexX;
            int hexY;
            if (tileToScreenXY(tile, &hexX, &hexY) != 0) {
                fail(command, "invalid tile");
            }
            float x = hexX + 16.0f;
            float y = hexY + 8.0f;
            mapViewSurfaceToScreen(&x, &y);
            gAutomation.mouseX = static_cast<int>(x);
            gAutomation.mouseY = static_cast<int>(y);
            return true;
        }

        if (name == "pick") {
            // `pick <name>`: what the game finds under the cursor (the
            // object the player would click, the hex under the cursor) to
            // <name>.json.
            if (args.empty() || strcmp(devAutomationGetState(), "game") != 0) {
                fail(command, "usage: pick <name> (in a game)");
            }
            int x;
            int y;
            mouseGetPosition(&x, &y);
            int mapX = x;
            int mapY = y;
            mapViewScreenToSurface(&mapX, &mapY);
            Object* object = gameMouseGetObjectUnderCursor(OBJ_TYPE_INVALID, true, gElevation);
            Object* critter = gameMouseGetObjectUnderCursor(OBJ_TYPE_CRITTER, true, gElevation);
            std::ofstream out(outputPath(args[0], ".json"));
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

        if (name == "refresh") {
            // Redraws the whole map window (timed by `perf`).
            if (strcmp(devAutomationGetState(), "game") == 0) {
                tileWindowRefresh();
            }
            return true;
        }

        if (name == "benchrefresh") {
            // Redraws the whole map window <count> times in a row and writes
            // the min/median/max time per redraw (and of that, the HD tile
            // drawing) to <name>.json.
            if (args.size() < 2) {
                fail(command, "usage: benchrefresh <name> <count>");
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
            std::ofstream out(outputPath(args[0], ".json"));
            out << "{\"count\": " << totals.size() << ", \"refresh_ms\": " << summary(totals) << ", \"hd_tiles_ms\": " << summary(tiles) << ", \"hd_objects_ms\": " << summary(objects) << "}\n";
            return true;
        }

        if (name == "hdcheck") {
            // Phase 2.7 self-check of the iso window's HD floors and roofs
            // (see tileHdCheck): <name>.json with the counts, <name>.png
            // with the per-pixel classes (black legacy, grey HD tile, blue
            // legacy object, red HD on a critter or other legacy object,
            // green HD wall/scenery/item, yellow stale, cyan legacy pixel
            // differs from a fresh redraw).
            if (args.empty()) {
                fail(command, "missing name");
            }
            TileHdCheck check;
            unsigned int cutOutsBefore = objectHdCutOutDraws();
            bool ok = tileHdCheck(&check);
            // (tileHdCheck redraws the map three times.)
            unsigned int cutOuts = (objectHdCutOutDraws() - cutOutsBefore) / 3;
            std::ofstream out(outputPath(args[0], ".json"));
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
                lodepng::encode(outputPath(args[0], ".png"), rgb, width, height, LCT_RGB, 8);
            }
            return true;
        }

        if (name == "hdstats") {
            if (args.empty()) {
                fail(command, "missing name");
            }
            HdArtStats stats;
            hdArtGetStats(&stats);
            std::ofstream out(outputPath(args[0], ".json"));
            out << "{\"assets_resolved\": " << stats.assetsResolved
                << ", \"assets_with_hd\": " << stats.assetsWithHd
                << ", \"frames_cached\": " << stats.framesCached
                << ", \"bytes_cached\": " << stats.bytesCached
                << ", \"hits\": " << stats.hits
                << ", \"misses\": " << stats.misses << "}\n";
            return true;
        }

        if (name == "fadeshot") {
            if (args.size() < 2) {
                fail(command, "usage: fadeshot <name> <percent> [in|out]");
            }
            AutomationState::FadeCapture capture;
            capture.name = args[0];
            capture.percent = atoi(args[1].c_str());
            capture.direction = 0;
            if (args.size() > 2) {
                capture.direction = args[2] == "in" ? 1 : (args[2] == "out" ? -1 : 0);
            }
            gAutomation.fadeCaptures.push_back(capture);
            return true;
        }

        if (name == "dump") {
            if (args.empty()) {
                fail(command, "missing name");
            }
            std::string path = outputPath(args[0], ".json");
            if (!writeStateDump(path)) {
                fail(command, "state dump failed");
            }
            debugPrint("AUTOMATION: wrote %s\n", path.c_str());
            return true;
        }

        if (name == "perf") {
            // `perf reset` starts a measurement; `perf <name>` writes the
            // presenter counters gathered since then and starts a new one.
            if (args.empty()) {
                fail(command, "missing name");
            }
            if (args[0] != "reset") {
                PresenterStats stats;
                presenterGetStats(&stats);
                std::string path = outputPath(args[0], ".json");
                FILE* stream = fopen(path.c_str(), "wt");
                if (stream == nullptr) {
                    fail(command, "cannot write perf file");
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
                    tileRenderClockMs() - gAutomation.perfStartMs, render.refreshes, render.refreshMs, render.refreshMaxMs,
                    render.scrolls, render.scrollMs, render.scrollMaxMs, render.hdTilesMs, render.hdObjectsMs,
                    static_cast<unsigned long long>(stats.uploadedPixels), stats.lightMs, static_cast<unsigned long long>(stats.lightUpdates),
                    lights.updateMs, lights.updateMaxMs, lights.rebuilds, lights.rebuildMs);
                fclose(stream);
                MapViewStats view;
                mapViewGetStats(&view);
                std::ofstream viewOut(outputPath(args[0] + "_view", ".json"));
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
            gAutomation.perfStartMs = tileRenderClockMs();
            return true;
        }

        if (name == "log") {
            debugPrint("AUTOMATION: %s\n", command.rest.c_str());
            return true;
        }

        if (name == "quit") {
            int code = args.empty() ? 0 : atoi(args[0].c_str());
            debugPrint("AUTOMATION: quit %d\n", code);
            exit(code);
        }

        fail(command, "unknown command");
    }

} // namespace

static bool gAutomationUnattended = false;
static bool gAutomationFastFades = false;

void devAutomationEarlyInit(int argc, char** argv)
{
    for (int index = 1; index < argc; index++) {
        if (strncmp(argv[index], "--dev-script=", 13) == 0) {
            gAutomationUnattended = true;
            break;
        }
    }

    if (!gAutomationUnattended) {
        return;
    }

    SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);

    // The window never receives focus, so do not wait for it.
    gProgramIsActive = true;
}

bool devAutomationWantsHiddenWindow()
{
    return gAutomationUnattended;
}

void devAutomationParseArgs(int argc, char** argv)
{
    const char* scriptPrefix = "--dev-script=";
    const char* outPrefix = "--dev-out=";

    const char* scriptPath = nullptr;
    for (int index = 1; index < argc; index++) {
        if (strncmp(argv[index], scriptPrefix, strlen(scriptPrefix)) == 0) {
            scriptPath = argv[index] + strlen(scriptPrefix);
        } else if (strcmp(argv[index], "--dev-fast") == 0) {
            gAutomationFastFades = true;
        } else if (strncmp(argv[index], outPrefix, strlen(outPrefix)) == 0) {
            gAutomation.outDir = argv[index] + strlen(outPrefix);
        }
    }

    if (scriptPath == nullptr) {
        return;
    }

    if (!loadScript(scriptPath)) {
        fprintf(stderr, "AUTOMATION: cannot open script %s\n", scriptPath);
        exit(2);
    }

    gAutomation.active = true;
    gAutomation.mouseX = screenGetWidth() / 2;
    gAutomation.mouseY = screenGetHeight() / 2;
}

bool devAutomationFastFades()
{
    return gAutomationFastFades;
}

bool devAutomationIsActive()
{
    return gAutomation.active;
}

void devAutomationTick()
{
    if (!gAutomation.active) {
        return;
    }

    if (gAutomation.scrolling) {
        if (getTicks() >= gAutomation.scrollUntil || strcmp(devAutomationGetState(), "game") != 0) {
            gAutomation.scrolling = false;
        } else {
            mapScroll(gAutomation.scrollDx, gAutomation.scrollDy);
        }
    }

    // Run every command that completes immediately, stop at the first one that waits.
    gAutomation.inputSent = false;
    while (gAutomation.next < gAutomation.commands.size()) {
        if (!runCommand(gAutomation.commands[gAutomation.next])) {
            return;
        }
        gAutomation.next++;
        if (gAutomation.inputSent) {
            return;
        }
    }
}

void devAutomationOnFadeStep(int step, int steps, bool toBlack)
{
    if (!gAutomation.active || gAutomation.fadeCaptures.empty() || steps <= 0) {
        return;
    }

    auto& captures = gAutomation.fadeCaptures;
    for (auto it = captures.begin(); it != captures.end();) {
        bool directionMatches = it->direction == 0 || (it->direction == 1 && !toBlack) || (it->direction == -1 && toBlack);
        if (directionMatches && step * 100 >= it->percent * steps) {
            std::string path = outputPath(it->name, ".png");
            screenshotSaveFramebufferPng(path.c_str());
            writeLegacyPalettePng(outputPath(it->name + "_6bit", ".png"));
            debugPrint("AUTOMATION: wrote %s (fade step %d/%d)\n", path.c_str(), step, steps);
            it = captures.erase(it);
        } else {
            ++it;
        }
    }
}

void devAutomationOnPresent()
{
    if (gAutomation.active && !gAutomation.frameCaptures.empty()) {
        captureFrames();
    }
}

bool devAutomationOverrideMouse(MouseData* mouseData)
{
    if (!gAutomation.active) {
        return false;
    }

    if (mouseDeviceUsesRelativeMode()) {
        int x;
        int y;
        mouseGetPosition(&x, &y);
        mouseData->x = gAutomation.mouseX - x;
        mouseData->y = gAutomation.mouseY - y;
    } else {
        mouseData->x = gAutomation.mouseX;
        mouseData->y = gAutomation.mouseY;
    }
    mouseData->buttons[0] = gAutomation.buttonLeft ? 1 : 0;
    mouseData->buttons[1] = gAutomation.buttonRight ? 1 : 0;
    mouseData->wheelX = 0;
    mouseData->wheelY = 0;
    return true;
}

const char* devAutomationGetState()
{
    return gAutomation.engineState.c_str();
}

void devAutomationSetState(const char* state)
{
    if (gAutomation.engineState != state) {
        gAutomation.engineState = state;
        if (gAutomation.active) {
            debugPrint("AUTOMATION: state '%s'\n", state);
        }
    }
}

} // namespace fallout
