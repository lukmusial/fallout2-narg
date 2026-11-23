#ifndef FALLOUT_HD_ART_H_
#define FALLOUT_HD_ART_H_

#include "hd_layer.h"

namespace fallout {

// Loads a PNG (any colour type) through the game's file layering
// (patches folder, mods, ce.dat, master.dat) as a true-colour image.
bool hdImageLoadPng(const char* path, HdImage* image);

// Draws the HD version of an interface image into a window, if one exists.
// `artPath` is the legacy art path, e.g. "art/intrface/mainmenu.frm";
// (x, y, width, height) is where the legacy image was drawn, in window
// coordinates. Returns false (and draws nothing) when HD is off or no HD
// art is found.
bool hdArtDrawInterface(int win, const char* artPath, int x, int y, int width, int height);

} // namespace fallout

#endif /* FALLOUT_HD_ART_H_ */
