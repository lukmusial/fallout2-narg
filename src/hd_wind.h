#ifndef FALLOUT_HD_WIND_H_
#define FALLOUT_HD_WIND_H_

#include <string>
#include <vector>

#include "obj_types.h"

namespace fallout {

// Wind (Phase 2.15, `[graphics] hd_wind=1`, needs hd=1): plants (trees,
// bushes, grass, corn, cacti; a list of scenery art names, see hd_wind.cc)
// sway in the map window's HD layer. The 8-bit buffers never change: the
// legacy plant stays where it is, only its HD image is drawn displaced.
//
// The sway is a horizontal shift per output row: none for the bottom 35% of
// the sprite (the base and trunk stay put), growing with the square of the
// height above that to the full amount at the top. The amount is a few
// output pixels for trees and less for small plants and stiff cacti, each
// plant out of step (phase from its tile), modulated by slow gusts.
//
// Compositing: an HD pixel shows only where the window buffer still holds the
// index recorded as its shadow (hd_layer.h). A plant is therefore drawn by
// its own object draw (object.cc) into the rectangle being redrawn widened
// by the sway margin: the layer's colours there are captured before the
// legacy draw (floor and everything drawn earlier); each pixel the shifted
// HD plant covers gets the plant over that, each pixel the legacy plant
// covers but the shifted one leaves gets what was under it, both with the
// buffer's current index as shadow. Objects drawn later (critters in front)
// still win, scrolling moves the result with the buffer, and the legacy
// pixels never change.
//
// Moving the plants does not redraw the map: a ticker (about 18 Hz) finds
// the plants on screen whose shifted rows changed and recomposes only the
// HD layer around them (object.cc, the wind scene): from what each plant's
// last draw recorded (the colours under the first plant at each pixel, and
// which pixels the plants still own), the plants are painted again in the
// engine's order over that base, and a pixel is written only where the layer
// still shows exactly what the plants left (critters, roofs, text drawn
// over them since stay). Plants the scene does not know yet (new on screen,
// partly redrawn) get one ordinary redraw of their rectangle instead.

bool hdWindEnabled();
void hdWindSetEnabled(bool enabled);

// Adds the ticker (map start-up); harmless when the effect is off.
void hdWindInit();
void hdWindExit();

// For the object draw: whether `object` is a plant that sways (also at a
// moment it is upright). `shifts` receives one
// shift per output row of a frame `frameHeight` logical rows tall (2 *
// frameHeight values, output pixels, positive to the right) and `margin`
// the widest shift in logical pixels (rounded up).
bool hdWindSway(Object* object, int frameHeight, std::vector<int>* shifts, int* margin);

// An object is being freed (object.cc).
void hdWindForget(Object* object);

// Harness: hold the wind at `ms` (the sway is a function of time), or let it
// run with the clock again (`ms` < 0).
// Test mode: a gust over `tile`, wide enough to reach the whole view, that starts
// blowing at `startMs` of the wind clock (see hdWindSetTime).
void hdWindGust(int tile, unsigned int startMs);

void hdWindSetTime(int ms);

// JSON: plants on screen, their sway now, ticker timing (harness).
std::string hdWindDescribe();

} // namespace fallout

#endif /* FALLOUT_HD_WIND_H_ */
