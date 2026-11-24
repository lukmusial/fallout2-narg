#ifndef FALLOUT_HD_DEV_AUTOMATION_H_
#define FALLOUT_HD_DEV_AUTOMATION_H_

namespace fallout {

// HD commands for the scripted test mode (Stimpak's dev_automation.h), added
// through its extension API:
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
//   hdlights field <name>     write the applied light field as images
//   refresh                   redraw the whole map window (in a game)
//   benchrefresh <name> <n>   redraw it n times in a row; min/median/max ms
//                             per redraw (and HD tile drawing) to <name>.json
//   hdcheck <name>            self-check of the map's HD layer (floors, roofs,
//                             walls, scenery, items; see tileHdCheck):
//                             <name>.json counts, <name>.png classes
//   perf reset|<name>         reset presenter and map redraw timing counters, or
//                             write them to <out>/<name>.json and reset
//                             (map view counters to <name>_view.json)
//   walk <rotation> <hexes> [run]
//                             the player walks (runs) in a straight line
//   scrollhold <dx> <dy> <ms> scroll the map as a held arrow key would; the
//                             next commands run meanwhile
//   frames <name> <count> [<x> <y> <w> <h> [<every>]]
//                             the next <count> presented frames (every
//                             <every>th, default 1; a crop in
//                             output pixels) to <name>_NN.png and the view
//                             per frame to <name>.json; runs meanwhile
//   framesdone                wait until the `frames` captures are written
//   zoom <level> [now] | in | out | reset
//                             map view zoom ([graphics] zoom=1)
//   viewsettle                wait until the map view stops panning/zooming
//   viewdump <name>           the map view and player positions to <name>.json
//   mousetoparty <n>          the cursor to party member n's sprite as shown
//   mousetotile <tile|dude>   the cursor to the centre of a hex as shown
//   pick <name>               the object and hex under the cursor to <name>.json
//   worldmap                  open the world map (in a game)
//   die                       the player dies (in a game): the death scene
//   gotomap <map> [<elevation> <tile>]
//                             the party moves to another map (a map
//                             transition; without a tile the player keeps
//                             its hex)
//   shoot <pid> nearest|party <n> [secondary]
//                             the player gets weapon <pid> (loaded) in the
//                             active hand and attacks the nearest critter
//                             outside the party (or party member n), which
//                             is given 999 HP so the scene goes on
//   hdfx shot|wind on|off     switch the gunshot light / wind live
//   hdfx dump <name>          shots fired and plants swaying to <name>.json
//   hdfx shotlight <mx> <my> <tx> <ty> [<damage type> [explosion]]
//                             a shot light at once (map window coordinates)
//   hdfx waitshot <n>         wait until n more shots were fired, then hold
//                             the shots' clock (their light stays)
//   hdfx shotclock hold|run   hold the shots' clock or let it run
//   hdfx windtime <ms>|run    hold the wind at a time, or let it run again
//   hdfx gust dude|<tile> <ms> a gust over that place from <ms> of the wind clock
//   hdfx legacycheck <name>   whole map redraws with the wind at four times
//                             and off: changed 8-bit pixels to <name>.json
//
// In HD mode `shot` writes the presented framebuffer (see screenshotSavePng).

// Registers the commands; called once at start-up (directDrawInit).
void hdDevAutomationInit();

// Engine state marks for `until` from HD-tree code (devAutomationSetState).
void hdDevAutomationMark(const char* state);

// Called after every present, for `frames` captures.
void hdDevAutomationOnPresent();

// Called by palette fades for `fadeshot` captures.
void hdDevAutomationOnFadeStep(int step, int steps, bool toBlack);

} // namespace fallout

#endif /* FALLOUT_HD_DEV_AUTOMATION_H_ */
