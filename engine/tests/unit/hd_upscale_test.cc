// Unit tests for the runtime scripted upscale (src/hd_upscale.cc): the C++
// port against what tools/hd/enhance_scripted.py makes of the same frames
// (fixtures from tools/hd/make_upscale_fixtures.py: floor and roof tiles,
// scenery with transparency and colour-cycling pixels, an interface image),
// the disk cache, the thread pool, and the speed (reported, not checked).
//
// Requirement per fixture: the silhouette (alpha) identical, RGB PSNR over
// the opaque pixels >= 40 dB and no channel off by more than 24 levels (the
// float order and a polynomial exp shift a few pixels by a level or two;
// the DCCI picks one direction over the other at a threshold, which can flip
// on such a difference).

#include "hd_upscale.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <sys/utime.h>
#define rmdir _rmdir
#define utime _utime
#define utimbuf _utimbuf
#else
#include <unistd.h>
#include <utime.h>
#endif

#include <chrono>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include <lodepng.h>

using namespace fallout;

#ifndef HD_UPSCALE_FIXTURES
#define HD_UPSCALE_FIXTURES "tests/unit/fixtures/upscale"
#endif

static int gFailures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                #condition);                                                 \
            gFailures++;                                                     \
        }                                                                    \
    } while (0)

struct Fixture {
    std::string name;
    bool tile = false;
    unsigned width = 0;
    unsigned height = 0;
    std::vector<unsigned char> indices;
    unsigned char palette[256 * 3] = {};
    std::vector<unsigned char> expected; // RGBA, 2x
    std::vector<unsigned char> context[3][3]; // tiles with neighbours (<name>.ctx<r><c>.png)
    HdTileContext tileContext() const
    {
        HdTileContext result;
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                result.neighbour[r][c] = context[r][c].empty() ? nullptr : context[r][c].data();
            }
        }
        return result;
    }
};

static bool loadIndexed(const std::string& path, std::vector<unsigned char>* indices, unsigned* width, unsigned* height)
{
    std::vector<unsigned char> png;
    if (lodepng::load_file(png, path) != 0) {
        return false;
    }
    lodepng::State state;
    state.decoder.color_convert = 0;
    return lodepng::decode(*indices, *width, *height, state, png) == 0;
}

static bool loadFixture(const std::string& name, bool tile, Fixture* fixture)
{
    std::string base = std::string(HD_UPSCALE_FIXTURES) + "/" + name;
    std::vector<unsigned char> png;
    if (lodepng::load_file(png, base + ".in.png") != 0) {
        return false;
    }

    lodepng::State state;
    state.decoder.color_convert = 0;
    std::vector<unsigned char> raw;
    unsigned width = 0;
    unsigned height = 0;
    if (lodepng::decode(raw, width, height, state, png) != 0 || state.info_png.color.colortype != LCT_PALETTE
        || state.info_png.color.bitdepth != 8) {
        fprintf(stderr, "%s: expected an 8-bit palette PNG\n", name.c_str());
        return false;
    }
    fixture->name = name;
    fixture->tile = tile;
    fixture->width = width;
    fixture->height = height;
    fixture->indices = raw;
    for (size_t i = 0; i < state.info_png.color.palettesize && i < 256; i++) {
        fixture->palette[i * 3] = state.info_png.color.palette[i * 4];
        fixture->palette[i * 3 + 1] = state.info_png.color.palette[i * 4 + 1];
        fixture->palette[i * 3 + 2] = state.info_png.color.palette[i * 4 + 2];
    }

    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            unsigned cw = 0;
            unsigned ch = 0;
            std::vector<unsigned char> neighbour;
            char suffix[16];
            snprintf(suffix, sizeof(suffix), ".ctx%d%d.png", r, c);
            if (loadIndexed(base + suffix, &neighbour, &cw, &ch) && cw == width && ch == height) {
                fixture->context[r][c] = neighbour;
            }
        }
    }

    unsigned ew = 0;
    unsigned eh = 0;
    if (lodepng::decode(fixture->expected, ew, eh, base + ".out.png", LCT_RGBA, 8) != 0 || ew != width * 2 || eh != height * 2) {
        fprintf(stderr, "%s: bad reference image\n", name.c_str());
        return false;
    }
    return true;
}

