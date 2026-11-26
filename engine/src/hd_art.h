#ifndef FALLOUT_HD_ART_H_
#define FALLOUT_HD_ART_H_

#include <stddef.h>

#include <memory>
#include <string>

#include "hd_layer.h"

namespace fallout {

class FrmId;
class FrmImage;
struct Art;

// HD art lookup and cache (file formats: see hd_format.h).
//
// The active pack is `[graphics] hd_pack` (a folder name under hd/, looked up
// through the normal file layering: data/ patches folder, mods, ce.dat,
// master.dat). Empty means no pack: every frame comes from the runtime
// upscale (hd_upscale=scripted, the default), or is doubled
// (hd_upscale=double). A pack folder may contain
// `manifest.txt` to take individual assets from other packs; this is how a
// "mixed" pack is made:
//
//   # hd/mixed/manifest.txt
//   * = scripted                       # default for assets not listed
//   art/intrface/mainmenu.frm = ai
//
// A pack may also contain `legacy.txt` (hd_format.h, tools/hd/legacy_art.py):
// the hash of every legacy frame its images were made from. An image is then
// only used while the running game's frame has that hash, so art a mod
// replaces (e.g. the Restoration Project's) is drawn from the mod's frame,
// doubled, instead of as the pack's picture of the original. `[graphics]
// hd_verify=0` turns the check off.
//
// A manifest value may name several packs, tried in order for each frame:
// the first with an image made from the frame the game draws is used. A pack
// made from a mod's art then follows the normal pack, and one install serves
// both the original game and the mod:
//
//   * = ai rpu                         # RPU's art where RPU changed or added it
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

// The pack a frame was taken from by the last lookup (empty: none).
std::string hdArtFramePack(const char* artPath, int frame, int direction);

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
    int assetsResolved = 0; // distinct art paths looked up
    int assetsWithHd = 0; // of those, with a pack image
    int framesCached = 0;
    size_t bytesCached = 0;
    unsigned long long hits = 0;
    unsigned long long misses = 0;
    int legacyChecked = 0; // frames whose legacy source was checked (legacy.txt)
    int legacyRejected = 0; // frames no pack has an image of as the running game draws them

    // Runtime scripted upscale (Phase 2.22).
    int runtimeServed = 0; // frames drawn from it
    unsigned long long runtimeInline = 0; // of those, upscaled on first draw (not ready in time)
    double runtimeInlineMs = 0; // time spent on those (main thread)
    double runtimeInlineMaxMs = 0;
    unsigned long long runtimeFromPool = 0; // taken from the background threads when first drawn
    unsigned long long drawUpscales = 0; // frames not ready when drawn (upscaled or waited for then)
    unsigned long long drawUpscalesSinceScene = 0; // of those, since the last scene preparation ended
    double runtimeTakeMs = 0; // main-thread time for those (waiting for one, or running a queued one)
    unsigned long long preupscaleQueued = 0; // frames queued at start and at map loads
    int upscaleThreads = 0;
    int upscalePending = 0; // queued or running now
    unsigned long long upscaleComputed = 0; // by the background threads
    unsigned long long upscaleComputedInline = 0;
    unsigned long long upscaleDiskHits = 0;
    unsigned long long upscaleDiskWrites = 0;
    double upscaleComputeMs = 0; // summed over threads
    unsigned long long upscaleOutputPixels = 0;
    // The last batch queued ("interface" at start, "map" at a map load):
    // frames, main-thread time queueing them, time until the background
    // threads were done with it (-1: not done yet).
    std::string batchName;
    int batchQueued = 0;
    double batchQueueMs = 0;
    double batchDoneMs = -1;
};

void hdArtGetStats(HdArtStats* stats);

// Runtime scripted upscale (Phase 2.22, `[graphics] hd_upscale=scripted`,
// src/hd_upscale.h). Lookup order for every frame: (1) the image of the
// first pack (manifest order) made from the frame the game draws; (2) when
// none, the frame upscaled at runtime with the scripted pack's algorithm
// (also when the pack is not installed: `hd_pack=scripted` and manifest
// entries naming it work without it; an installed pack still wins, its
// offline images had the tiles' and walls' real neighbours); (3) with
// `hd_upscale=double`, or for critters with `hd_upscale_critters=0`, the
// callers' own fallback (pixels doubled, or legacy).
//
// Frames are upscaled in background threads ahead of use where the game
// knows what it will draw (`hd_upscale_preload`): the early interface art at
// start and the art a map uses when it loads. A frame that is not ready when
// first drawn is upscaled right then on the main thread (or taken from the
// background thread working on it), never drawn doubled first: the map's
// and objects' image tables keep the first image they get, and a later
// swap would show as a pop. Workers only see copies of the pixels and the
// palette; nothing here locks art (the callers that have the frame pass it,
// others read the art file, never from inside an art cache read).

// Whether HD art is on: a pack is set, or the runtime upscale is (no pack
// needed: `hd_pack` empty with hd_upscale=scripted upscales everything).
bool hdArtActive();

// Whether critters are drawn HD (runtime upscale on, hd_upscale_critters).
bool hdArtUpscalesCritters();

// Queues the early interface art (start of the game).
void hdArtPreupscaleInterface();
// Job groups of the background upscale.
constexpr int kHdUpscaleGroupGeneral = 0; // interface, frames drawn
constexpr int kHdUpscaleGroupScene = 1; // what a map load waits for (hd_scene.h)
constexpr int kHdUpscaleGroupBackground = 2; // idle-time preparation

// Queues every frame of `art` (locked by the caller, the legacy art of
// `artPath`) that would be upscaled at runtime, at `priority` (lower first)
// in `group`. Returns how many.
int hdArtQueueArt(const char* artPath, Art* art, float priority, int group);
// The same for an art file read now (`palettePath`: the .pal the game shows
// it with, e.g. the death screen's; null: the current palette).
int hdArtQueueArtFile(const char* artPath, const char* palettePath, float priority, int group);
// Drops a group's queued frames.
void hdArtUpscaleCancel(int group);
// Frames of a group queued since its last reset, and how many are done.
void hdArtUpscaleProgress(int group, int* total, int* done);
void hdArtUpscaleResetGroup(int group);
// Floor and roof tiles upscaled with their real neighbours (hd_scene.h):
// `neighbours` [row delta + 1][screen column delta + 1] (null: the tile
// itself), each width x height packed. Cleared for every map.
void hdArtClearTileContexts();
void hdArtSetTileContext(const char* artPath, const unsigned char* const neighbours[3][3], int width, int height);
// Runs one queued frame on the calling (main) thread; false when none.
bool hdArtUpscaleHelp();
// Scene preparation bookkeeping (stats, "upscaled when drawn" counter).
void hdArtSceneBegin();
void hdArtSceneEnd(double queueMs);
// The HD art cache's size and budget (hd_cache_mb), bytes.
size_t hdArtCacheBytes();
size_t hdArtCacheBudget();
// An image that is not game art (a splash screen with its own palette),
// upscaled at runtime now (or from the cache); `name` identifies it.
std::shared_ptr<const HdImage> hdArtUpscaleImage(const std::string& name, const unsigned char* data, int width, int height,
    int pitch, const unsigned char* palette8);
// Frames queued or being upscaled in the background.
int hdArtUpscalePending();
// Waits until the background threads are idle and takes their results.
void hdArtUpscaleWaitIdle();

// Gives Stimpak's screens HD art (see stimpakSetRenderHooks).
void hdArtAttachStimpak();

} // namespace fallout

#endif /* FALLOUT_HD_ART_H_ */
