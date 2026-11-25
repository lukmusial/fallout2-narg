#ifndef FALLOUT_HD_FORMAT_H_
#define FALLOUT_HD_FORMAT_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "hd_layer.h"

namespace fallout {

// HD art files (engine-independent, unit-tested).
//
// For a legacy art file `art/<type>/<name>.frm` an HD pack provides either
//
//   hd/<pack>/art/<type>/<name>.png   one image, used for frame 0 of
//                                     direction 0 (interface screens,
//                                     single-frame art), or
//   hd/<pack>/art/<type>/<name>.fhd   a container with any of the frames.
//
// Images are true-colour PNGs (any colour type; alpha is straight, not
// premultiplied) nominally twice the legacy frame's size. They cover exactly
// the legacy frame's rectangle, so frame and direction offsets come from the
// legacy FRM and are not repeated here; an image of another size is resampled
// to 2x the legacy frame.
//
// .fhd layout (all integers little-endian):
//
//   offset  size  field
//   0       4     magic "FHD1"
//   4       2     uint16 direction count (1..6; 1 = same images for all)
//   6       2     uint16 frames per direction (>= 1)
//   8       4     uint32 flags (reserved, must be 0)
//   12      8*N   entry table, N = directions * frames, direction-major
//                 (entry index = direction * frames + frame); each entry is
//                 uint32 offset, uint32 size of a PNG stream within the file.
//                 size 0 means "no HD image for this frame" (the engine falls
//                 back to the legacy frame).
//   ...           PNG streams
//
// tools/dev/fhd.py builds, lists and extracts .fhd files.

constexpr int kHdFhdMaxDirections = 6;

struct HdFhdEntry {
    uint32_t offset = 0;
    uint32_t size = 0;
};

struct HdFhdInfo {
    int directions = 0;
    int frames = 0;
    std::vector<HdFhdEntry> entries;

    // Entry for a frame/direction, or nullptr when out of range or empty.
    const HdFhdEntry* find(int frame, int direction) const;
};

bool hdFhdParse(const unsigned char* data, size_t size, HdFhdInfo* info, std::string* error = nullptr);

// Normalises an art path to the lookup key form: lower case, forward slashes,
// starting at "art/" (e.g. "ART\\INTRFACE\\MAINMENU.FRM" ->
// "art/intrface/mainmenu.frm").
std::string hdArtNormalizePath(const char* artPath);

// Parses a pack manifest: `<art path> = <pack>` lines, `* = <pack>` for the
// default, `#` comments. Paths are normalised.
void hdManifestParse(const std::string& text, std::unordered_map<std::string, std::string>* entries, std::string* defaultPack);

// Decodes a PNG stream to 0xAARRGGBB.
bool hdImageDecodePng(const unsigned char* data, size_t size, HdImage* image, std::string* error = nullptr);

// Legacy source hashes. A pack's `legacy.txt` (tools/hd/legacy_art.py) lists
// the legacy frame each HD image was made from:
//
//   <art path> <direction> <frame> <hash>
//
// <art path> normalised (hdArtNormalizePath, keeping the legacy file's own
// extension), <hash> 16 hex digits of hdLegacyFrameHash. The engine uses an HD
// image only while the running game's frame still has that hash, so a mod
// that replaces the art (another picture under the same path) gets its own
// picture drawn instead of the pack's image of the original.

// FNV-1a 64 over width and height (uint16 little-endian each), then the
// frame's palette indices row by row (`pitch` bytes between rows).
uint64_t hdLegacyFrameHash(const unsigned char* pixels, int width, int height, int pitch);

// The table key of a frame: "<art path>#<direction>/<frame>".
std::string hdLegacyKey(const std::string& artPath, int direction, int frame);

// Parses legacy.txt into key -> hash; malformed lines are skipped. Returns
// the number of entries read.
size_t hdLegacyParse(const std::string& text, std::unordered_map<std::string, uint64_t>* entries);

} // namespace fallout

#endif /* FALLOUT_HD_FORMAT_H_ */
