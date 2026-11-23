#include "stimpack.h"

#include <string.h>

#include <string>
#include <vector>

#include "art.h"
#include "character_editor.h"
#include "color.h"
#include "combat.h"
#include "critter.h"
#include "dbox.h"
#include "debug.h"
#include "dev_automation.h"
#include "draw.h"
#include "game_mouse.h"
#include "game_sound.h"
#include "graph_lib.h"
#include "input.h"
#include "interface.h"
#include "kb.h"
#include "loadsave.h"
#include "map.h"
#include "memory.h"
#include "message.h"
#include "mouse.h"
#include "object.h"
#include "party_member.h"
#include "proto.h"
#include "scripts.h"
#include "svga.h"
#include "text_font.h"
#include "tile.h"
#include "window_manager.h"

namespace fallout {

namespace {

    // Hub entries, top to bottom. Event codes are 600 + entry.
    enum HubEntry : int {
        HUB_ENTRY_CHARACTER,
        HUB_ENTRY_COMPANIONS,
        HUB_ENTRY_ROSTER,
        HUB_ENTRY_QUESTS,
        HUB_ENTRY_LOCATION,
        HUB_ENTRY_SAVE_AS,
        HUB_ENTRY_EXIT,
        HUB_ENTRY_COUNT,
    };

    constexpr int kHubEventBase = 600;

    struct HubEntrySpec {
        int messageId;
        const char* fallback;
        int hotkeyLower;
        bool implemented;
    };

    constexpr HubEntrySpec kHubEntries[HUB_ENTRY_COUNT] = {
        { STIMPACK_MSG_CHARACTER, "CHARACTER", KEY_LOWERCASE_C, true },
        { STIMPACK_MSG_COMPANIONS, "COMPANIONS", KEY_LOWERCASE_O, true },
        { STIMPACK_MSG_ROSTER, "ROSTER", KEY_LOWERCASE_R, false },
        { STIMPACK_MSG_QUESTS, "QUESTS", KEY_LOWERCASE_Q, false },
        { STIMPACK_MSG_LOCATION, "LOCATION", KEY_LOWERCASE_L, false },
        { STIMPACK_MSG_SAVE_AS, "SAVE AS", KEY_LOWERCASE_S, true },
        { STIMPACK_MSG_EXIT, "EXIT", KEY_LOWERCASE_E, true },
    };

    // Panel art: opbase_sp.png (options panel with eight slots, made by
    // tools/dev/menu_art.py). Slot 0 is the header, slots 1..7 the entries.
    constexpr int kPanelFirstSlotY = 17;
    constexpr int kPanelSlotPitch = 37;
    constexpr int kPanelSlotX = 13;
    constexpr int kHeaderFont = 101;
    constexpr int kButtonFont = 103;

    MessageList gStimpackMessageList;
    bool gStimpackMessageListLoaded = false;

    constexpr int kPanelMaxEntries = 7;

    // One plate of a panel: a button, or a dimmed plate when not enabled.
    struct PanelEntry {
        std::string label;
        bool enabled;
    };

    // Options-style panel: header slot with two text lines, then up to seven
    // plate buttons sending eventBase + index. Used by the hub and its pickers.
    struct Hub {
        int window = -1;
        unsigned char* buffer = nullptr;
        FrmImage background;
        FrmImage buttonUp;
        FrmImage buttonDown;
        // Normal and pressed images for each entry, with the label drawn in.
        unsigned char* images[kPanelMaxEntries][2] = {};
        int oldFont = 0;
        bool cursorWasHidden = false;
        bool mouseObjectsWereVisible = false;
    };

    void hubFree(Hub& hub)
    {
        if (hub.window != -1) {
            windowDestroy(hub.window);
            hub.window = -1;
        }
        for (auto& pair : hub.images) {
            for (unsigned char*& image : pair) {
                if (image != nullptr) {
                    internal_free(image);
                    image = nullptr;
                }
            }
        }
        hub.background.unlock();
        hub.buttonUp.unlock();
        hub.buttonDown.unlock();
        fontSetCurrent(hub.oldFont);

        if (hub.mouseObjectsWereVisible) {
            gameMouseObjectsShow();
        }
        if (hub.cursorWasHidden) {
            mouseHideCursor();
        }
    }