static std::vector<Fixture> loadFixtures()
{
    std::vector<Fixture> list;
    std::ifstream in(std::string(HD_UPSCALE_FIXTURES) + "/fixtures.txt");
    std::string name;
    int tile = 0;
    while (in >> name >> tile) {
        // The fixtures are frames of the game's art: they are not in the
        // repository, make_upscale_fixtures.py makes them from your copy.
        if (!std::ifstream(std::string(HD_UPSCALE_FIXTURES) + "/" + name + ".in.png")) {
            printf("hd_upscale_test: fixtures not found (run tools/hd/make_upscale_fixtures.py); skipped\n");
            return {};
        }
        Fixture fixture;
        if (loadFixture(name, tile != 0, &fixture)) {
            list.push_back(std::move(fixture));
        } else {
            fprintf(stderr, "cannot load fixture %s\n", name.c_str());
            gFailures++;
        }
    }
    CHECK(list.size() >= 3);
    return list;
}

// `precise`: the palette boost; the Python reference is 8-bit, so that
// output differs from it on purpose (reported, loosely checked).
static void testAgainstPython(const std::vector<Fixture>& fixtures, bool precise)
{
    printf("%s\n", precise ? "palette boost (precision 16) vs the 8-bit Python reference:" : "precision 8 vs the Python reference:");
    printf("%-20s %9s %8s %7s %9s %9s\n", "fixture", "size", "PSNR dB", "max err", "off>2 %", "alpha");
    double worstPsnr = 1e9;
    int worstMax = 0;
    for (const Fixture& fixture : fixtures) {
        HdImage image;
        HdTileContext context = fixture.tileContext();
        hdUpscaleScripted(fixture.indices.data(), fixture.width, fixture.height, fixture.width, fixture.palette, fixture.tile, &image,
            1, precise, &context);
        CHECK(image.width == static_cast<int>(fixture.width * 2));
        CHECK(image.height == static_cast<int>(fixture.height * 2));

        double sse = 0.0;
        size_t samples = 0;
        size_t alphaMismatch = 0;
        size_t off = 0;
        int maxError = 0;
        for (size_t i = 0; i < image.pixels.size(); i++) {
            uint32_t pixel = image.pixels[i];
            const unsigned char* ref = fixture.expected.data() + i * 4;
            int alpha = pixel >> 24;
            if (alpha != ref[3]) {
                alphaMismatch++;
                continue;
            }
            if (alpha == 0) {
                if (pixel != 0) {
                    alphaMismatch++;
                }
                continue;
            }
            int channels[3] = { static_cast<int>((pixel >> 16) & 0xFF), static_cast<int>((pixel >> 8) & 0xFF), static_cast<int>(pixel & 0xFF) };
            bool pixelOff = false;
            for (int c = 0; c < 3; c++) {
                int diff = channels[c] - ref[c];
                sse += static_cast<double>(diff) * diff;
                samples++;
                maxError = std::max(maxError, std::abs(diff));
                pixelOff = pixelOff || std::abs(diff) > 2;
            }
            off += pixelOff ? 1 : 0;
        }
        double mse = samples > 0 ? sse / samples : 0.0;
        double psnr = mse > 0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
        char size[32];
        snprintf(size, sizeof(size), "%ux%u", fixture.width, fixture.height);
        printf("%-20s %9s %8.2f %7d %9.3f %9zu\n", fixture.name.c_str(), size, psnr, maxError,
            samples > 0 ? 100.0 * off / (samples / 3) : 0.0, alphaMismatch);
        CHECK(alphaMismatch == 0);
        if (precise) {
            CHECK(psnr >= 28.0);
        } else {
            CHECK(psnr >= 40.0);
            CHECK(maxError <= 24);
        }
        worstPsnr = std::min(worstPsnr, psnr);
        worstMax = std::max(worstMax, maxError);
    }
    printf("worst: PSNR %.2f dB, max error %d\n", worstPsnr, worstMax);
}

