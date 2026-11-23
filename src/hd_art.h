#ifndef FALLOUT_HD_ART_H_
#define FALLOUT_HD_ART_H_

#include <stddef.h>

#include <memory>
#include <string>

#include "hd_layer.h"

namespace fallout {

class FrmId;

// HD art lookup and cache (file formats: see hd_format.h).
//
// The active pack is `[graphics] hd_pack` (a folder name under hd/, looked up
// through the normal file layering: data/ patches folder, mods, ce.dat,
// master.dat). Empty means no HD art. A pack folder may contain
// `manifest.txt` to take individual assets from other packs; this is how a
// "mixed" pack is made:
//
//   # hd/mixed/manifest.txt
//   * = scripted                       # default for assets not listed
//   art/intrface/mainmenu.frm = ai
//
// Decoded frames are kept in an LRU cache bounded by `[graphics]
// hd_cache_mb`. When an asset has no HD image, callers can ask for an
// automatic 2x expansion of the legacy frame instead, so later RGB effects
// (lighting) apply to every pixel.

// Switches the active pack (clears the cache). An empty name disables HD art.
void hdArtSetPack(const char* pack);
const std::string& hdArtGetPack();

// The pack's HD image for a frame of a legacy art file, or null.
std::shared_ptr<const HdImage> hdArtFindFrame(const char* artPath, int frame, int direction);

// As hdArtFindFrame; without a pack image, the legacy frame (`legacyData`,
// `width` x `height`, index 0 transparent) expanded 2x through the base
// palette. `fromLegacy` (optional) tells which one was returned.
std::shared_ptr<const HdImage> hdArtGetFrame(const char* artPath, int frame, int direction,
    const unsigned char* legacyData, int width, int height, int pitch, bool* fromLegacy = nullptr);

// Draws the pack's HD version of an interface image into a window. `artPath`
// is the legacy art path, (x, y, width, height) the rectangle the legacy image
// was drawn to (window coordinates). No fallback: returns false and draws
// nothing when HD is off or the pack has no image.
bool hdArtDrawInterface(int win, const char* artPath, int x, int y, int width, int height);

// Same for a frame of any art id; with `allowFallback` the legacy frame data
// is expanded 2x when the pack has no image.
bool hdArtDrawFrame(int win, const FrmId& frmId, int frame, int direction, int x, int y, int width, int height,
    const unsigned char* legacyData, int legacyPitch, bool allowFallback);

struct HdArtStats {
    int assetsResolved; // distinct art paths looked up
    int assetsWithHd; // of those, with a pack image
    int framesCached;
    size_t bytesCached;
    unsigned long long hits;
    unsigned long long misses;
};

void hdArtGetStats(HdArtStats* stats);

} // namespace fallout

#endif /* FALLOUT_HD_ART_H_ */