    // Header slot: two short lines (for the hub: character name and location).
    void hubDrawHeader(Hub& hub, const char* line1, const char* line2)
    {
        int width = hub.background.getWidth();
        fontSetCurrent(kHeaderFont);
        int lineHeight = fontGetLineHeight();
        int top = kPanelFirstSlotY + (kPanelSlotPitch - 3 - 2 * lineHeight) / 2;

        const char* lines[2] = { line1, line2 };
        for (int index = 0; index < 2; index++) {
            const char* text = lines[index] != nullptr ? lines[index] : "";
            int textWidth = fontGetStringWidth(text);
            int x = std::max(0, (width - textWidth) / 2);
            fontDrawText(hub.buffer + width * (top + index * lineHeight) + x, text, width - x, width, COLOR_GREEN);
        }
    }

    bool hubInit(Hub& hub, const char* header1, const char* header2, const std::vector<PanelEntry>& entries, int eventBase)
    {
        hub.oldFont = fontGetCurrent();

        if (!hub.background.lock(InterfaceFrmId("opbase_sp.png"))
            || !hub.buttonUp.lock(InterfaceFrameId::OptionsButtonOff)
            || !hub.buttonDown.lock(InterfaceFrameId::OptionsButtonOn)) {
            debugPrint("STIMPACK: missing panel art\n");
            return false;
        }

        int buttonWidth = hub.buttonUp.getWidth();
        int buttonHeight = hub.buttonUp.getHeight();
        size_t buttonSize = static_cast<size_t>(buttonWidth) * buttonHeight;

        // Panels with fewer entries drop the unused slots: the art's top part
        // down to the last used slot, then its bottom border.
        int entryCount = std::min(static_cast<int>(entries.size()), kPanelMaxEntries);
        int panelWidth = hub.background.getWidth();
        int artHeight = hub.background.getHeight();
        int keptTop = kPanelFirstSlotY + (entryCount + 1) * kPanelSlotPitch;
        int border = artHeight - (kPanelFirstSlotY + (kPanelMaxEntries + 1) * kPanelSlotPitch);
        int panelHeight = std::min(artHeight, keptTop + border);
        int x = (screenGetWidth() - panelWidth) / 2;
        int y = (screenGetHeight() - panelHeight) / 2;
        hub.window = windowCreate(x, y, panelWidth, panelHeight, static_cast<ColorWithFlags>(256), WINDOW_MODAL | WINDOW_DONT_MOVE_TOP);
        if (hub.window == -1) {
            return false;
        }

        hub.buffer = windowGetBuffer(hub.window);
        memcpy(hub.buffer, hub.background.getData(), static_cast<size_t>(panelWidth) * (panelHeight - border));
        memcpy(hub.buffer + static_cast<size_t>(panelWidth) * (panelHeight - border),
            hub.background.getData() + static_cast<size_t>(panelWidth) * (artHeight - border),
            static_cast<size_t>(panelWidth) * border);
        hubDrawHeader(hub, header1, header2);

        fontSetCurrent(kButtonFont);
        for (int entry = 0; entry < entryCount; entry++) {
            const char* label = entries[entry].label.c_str();
            int textX = std::max(0, (buttonWidth - fontGetStringWidth(label)) / 2);
            int textY = (buttonHeight - fontGetLineHeight()) / 2 + 1;
            int slotY = kPanelFirstSlotY + (entry + 1) * kPanelSlotPitch;

            if (!entries[entry].enabled) {
                // Not available yet: draw the plate with a dim label and no button.
                blitBufferToBufferTrans(hub.buttonUp.getData(), buttonWidth, buttonHeight, buttonWidth,
                    hub.buffer + panelWidth * slotY + kPanelSlotX, panelWidth);
                fontDrawText(hub.buffer + panelWidth * (slotY + textY) + kPanelSlotX + textX, label, buttonWidth, panelWidth, COLOR_DARK_GREY_2);
                continue;
            }

            for (int state = 0; state < 2; state++) {
                hub.images[entry][state] = static_cast<unsigned char*>(internal_malloc(buttonSize));
                if (hub.images[entry][state] == nullptr) {
                    return false;
                }
                memcpy(hub.images[entry][state], (state == 0 ? hub.buttonUp : hub.buttonDown).getData(), buttonSize);
                fontDrawText(hub.images[entry][state] + buttonWidth * (textY + state) + textX,
                    label,
                    buttonWidth,
                    buttonWidth,
                    state == 0 ? COLOR_DARK_YELLOW : COLOR_DARK_YELLOW_2);
            }

            int button = buttonCreate(hub.window,
                kPanelSlotX,
                slotY,
                buttonWidth,
                buttonHeight,
                -1,
                -1,
                -1,
                eventBase + entry,
                hub.images[entry][0],
                hub.images[entry][1],
                nullptr,
                BUTTON_FLAG_TRANSPARENT);
            if (button != -1) {
                buttonSetCallbacks(button, _gsound_lrg_butt_press, _gsound_lrg_butt_release);
            }
        }

        hub.mouseObjectsWereVisible = gameMouseObjectsIsVisible();
        if (hub.mouseObjectsWereVisible) {
            gameMouseObjectsHide();
        }
        gameMouseSetCursor(MOUSE_CURSOR_ARROW);
        hub.cursorWasHidden = cursorIsHidden();
        if (hub.cursorWasHidden) {
            mouseShowCursor();
        }

        windowRefresh(hub.window);
        return true;
    }