static void testDeterministicAndKey(const std::vector<Fixture>& fixtures)
{
    const Fixture& fixture = fixtures.front();
    HdImage a;
    HdImage b;
    hdUpscaleScripted(fixture.indices.data(), fixture.width, fixture.height, fixture.width, fixture.palette, fixture.tile, &a);
    hdUpscaleScripted(fixture.indices.data(), fixture.width, fixture.height, fixture.width, fixture.palette, fixture.tile, &b);
    CHECK(a.pixels == b.pixels);

    uint64_t key = hdUpscaleKey(fixture.indices.data(), fixture.width, fixture.height, fixture.width, fixture.palette, fixture.tile);
    CHECK(key == hdUpscaleKey(fixture.indices.data(), fixture.width, fixture.height, fixture.width, fixture.palette, fixture.tile));
    CHECK(key != hdUpscaleKey(fixture.indices.data(), fixture.width, fixture.height, fixture.width, fixture.palette, !fixture.tile));
    unsigned char palette[256 * 3];
    memcpy(palette, fixture.palette, sizeof(palette));
    palette[30] ^= 1;
    CHECK(key != hdUpscaleKey(fixture.indices.data(), fixture.width, fixture.height, fixture.width, palette, fixture.tile));

    // A pitch wider than the width reads the same pixels.
    std::vector<unsigned char> wide(static_cast<size_t>(fixture.width + 7) * fixture.height, 0x55);
    for (unsigned y = 0; y < fixture.height; y++) {
        memcpy(wide.data() + y * (fixture.width + 7), fixture.indices.data() + y * fixture.width, fixture.width);
    }
    HdImage c;
    hdUpscaleScripted(wide.data(), fixture.width, fixture.height, fixture.width + 7, fixture.palette, fixture.tile, &c);
    CHECK(c.pixels == a.pixels);
    CHECK(key == hdUpscaleKey(wide.data(), fixture.width, fixture.height, fixture.width + 7, fixture.palette, fixture.tile));

    // Degenerate input: one pixel, all transparent, all opaque.
    unsigned char one = 5;
    HdImage d;
    hdUpscaleScripted(&one, 1, 1, 1, fixture.palette, false, &d);
    CHECK(d.width == 2 && d.height == 2 && (d.pixels[0] >> 24) == 0xFF);
    std::vector<unsigned char> clear(64, 0);
    hdUpscaleScripted(clear.data(), 8, 8, 8, fixture.palette, false, &d);
    bool allClear = true;
    for (uint32_t pixel : d.pixels) {
        allClear = allClear && pixel == 0;
    }
    CHECK(allClear);
}

// The palette boost on a synthetic banded gradient: grey ramp of 6-bit
// levels 10..41, each level a band 6 pixels wide. Precision 8 keeps the
// steps (one palette step, ~4 levels, at each band edge, partly split in
// two by the gradient restoration); precision 16 must give a monotonic ramp
// without band edges. A jump of several steps (an edge) stays
// sharp in both.
static void testBandedGradient()
{
    unsigned char palette[256 * 3] = {};
    for (int level = 0; level < 64; level++) {
        int index = 1 + level;
        unsigned char v = static_cast<unsigned char>((level << 2) | (level >> 4));
        palette[index * 3] = palette[index * 3 + 1] = palette[index * 3 + 2] = v;
    }
    const int bandWidth = 6;
    const int bands = 32;
    const int width = bandWidth * bands;
    const int height = 48;
    std::vector<unsigned char> indices(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            indices[static_cast<size_t>(y) * width + x] = static_cast<unsigned char>(1 + 10 + x / bandWidth);
        }
    }

    for (int mode = 0; mode < 2; mode++) {
        bool precise = mode == 1;
        HdImage image;
        hdUpscaleScripted(indices.data(), width, height, width, palette, false, &image, 1, precise);
        // Column profile over the middle rows (averages the dither out),
        // away from the image's left and right ends.
        std::vector<double> profile(image.width, 0.0);
        int rows = 0;
        for (int y = image.height / 4; y < image.height * 3 / 4; y++, rows++) {
            for (int x = 0; x < image.width; x++) {
                profile[x] += (image.pixels[static_cast<size_t>(y) * image.width + x] >> 8) & 0xFF;
            }
        }
        // Bins of 4 columns (the ordered dither's period); a band is 3 bins.
        double maxStep = 0.0;
        double minStep = 1e9;
        auto bin = [&](int b) {
            double sum = 0.0;
            for (int x = b * 4; x < b * 4 + 4; x++) {
                sum += profile[x];
            }
            return sum / (4.0 * rows);
        };
        int binsCount = image.width / 4;
        for (int b = 7; b < binsCount - 6; b++) {
            double step = bin(b) - bin(b - 1);
            maxStep = std::max(maxStep, step);
            minStep = std::min(minStep, step);
        }
        printf("banded gradient, precision %d: steps between 4-pixel bins %.2f .. %.2f levels (even ramp 1.35; a band edge is %.2f)\n",
            precise ? 16 : 8, minStep, maxStep, 255.0 / 63.0);
        if (precise) {
            CHECK(minStep >= 0.0); // monotonic
            // No band edge left (one is ~4.05 levels). Not perfectly even:
            // neighbours two palette steps away are an edge to the filter.
            CHECK(maxStep <= 2.5);
        } else {
            CHECK(maxStep >= 2.8); // the 8-bit path keeps visible steps
        }
    }

    // Edge: two flat areas 8 palette steps apart stay separated by a short
    // transition.
    std::vector<unsigned char> edge(static_cast<size_t>(64) * 32);
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 64; x++) {
            edge[static_cast<size_t>(y) * 64 + x] = static_cast<unsigned char>(x < 32 ? 1 + 20 : 1 + 28);
        }
    }
    HdImage image;
    hdUpscaleScripted(edge.data(), 64, 32, 64, palette, false, &image, 1, true);
    int lo = palette[(1 + 20) * 3];
    int hi = palette[(1 + 28) * 3];
    int mid = 32;
    int transition = 0;
    for (int x = 0; x < image.width; x++) {
        int v = (image.pixels[static_cast<size_t>(mid) * image.width + x] >> 8) & 0xFF;
        if (v > lo + 2 && v < hi - 2) {
            transition++;
        }
    }
    printf("edge of 8 palette steps, precision 16: %d transition pixels at 2x\n", transition);
    CHECK(transition <= 4);
}

