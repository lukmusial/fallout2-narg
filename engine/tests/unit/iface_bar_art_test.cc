// The wide interface bar built from the 640-pixel one (iface_bar_art.h).
// (The check with the game's own iface.frm, which needs the game data, is
// test_iface_bar_built in tests/run_tests.py.)

#include "iface_bar_art.h"

#include <stdio.h>

#include <algorithm>
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
            // Rows 0 and 1 identify the column; the rest is texture.
            int value = y == 0 ? x & 0xFF : (y == 1 ? x >> 8 : x * 7 + y * 13);
            iface[y * kWidth + x] = static_cast<unsigned char>(value & 0xFF);
        }
    }

    // Columns 0..29 and 185..639 kept; the span 30..184 stretched without
    // repeating: each output column maps to one source column, the mapping
    // is non-decreasing, every span column appears 1 to 3 times, and the
    // runs are irregular (not one count throughout).
    for (int width : { 800, 1024, 641 }) {
        std::vector<unsigned char> bar;
        CHECK(interfaceBarArtBuild(iface.data(), kWidth, kHeight, width, bar));
        CHECK(bar.size() == static_cast<size_t>(width * kHeight));
        if (bar.size() != static_cast<size_t>(width * kHeight)) {
            continue;
        }
        std::vector<int> source(width, -1);
        int unmatched = 0;
        for (int x = 0; x < width; x++) {
            for (int candidate = 0; candidate < kWidth; candidate++) {
                bool same = true;
                for (int y = 0; y < kHeight && same; y++) {
                    same = bar[y * width + x] == iface[y * kWidth + candidate];
                }
                if (same) {
                    source[x] = candidate;
                    break;
                }
            }
            if (source[x] < 0) {
                unmatched++;
            }
        }
        CHECK(unmatched == 0);
        for (int x = 0; x < 30; x++) {
            CHECK(source[x] == x);
        }
        for (int x = 0; x < 455; x++) {
            CHECK(source[width - 455 + x] == 185 + x);
        }
        std::vector<int> counts(640, 0);
        bool monotonic = true;
        for (int x = 0; x < width; x++) {
            if (x > 0 && source[x] < source[x - 1]) {
                monotonic = false;
            }
            if (source[x] >= 0) {
                counts[source[x]]++;
            }
        }
        CHECK(monotonic);
        int low = 1 << 30;
        int high = 0;
        for (int column = 30; column < 185; column++) {
            low = std::min(low, counts[column]);
            high = std::max(high, counts[column]);
        }
        CHECK(low >= 1);
        if (width == 800) {
            CHECK(high <= 3);
            CHECK(high > low);
        }
    }

    std::vector<unsigned char> bar;
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
