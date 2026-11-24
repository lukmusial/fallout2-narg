#ifndef FALLOUT_HD_SHOT_LIGHT_H_
#define FALLOUT_HD_SHOT_LIGHT_H_

#include <string>
#include <vector>

#include "hd_lights.h"
#include "obj_types.h"

namespace fallout {

// Gunshot light (Phase 2.15, `[graphics] hd_shot_light=1`, needs hd=1): every
// ranged attack lights its surroundings for a moment from the weapon's
// muzzle, through the HD light field (hd_lights.h), so floors, walls,
// scenery and critters are lit alike; explosions give a bigger flash all
// around. Nothing changes in the 8-bit buffers or the game state.
//
// When: the attack registers itself (hdShotLightAttack, from the ranged
// attack sequence); the light follows the attacker's animation as drawn.
// The fire animations have their muzzle flashes drawn in: the frames whose
// bright flash pixels peak are the shots (one per round of a burst, at
// whatever rate the art fires); single shots use the art's action frame.
//
// Where: the muzzle is found in the shot frame itself: the flash pixels are
// the bright warm pixels that the quietest frame of the same direction does
// not have at that place; of those within reach of the hands (36 px sideways
// from the critter's centre: a flamer's stream or a rocket's trail reach
// further), the cluster around the frontmost one along the facing (stray
// body pixels that moved are not connected to it), and its near end is the
// muzzle. Without flash pixels the muzzle is the sprite's frontmost pixel
// along the facing at weapon height (45-75% of the sprite's height).
//
// Light: a cone from the muzzle towards the target (half-angle ~35 degrees,
// falling off with distance), a small glow at the muzzle, a flash streak
// along the aim; 150 ms (peak, then decay). Colour by damage type: warm
// yellow (bullets), red (laser), green (plasma), orange (fire), blue
// (electrical, EMP).

bool hdShotLightEnabled();
void hdShotLightSetEnabled(bool enabled);

// A ranged attack is being sequenced (actions.cc): `targetTile` is where the
// shot goes; `animation` the attacker's fire animation.
void hdShotLightAttack(Object* attacker, int targetTile, Object* weapon, int damageType, int animation);

// An explosion object (rocket, grenade, dynamite) that will show `fid` (-1:
// whatever it shows) once it is unhidden.
void hdShotLightExplosion(Object* explosion, int fid, int damageType);

// For the light field update (hd_lights_map.cc): the shots alive now, in map
// window coordinates; `night` 0 (day) .. 1 scales the light (not the flash).
// Returns true while shots are alive, and once more after the last one ends.
bool hdShotLightCollect(float night, std::vector<HdShotLight>* shots, std::vector<HdLightFlash>* flashes);

// JSON: pending attacks, shots alive and the last shots fired (harness).
std::string hdShotLightDescribe();

// Harness: how many shots have been fired; hold the shots' clock (the light
// stays as it is) or let it run again.
int hdShotLightFiredCount();
void hdShotLightHold(bool hold);

// A shot at once (harness `shotlight`): muzzle and target in map window
// coordinates, `damageType` for the colour.
void hdShotLightFire(float muzzleX, float muzzleY, float targetX, float targetY, int damageType, bool explosion);

} // namespace fallout

#endif /* FALLOUT_HD_SHOT_LIGHT_H_ */
