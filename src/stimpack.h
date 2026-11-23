#ifndef FALLOUT_STIMPACK_H_
#define FALLOUT_STIMPACK_H_

namespace fallout {

// Stimpack: savegame editor opened from the main menu.

// Message ids in `text/<language>/game/stimpack.msg`.
enum StimpackMessage : int {
    STIMPACK_MSG_MENU_BUTTON = 100,
    STIMPACK_MSG_CHARACTER = 101,
    STIMPACK_MSG_COMPANIONS = 102,
    STIMPACK_MSG_ROSTER = 103,
    STIMPACK_MSG_QUESTS = 104,
    STIMPACK_MSG_LOCATION = 105,
    STIMPACK_MSG_SAVE_AS = 106,
    STIMPACK_MSG_EXIT = 107,
    STIMPACK_MSG_COMBAT_SAVE = 110,
    STIMPACK_MSG_DISCARD_CHANGES = 111,
    STIMPACK_MSG_PERKS_BUTTON = 120,
    STIMPACK_MSG_EDIT_LEVEL = 121,
    STIMPACK_MSG_EDIT_EXPERIENCE = 122,
    STIMPACK_MSG_EDIT_HIT_POINTS = 123,
    STIMPACK_MSG_EDIT_SKILL_POINTS = 124,
    STIMPACK_MSG_PERK_EDITOR_TITLE = 130,
    STIMPACK_MSG_PERK_EDITOR_RANK = 131,
};

// Returns the text of a message from stimpack.msg, or `fallback` when the file or
// the entry is missing.
const char* stimpackGetMessage(int messageId, const char* fallback);

// Runs the editor on the game that was just loaded from the main menu (map live,
// game loop not running). Returns true when the game was saved. The caller tears
// the game down afterwards exactly as after leaving a game.
bool stimpackRun();

} // namespace fallout

#endif /* FALLOUT_STIMPACK_H_ */
