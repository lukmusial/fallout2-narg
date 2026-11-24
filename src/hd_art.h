#ifndef FALLOUT_HD_ART_H_
#define FALLOUT_HD_ART_H_

#include <stddef.h>

#include <memory>
#include <string>

#include "hd_layer.h"

namespace fallout {

class FrmId;
class FrmImage;

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

// Changes whenever the pack changes, so callers can drop images they keep.
unsigned int hdArtGeneration();

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

// The pack's HD image for a locked image (its art path and frame), or null.
std::shared_ptr<const HdImage> hdArtFindImage(const FrmImage& image);

// Interface screens (Phase 2.6). These draw the pack's HD version of a locked
// legacy image that the caller has just drawn into window `win`; HD pixels go
// only where the window buffer still equals the image, so text, numbers or
// other legacy drawing into or over the art stay legacy. No fallback: they
// return false and draw nothing when HD is off or the pack has no image.

// Whole image, drawn by the legacy code at (x, y) (window coordinates).
bool hdArtDrawImage(int win, const FrmImage& image, int x, int y);

// The rectangle (srcX, srcY, width, height) of the image, drawn at (destX, destY).
bool hdArtDrawImageRegion(int win, const FrmImage& image, int srcX, int srcY, int width, int height, int destX, int destY);

// The same with the legacy blit's arguments: `srcOffset` into the image's
// data (row pitch = the image's width) and `destOffset` into the window
// buffer (row pitch `destPitch`), as in `blitBufferToBuffer(image.getData()
// + srcOffset, width, height, image.getWidth(), buffer + destOffset,
// destPitch)`.
bool hdArtDrawImageOffset(int win, const FrmImage& image, int srcOffset, int width, int height, int destOffset, int destPitch);

// Same for a frame of any art id whose legacy pixels (`legacyData`,
// `legacyWidth` x `legacyHeight`, pitch `legacyWidth`) the caller blitted
// itself, e.g. from cached art data (world map terrain, animated dials).
bool hdArtDrawFrameRegion(int win, const FrmId& frmId, int frame, const unsigned char* legacyData, int legacyWidth,
    int legacyHeight, int srcX, int srcY, int width, int height, int destX, int destY);

// Same for an HD image the caller made itself (e.g. a composite of two
// screens), with `source` (sourceWidth x sourceHeight, 1x) the legacy pixels
// it stands for.
bool hdArtDrawRegion(int win, const HdImage& hd, const unsigned char* source, int sourceWidth, int sourceHeight,
    int srcX, int srcY, int width, int height, int destX, int destY);

// Inventory items (Phase 2.10): draws art into window `win` like artRender
// (centred in the box (x, y, width, height), scaled down to fit when larger),
// then the pack's HD image of it, resampled to the drawn size, over what the
// window's HD layer showed there. Without HD (or a pack image) it is exactly
// artRender.
void hdArtRender(int win, const FrmId& frmId, int x, int y, int width, int height);

// Gives a graphic button the HD art of the images it was made from (see
// buttonSetHdArt). The button images must be the size of these images; they
// may be copies with labels drawn in.
int hdArtSetButtonArt(int btn, const FrmImage* normal, const FrmImage* pressed, const FrmImage* hover = nullptr);

// For every graphic button of window `win` without HD art yet whose normal,
// pressed or hover image is the data of one of `images` (or of the matching
// entry of `copies`, a same-size copy with a label drawn in; entries may be
// null or (unsigned char*)-1), sets that image's HD art. Lets a screen that
// builds its buttons from a table of locked images get HD buttons in one
// call; call again after creating more buttons.
void hdArtAttachButtonArt(int win, const FrmImage* images, int count, unsigned char* const* copies = nullptr);

struct HdArtStats {
    int assetsResolved; // distinct art paths looked up
    int assetsWithHd; // of those, with a pack image
    int framesCached;
    size_t bytesCached;
    unsigned long long hits;
    unsigned long long misses;
};

void hdArtGetStats(HdArtStats* stats);

// Gives Stimpak's screens HD art (see stimpakSetRenderHooks).
void hdArtAttachStimpak();

} // namespace fallout

#endif /* FALLOUT_HD_ART_H_ */