    // Shows the loaded map in greyscale behind the hub, without the interface bar.
    void shadeMapView()
    {
        // Stop object animations so nothing redraws in colour over the shading.
        isoDisable();
        interfaceBarHide();
        tileWindowRefresh();
        grayscalePaletteApply(windowGetBuffer(gIsoWindow), windowGetWidth(gIsoWindow), windowGetHeight(gIsoWindow), windowGetWidth(gIsoWindow));
        windowRefresh(gIsoWindow);
    }

    void showMessage(int messageId, const char* fallback)
    {
        const char* body[] = { stimpackGetMessage(messageId, fallback) };
        showDialogBox(nullptr, body, 1, 169, 116, COLOR_AMBER, nullptr, COLOR_AMBER, DIALOG_BOX_LARGE);
    }

    bool confirm(int messageId, const char* fallback)
    {
        const char* body[] = { stimpackGetMessage(messageId, fallback) };
        return showDialogBox(nullptr, body, 1, 169, 116, COLOR_AMBER, nullptr, COLOR_AMBER, DIALOG_BOX_LARGE | DIALOG_BOX_YES_NO) != 0;
    }

    // Returns the selected entry, or -1 when nothing was chosen this frame.
    int hubEntryForKey(int keyCode)
    {
        if (keyCode >= kHubEventBase && keyCode < kHubEventBase + HUB_ENTRY_COUNT) {
            return keyCode - kHubEventBase;
        }
        if (keyCode == KEY_ESCAPE) {
            return HUB_ENTRY_EXIT;
        }
        for (int entry = 0; entry < HUB_ENTRY_COUNT; entry++) {
            int lower = kHubEntries[entry].hotkeyLower;
            if (kHubEntries[entry].implemented && (keyCode == lower || keyCode == lower - ('a' - 'A'))) {
                return entry;
            }
        }
        return -1;
    }

    constexpr int kPickerEventBase = 640;

    // Companions in the party (alive, on the map), at most six so BACK fits.
    std::vector<Object*> stimpackCompanions()
    {
        std::vector<Object*> companions;
        for (Object* member : get_all_party_members_objects(false)) {
            if (member != gDude && ProtoId(member).objectType() == OBJ_TYPE_CRITTER && companions.size() < kPanelMaxEntries - 1) {
                companions.push_back(member);
            }
        }
        return companions;
    }

