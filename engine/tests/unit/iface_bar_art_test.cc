// The wide interface bar built from the 640-pixel one (iface_bar_art.h):
// the same columns as tools/dev/iface_bar_art.py, which made the
// HR_IFACE_800.FRM shipped before. (The check against the game's own
// iface.frm, which needs the game data, is test_iface_bar_built in
// tests/run_tests.py.)

#include "iface_bar_art.h"

#include <stdio.h>

#include <vector>

using namespace fallout;

static int gFailures = 0;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                #condition);                                                \
            gFailures++;                                                    \
        }                                                                   \
    } while (0)

int main()
{
    constexpr int kWidth = 640;
    constexpr int kHeight = 99;
    std::vector<unsigned char> iface(kWidth * kHeight);
    for (int y = 0; y < kHeight; y++) {
        for (int x = 0; x < kWidth; x++) {
            iface[y * kWidth + x] = static_cast<unsigned char>((x * 7 + y * 13) & 0xFF);
        }
    }

    // As iface_bar_art.py: columns 0..99, then 100..119 eight times, then
    // 100..639.
    std::vector<unsigned char> bar;
    CHECK(interfaceBarArtBuild(iface.data(), kWidth, kHeight, 800, bar));
    CHECK(bar.size() == static_cast<size_t>(800 * kHeight));
    int mismatches = 0;
    for (int y = 0; y < kHeight; y++) {
        for (int x = 0; x < 800; x++) {
            int source;
            if (x < 100) {
                source = x;
            } else if (x < 260) {
                source = 100 + (x - 100) % 20;
            } else {
                source = x - 160;
            }
            if (bar[y * 800 + x] != iface[y * kWidth + source]) {
                mismatches++;
            }
        }
    }
    CHECK(mismatches == 0);

    // Other widths: 640 plus a multiple of the 20-column segment.
    CHECK(interfaceBarArtBuild(iface.data(), kWidth, kHeight, 1020, bar));
    CHECK(bar.size() == static_cast<size_t>(1020 * kHeight));
    CHECK(bar[1019] == iface[639]);
    CHECK(!interfaceBarArtBuild(iface.data(), kWidth, kHeight, 810, bar));
    CHECK(!interfaceBarArtBuild(iface.data(), kWidth, kHeight, 640, bar));
    CHECK(!interfaceBarArtBuild(iface.data(), 600, kHeight, 800, bar));
    CHECK(!interfaceBarArtBuild(nullptr, kWidth, kHeight, 800, bar));

    if (gFailures != 0) {
        fprintf(stderr, "%d check(s) failed\n", gFailures);
        return 1;
    }
    printf("iface_bar_art_test: all checks passed\n");
    return 0;
}
