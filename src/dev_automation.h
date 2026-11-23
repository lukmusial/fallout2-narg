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
//   key <name|char|code>      send one key (esc, enter, space, tab, backspace,
//                             up, down, left, right, home, end, pageup, pagedown, f1..f12)
//   text <string>             type the rest of the line
//   move <x> <y>              move the cursor (logical screen coordinates)
//   click <x> <y> [right]     move and click
//   shot <name>               write <out>/<name>.png
//   dump <name>               write <out>/<name>.json with player, party, quest and world state
//   fbshot <name>             write the presented framebuffer (output resolution)
//   fadeshot <name> <pct> [in|out]
//                             during the next palette fade (in: to a non-black
//                             palette, out: to black) write the framebuffer at
//                             <pct>% of the fade, plus <name>_6bit.png rendered
//                             through the 6-bit palette as the old presenter did
//   hdpack <pack|->           switch the HD art pack live (- = none)
//   hdprobe <art path> <name> look up the HD image of frame 0 (2x legacy
//                             fallback); write <name>.png and <name>.json
//   hdstats <name>            write HD art cache counters to <name>.json
//   perf reset|<name>         reset presenter timing counters, or write them to
//                             <out>/<name>.json and reset
//   log <text>                write a line to the debug log
//   quit [<code>]             exit the process

// Called from `main` before SDL starts. When a script is requested the game runs
// unattended: background app on macOS (never takes focus), hidden window, no audio
// device, and it counts as active without ever receiving focus.
void devAutomationEarlyInit(int argc, char** argv);
bool devAutomationWantsHiddenWindow();

void devAutomationParseArgs(int argc, char** argv);
bool devAutomationIsActive();

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