    // COMPANIONS: pick a party member, edit it, come back to the list.
    // Returns true when anything was changed.
    bool runCompanions()
    {
        std::vector<Object*> companions = stimpackCompanions();
        if (companions.empty()) {
            showMessage(STIMPACK_MSG_NO_COMPANIONS, "There are no companions in this game.");
            return false;
        }

        bool edited = false;
        for (;;) {
            std::vector<PanelEntry> entries;
            for (Object* companion : companions) {
                entries.push_back({ critterGetName(companion), true });
            }
            entries.push_back({ stimpackGetMessage(STIMPACK_MSG_BACK, "BACK"), true });
            int backIndex = static_cast<int>(entries.size()) - 1;

            Hub picker;
            if (!hubInit(picker, stimpackGetMessage(STIMPACK_MSG_COMPANIONS, "COMPANIONS"), critterGetName(gDude), entries, kPickerEventBase)) {
                hubFree(picker);
                return edited;
            }

            int chosen = -1;
            while (chosen == -1) {
                sharedFpsLimiter.mark();
                int keyCode = inputGetInput();
                if (keyCode >= kPickerEventBase && keyCode < kPickerEventBase + static_cast<int>(entries.size())) {
                    chosen = keyCode - kPickerEventBase;
                } else if (keyCode == KEY_ESCAPE) {
                    chosen = backIndex;
                } else if (keyCode >= '1' && keyCode < '1' + backIndex) {
                    chosen = keyCode - '1';
                }
                renderPresent();
                sharedFpsLimiter.throttle();
            }
            hubFree(picker);
            windowRefreshAll(&_scr_size);

            if (chosen == backIndex) {
                return edited;
            }
            if (characterEditorShowStimpackCompanion(companions[chosen]) == 0) {
                edited = true;
            }
            windowRefreshAll(&_scr_size);
        }
    }

} // namespace

const char* stimpackGetMessage(int messageId, const char* fallback)
{
    if (!gStimpackMessageListLoaded) {
        gStimpackMessageListLoaded = true;
        messageListInit(&gStimpackMessageList);
        messageListLoad(&gStimpackMessageList, "game\\stimpack.msg");
    }

    MessageListItem item;
    item.num = messageId;
    if (messageListGetItem(&gStimpackMessageList, &item)) {
        return item.text;
    }

    return fallback;
}

bool stimpackRun()
{
    // A game saved mid-combat restores a half-finished combat state on load;
    // saving it again without running combat would persist that state.
    if (isInCombat()) {
        showMessage(STIMPACK_MSG_COMBAT_SAVE, "This game was saved during combat and cannot be edited.");
        return false;
    }

    // The game loop never runs while editing: keep scripts from reacting.
    scriptsDisable();
    shadeMapView();

    std::vector<PanelEntry> entries;
    for (const HubEntrySpec& spec : kHubEntries) {
        entries.push_back({ stimpackGetMessage(spec.messageId, spec.fallback), spec.implemented });
    }

    Hub hub;
    if (!hubInit(hub, critterGetName(gDude), mapGetCityName(mapGetCurrentMap()), entries, kHubEventBase)) {
        hubFree(hub);
        return false;
    }

    devAutomationSetState("stimpack");

    bool saved = false;
    bool edited = false;
    bool done = false;
    while (!done) {
        sharedFpsLimiter.mark();

        int keyCode = inputGetInput();
        switch (hubEntryForKey(keyCode)) {
        case HUB_ENTRY_CHARACTER:
            // Sub-screens open above the hub; hide it so it does not show around them.
            windowHide(hub.window);
            if (characterEditorShowStimpack() == 0) {
                edited = true;
            }
            windowShow(hub.window);
            windowRefreshAll(&_scr_size);
            break;
        case HUB_ENTRY_COMPANIONS:
            windowHide(hub.window);
            if (runCompanions()) {
                edited = true;
            }
            windowShow(hub.window);
            windowRefreshAll(&_scr_size);
            break;
        case HUB_ENTRY_SAVE_AS:
            lsgRequestEmptySlotOnNextSave();
            windowHide(hub.window);
            if (lsgSaveGame(LoadSaveMode::Normal) == 1) {
                saved = true;
                done = true;
            } else {
                windowShow(hub.window);
                windowRefreshAll(&_scr_size);
            }
            break;
        case HUB_ENTRY_EXIT:
            if (!edited || confirm(STIMPACK_MSG_DISCARD_CHANGES, "Leave without saving your changes?")) {
                done = true;
            }
            windowRefreshAll(&_scr_size);
            break;
        default:
            break;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    devAutomationSetState("");
    hubFree(hub);
    return saved;
}

} // namespace fallout
