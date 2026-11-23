#ifndef FALLOUT_HD_LIGHTS_MAP_H_
#define FALLOUT_HD_LIGHTS_MAP_H_

#include <string>

#include "hd_lights.h"

namespace fallout {

// The map side of HD lighting (see hd_lights.h): builds the light field from
// the game state. Enabled with `[graphics] hd_lights=1` (needs hd=1);
// `hd_lights_effects` picks the effect groups (kHdLightColour | kHdLightGlow
// | kHdLightShadows, default all).
//
// Emitters:
// - objects with a light (lightRadius / lightIntensity): the hexes the
//   engine lights for them (walls block light as in the game) become a smooth
//   footprint; its colour comes from the object's art, or nearby art, by the
//   colour-cycling indices it uses (fire: warm orange, flickering with the
//   fire palette cycle; monitors: their blue; slime: green), else a warm
//   white lamp. Lights carried by critters (the player's own light) add no
//   colour.
// - emissive pixels on screen (fire, monitor, slime and alarm cycling
//   indices): a wide coloured pool (light) and a tight additive glow, from
//   the presented palette, so both follow the colour cycling and fades.
// - shadows: a soft contact shadow under every living critter and a
//   directional one cast away from each nearby coloured light.
//
// Strengths scale with darkness (ambient light): full at night, subtle at day.

// Screen input for one update: the presenter's planes (logical size).
struct HdLightScreen {
    const unsigned char* index = nullptr;
    const unsigned char* mask = nullptr;
    int width = 0;
    int height = 0;
    // Presented palette, 8 bits per channel.
    const unsigned char* palette8 = nullptr;
};

// Called by the presenter before every present while HD is active. Returns
// true when `field` was rebuilt (it is then complete and finished), false
// when it is unchanged. Returns a cleared field once when lights go off.
bool hdLightsMapUpdate(const HdLightScreen& screen, HdLightField* field);

// Whether the presenter should light map pixels at all.
bool hdLightsMapEnabled();

// Live switch (harness `hdlights`): effect groups, 0 = off.
void hdLightsMapSetEffects(unsigned effects);
unsigned hdLightsMapGetEffects();

// JSON with the current emitters, ambient and timing (harness `hdlights dump`).
std::string hdLightsMapDescribe();

// Timing counters (ms), reset by hdLightsMapResetStats.
struct HdLightsStats {
    int updates = 0;
    int rebuilds = 0;
    double updateMs = 0.0;
    double updateMaxMs = 0.0;
    double rebuildMs = 0.0;
};

void hdLightsMapGetStats(HdLightsStats* stats);
void hdLightsMapResetStats();

} // namespace fallout

#endif /* FALLOUT_HD_LIGHTS_MAP_H_ */
