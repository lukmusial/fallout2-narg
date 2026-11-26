#ifndef FALLOUT_IFACE_BAR_ART_H_
#define FALLOUT_IFACE_BAR_ART_H_

#include <string.h>

#include <vector>

namespace fallout {

struct Art;

// The wide interface bar (art/intrface/HR_IFACE_<width>.FRM, used with
// [ui] iface_bar_width) built from the game's own 640-pixel bar (iface.frm):
// the middle of the message box is repeated so the box is `width - 640`
// pixels wider; every other pixel keeps its original palette index.
//
// The HR_IFACE_800.FRM bundled in f2_res.dat is a Russian redraw of the bar.
// When that is the file the game would load, or there is none, the bar is
// built instead, in any language (it then carries the labels of the game's
// own, possibly localized, iface.frm). A different file (a mod's, such as
// RPU's English one) is used as is.

// Art for `path` when it names a wide bar that is built as above, nullptr
// otherwise (load the file as usual). The result is freed with
// internal_free, like artLoad's.
Art* interfaceBarArtLoad(const char* path);

// Columns 100..119 of iface.frm lie inside the message box, between its
// rounded ends.
constexpr int kInterfaceBarSegmentStart = 100;
constexpr int kInterfaceBarSegmentEnd = 120;

// The wide bar's pixels from the 640-pixel bar's (`iface`, `ifaceWidth` 640).
// Returns false when `width` can't be built (not 640 plus a multiple of the
// repeated segment).
inline bool interfaceBarArtBuild(const unsigned char* iface, int ifaceWidth, int height, int width, std::vector<unsigned char>& pixels)
{
    constexpr int segmentStart = kInterfaceBarSegmentStart;
    constexpr int segmentWidth = kInterfaceBarSegmentEnd - kInterfaceBarSegmentStart;
    const int extra = width - ifaceWidth;
    if (iface == nullptr || ifaceWidth != 640 || height <= 0 || extra <= 0 || extra % segmentWidth != 0) {
        return false;
    }

    pixels.resize(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; y++) {
        const unsigned char* src = iface + static_cast<size_t>(y) * ifaceWidth;
        unsigned char* dest = pixels.data() + static_cast<size_t>(y) * width;
        memcpy(dest, src, segmentStart);
        dest += segmentStart;
        for (int repeat = 0; repeat < extra / segmentWidth; repeat++) {
            memcpy(dest, src + segmentStart, segmentWidth);
            dest += segmentWidth;
        }
        memcpy(dest, src + segmentStart, ifaceWidth - segmentStart);
    }
    return true;
}

} // namespace fallout

#endif /* FALLOUT_IFACE_BAR_ART_H_ */