// Large frames split into strips on several threads come out the same as in
// one piece.
static void testStrips(const std::vector<Fixture>& fixtures)
{
    for (const Fixture& fixture : fixtures) {
        if (fixture.tile) {
            continue;
        }
        // 3x3 copies of the fixture: large enough to be split.
        int width = fixture.width * 3;
        int height = fixture.height * 3;
        std::vector<unsigned char> big(static_cast<size_t>(width) * height);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                big[static_cast<size_t>(y) * width + x] = fixture.indices[(y % fixture.height) * fixture.width + (x % fixture.width)];
            }
        }
        HdImage whole;
        HdImage split;
        hdUpscaleScripted(big.data(), width, height, width, fixture.palette, false, &whole, 1, true);
        auto start = std::chrono::steady_clock::now();
        hdUpscaleScripted(big.data(), width, height, width, fixture.palette, false, &split, 8, true);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        CHECK(split.width == whole.width && split.height == whole.height);
        CHECK(split.pixels == whole.pixels);
        printf("strips: %s x9 (%dx%d) identical: %s, %.1f ms on 8 threads\n", fixture.name.c_str(), width, height,
            split.pixels == whole.pixels ? "yes" : "NO", ms);
    }
}

static void testDiskCache(const std::vector<Fixture>& fixtures)
{
    std::string dir = "hd_upscale_test_cache";
    const Fixture& fixture = fixtures.back();
    HdImage image;
    hdUpscaleScripted(fixture.indices.data(), fixture.width, fixture.height, fixture.width, fixture.palette, fixture.tile, &image);
    uint64_t key = hdUpscaleKey(fixture.indices.data(), fixture.width, fixture.height, fixture.width, fixture.palette, fixture.tile);
    CHECK(hdUpscaleCacheWrite(dir, key, image));
    HdImage back;
    CHECK(hdUpscaleCacheRead(dir, key, &back));
    CHECK(back.width == image.width && back.height == image.height && back.pixels == image.pixels);
    CHECK(!hdUpscaleCacheRead(dir, key ^ 1, &back));

    // Size cap: the least recently used files go first.
    std::vector<uint64_t> keys;
    for (int index = 0; index < 6; index++) {
        uint64_t extra = 0x1000 + index * 0x0100000000000000ull;
        CHECK(hdUpscaleCacheWrite(dir, extra, image));
        keys.push_back(extra);
    }
    // Older uses for all but the last (file times have 1 s resolution).
    for (size_t index = 0; index + 1 < keys.size(); index++) {
        char name[64];
        snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(keys[index]));
        std::string path = dir + "/" + std::string(name, 2) + "/" + name + ".rgba";
        struct utimbuf times;
        times.actime = times.modtime = time(nullptr) - 1000 + static_cast<time_t>(index);
        utime(path.c_str(), &times);
    }
    HdImage touched;
    unsigned long long fileSize = static_cast<unsigned long long>(image.pixels.size()) * 4 + 12;
    unsigned long long removed = 0;
    unsigned long long left = hdUpscaleCacheTrim(dir, fileSize * 4, &removed);
    CHECK(left <= fileSize * 4);
    CHECK(removed >= fileSize * 2);
    CHECK(hdUpscaleCacheRead(dir, keys.back(), &touched));
    printf("disk cache: 7 files, cap 4: %llu kept, %llu removed (bytes)\n", left, removed);

    // Clean up.
    keys.push_back(key);
    for (uint64_t each : keys) {
        char name[64];
        snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(each));
        std::string sub = dir + "/" + std::string(name, 2);
        remove((sub + "/" + name + ".rgba").c_str());
        rmdir(sub.c_str());
    }
    rmdir(dir.c_str());
}

