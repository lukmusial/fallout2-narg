#ifndef FALLOUT_IFACE_BAR_ART_H_
#define FALLOUT_IFACE_BAR_ART_H_

#include <stdint.h>
#include <string.h>

#include <vector>

namespace fallout {

struct Art;

// The wide interface bar (art/intrface/HR_IFACE_<width>.FRM, used with
// [ui] iface_bar_width) built from the game's own 640-pixel bar (iface.frm):
// the straight part of the message box is stretched so the box is
// `width - 640` pixels wider; every pixel is one of iface.frm's.
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

// The straight part of iface.frm's message box (columns 30..184, between
// its rounded ends), which is stretched to make the box wider.
constexpr int kInterfaceBarSpanStart = 30;
constexpr int kInterfaceBarSpanEnd = 185;

// How many times each column of the span appears in a bar `width` pixels
// wide: about (span + width - 640) / span each, so 1 to 3 times at 800, the
// counts spread irregularly (a fixed pseudo-random sequence) so the stretch
// has no period. The mapping is monotonic: no part of the box repeats.
inline bool interfaceBarArtColumnCounts(int width, std::vector<int>& counts)
{
    const int span = kInterfaceBarSpanEnd - kInterfaceBarSpanStart;
    const int length = span + width - 640;
    if (width <= 640 || length > span * 16) {
        return false;
    }

    uint32_t state = 0x2545F491u;
    auto next = [&state]() {
        state = (state * 1103515245u + 12345u) & 0x7fffffffu;
        return static_cast<int>(state >> 8);
    };

    const int base = length / span;
    counts.assign(span, base);
    for (int extra = length - base * span; extra > 0;) {
        int index = next() % span;
        if (counts[index] == base) {
            counts[index]++;
            extra--;
        }
    }
    for (int step = 0; step < 2 * span; step++) {
        int to = next() % span;
        int from = next() % span;
        if (to != from && counts[to] < base + 1 && counts[from] > base - 1 && counts[from] > 1) {
            counts[to]++;
            counts[from]--;
        }
    }
    return true;
}

// The wide bar's pixels from the 640-pixel bar's (`iface`, `ifaceWidth` 640):
// columns 0..29 and 185..639 as they are, the span between them stretched
// (interfaceBarArtColumnCounts). Pixels are only copied, so every palette
// index comes from iface.frm. Returns false when `width` can't be built.
inline bool interfaceBarArtBuild(const unsigned char* iface, int ifaceWidth, int height, int width, std::vector<unsigned char>& pixels)
{
    std::vector<int> counts;
    if (iface == nullptr || ifaceWidth != 640 || height <= 0 || !interfaceBarArtColumnCounts(width, counts)) {
        return false;
    }

    std::vector<int> columns;
    columns.reserve(width);
    for (int x = 0; x < kInterfaceBarSpanStart; x++) {
        columns.push_back(x);
    }
    for (size_t index = 0; index < counts.size(); index++) {
        columns.insert(columns.end(), counts[index], kInterfaceBarSpanStart + static_cast<int>(index));
    }
    for (int x = kInterfaceBarSpanEnd; x < ifaceWidth; x++) {
        columns.push_back(x);
    }
    if (static_cast<int>(columns.size()) != width) {
        return false;
    }

    pixels.resize(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; y++) {
        const unsigned char* src = iface + static_cast<size_t>(y) * ifaceWidth;
        unsigned char* dest = pixels.data() + static_cast<size_t>(y) * width;
        for (int x = 0; x < width; x++) {
            dest[x] = src[columns[x]];
        }
    }
    return true;
}

} // namespace fallout

#endif /* FALLOUT_IFACE_BAR_ART_H_ */
