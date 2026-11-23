#ifndef FALLOUT_DEV_AUTOMATION_H_
#define FALLOUT_DEV_AUTOMATION_H_

#include "dinput.h"

namespace fallout {

// Scripted input driver for automated testing and screenshots.
//
// Enabled with `--dev-script=<file>`; output (screenshots, state dumps) goes to
// `--dev-out=<dir>` (default `devout`, relative to the working directory).
//
// Script format: one command per line, `#` starts a comment.
//   wait <ms>                 pause
//   until <state> [<ms>]      wait until the engine reports a state (mainmenu,
//                             game, stimpack, input = a text input dialog is open),
//                             fail after timeout (default 30000)
//   key <name|char|code> [n]  send a key (n times, one per input poll) (esc, enter, space, tab, backspace,
//                             up, down, left, right, home, end, pageup, pagedown, f1..f12)
//   text <string>             type the rest of the line
//   move <x> <y>              move the cursor (logical screen coordinates)
//   click <x> <y> [right]     move and click
//   shot <name>               write <out>/<name>.png
//   dump <name>               write <out>/<name>.json with player, party, quest and world state
//   fbshot <name>             write the presented framebuffer (output resolution)
//   legacyshot <name>         write the 8-bit window composition (logical
//                             resolution) through the presented palette, also
//                             in HD mode (where `shot` equals `fbshot`)
//   fadeshot <name> <pct> [in|out]
//                             during the next palette fade (in: to a non-black
//                             palette, out: to black) write the framebuffer at
//                             <pct>% of the fade, plus <name>_6bit.png rendered
//                             through the 6-bit palette as the old presenter did
//   hdpack <pack|->           switch the HD art pack live (- = none)
//   hdprobe <art path> <name> look up the HD image of frame 0 (2x legacy
//                             fallback); write <name>.png and <name>.json
//   hdstats <name>            write HD art cache counters to <name>.json
//   hdtext off|sharp|smooth   switch HD text live (text drawn later uses it)
//   hdtextstats <name>        write HD text counters to <name>.json
//   floattext <text>          floating text over the player (in a game)
//   monitor <text>            a line in the interface bar's message monitor
//   loot [<n>]                open the loot window on party member n
//                             (default 1, the first companion; 0 = player)
//   talk [<n>]                start a dialogue with party member n
//   hdlights off|colour|glow|shadows|all|<mask>
//                             switch HD lights live (glow = colour + glow)
//   hdlights dump <name>      write the HD light emitters to <name>.json
//   refresh                   redraw the whole map window (in a game)
//   benchrefresh <name> <n>   redraw it n times in a row; min/median/max ms
//                             per redraw (and HD tile drawing) to <name>.json
//   hdcheck <name>            self-check of the map's HD layer (floors, roofs,
//                             walls, scenery, items; see tileHdCheck):
//                             <name>.json counts, <name>.png classes
//   perf reset|<name>         reset presenter and map redraw timing counters, or
//                             write them to <out>/<name>.json and reset
//   log <text>                write a line to the debug log
//   quit [<code>]             exit the process

// Called from `main` before SDL starts. When a script is requested the game runs
// unattended: background app on macOS (never takes focus), hidden window, no audio
// device, and it counts as active without ever receiving focus.
void devAutomationEarlyInit(int argc, char** argv);
bool devAutomationWantsHiddenWindow();

void devAutomationParseArgs(int argc, char** argv);
bool devAutomationIsActive();

// `--dev-fast`: palette fades take no time (tests that measure fades leave it off).
bool devAutomationFastFades();

// Advances the script; called once per input poll.
void devAutomationTick();

// Replaces the hardware mouse state while a script is active.
bool devAutomationOverrideMouse(MouseData* mouseData);

// Called by palette fades for `fadeshot` captures.
void devAutomationOnFadeStep(int step, int steps, bool toBlack);

// Engine state marks matched by `until`.
void devAutomationSetState(const char* state);
const char* devAutomationGetState();

} // namespace fallout

#endif /* FALLOUT_DEV_AUTOMATION_H_ */