static HdUpscaleJob makeJob(const Fixture& fixture, const std::string& key)
{
    HdUpscaleJob job;
    job.key = key;
    job.indices = fixture.indices;
    job.width = fixture.width;
    job.height = fixture.height;
    memcpy(job.palette8, fixture.palette, sizeof(job.palette8));
    job.tile = fixture.tile;
    job.precise = true;
    return job;
}

static void testPoolAndSpeed(const std::vector<Fixture>& fixtures)
{
    // Single thread, per output pixel.
    unsigned long long discard[kHdUpscaleStages];
    hdUpscaleStageTimes(discard, true);
    size_t pixels = 0;
    auto start = std::chrono::steady_clock::now();
    int rounds = 0;
    do {
        for (const Fixture& fixture : fixtures) {
            HdImage image;
            hdUpscaleScripted(fixture.indices.data(), fixture.width, fixture.height, fixture.width, fixture.palette, fixture.tile, &image,
                1, true);
            pixels += image.pixels.size();
        }
        rounds++;
    } while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < 1.0);
    double single = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / pixels;
    printf("single thread (precision 16): %.1f ns per output pixel (%d rounds)\n", single, rounds);
    {
        unsigned long long ns[kHdUpscaleStages];
        hdUpscaleStageTimes(ns);
        static const char* const kNames[kHdUpscaleStages] = { "prepare", "de-dither", "dcci", "restore", "de-band", "grain", "output" };
        printf("  per stage:");
        for (int stage = 0; stage < kHdUpscaleStages; stage++) {
            printf(" %s %.1f", kNames[stage], static_cast<double>(ns[stage]) / pixels);
        }
        printf(" ns/px\n");
    }
    {
        size_t pixels8 = 0;
        auto start8 = std::chrono::steady_clock::now();
        for (int round = 0; round < rounds; round++) {
            for (const Fixture& fixture : fixtures) {
                HdImage image;
                hdUpscaleScripted(fixture.indices.data(), fixture.width, fixture.height, fixture.width, fixture.palette, fixture.tile,
                    &image, 1, false);
                pixels8 += image.pixels.size();
            }
        }
        printf("single thread (precision 8): %.1f ns per output pixel\n",
            std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start8).count() / pixels8);
    }

    // Pool: the same frames many times over, all threads.
    HdUpscalePool pool;
    pool.start(0, std::string());
    CHECK(pool.running());
    std::vector<HdImage> reference;
    for (const Fixture& fixture : fixtures) {
        HdImage image;
        hdUpscaleScripted(fixture.indices.data(), fixture.width, fixture.height, fixture.width, fixture.palette, fixture.tile, &image,
            1, true);
        reference.push_back(std::move(image));
    }
    int copies = 12;
    size_t poolPixels = 0;
    start = std::chrono::steady_clock::now();
    for (int copy = 0; copy < copies; copy++) {
        for (size_t index = 0; index < fixtures.size(); index++) {
            pool.submit(makeJob(fixtures[index], std::to_string(copy) + ":" + fixtures[index].name));
        }
    }
    pool.waitIdle();
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    int matched = 0;
    for (int copy = 0; copy < copies; copy++) {
        for (size_t index = 0; index < fixtures.size(); index++) {
            std::shared_ptr<HdImage> image = pool.take(std::to_string(copy) + ":" + fixtures[index].name);
            CHECK(image != nullptr);
            if (image != nullptr) {
                poolPixels += image->pixels.size();
                matched += image->pixels == reference[index].pixels ? 1 : 0;
            }
        }
    }
    CHECK(matched == copies * static_cast<int>(fixtures.size()));
    printf("pool (%d threads, precision 16): %.1f ns per output pixel\n", pool.threadCount(), seconds * 1e9 / std::max<size_t>(poolPixels, 1));

    // take() of a queued job runs it on the caller; unknown keys give null.
    CHECK(pool.take("missing") == nullptr);
    pool.submit(makeJob(fixtures.front(), "again"));
    std::shared_ptr<HdImage> again = pool.take("again");
    CHECK(again != nullptr && again->pixels == reference.front().pixels);
    pool.stop();
    CHECK(!pool.running());
}

int main()
{
    std::vector<Fixture> fixtures = loadFixtures();
    if (!fixtures.empty()) {
        testAgainstPython(fixtures, false);
        testAgainstPython(fixtures, true);
        testBandedGradient();
        testDeterministicAndKey(fixtures);
        testStrips(fixtures);
        testDiskCache(fixtures);
        testPoolAndSpeed(fixtures);
    }

    if (gFailures != 0) {
        fprintf(stderr, "hd_upscale_test: %d check(s) failed\n", gFailures);
        return 1;
    }
    printf("hd_upscale_test: all checks passed\n");
    return 0;
}
