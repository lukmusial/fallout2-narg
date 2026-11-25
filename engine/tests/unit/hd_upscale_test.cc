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
};

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

static void testAgainstPython(const std::vector<Fixture>& fixtures)
{
    printf("%-20s %9s %8s %7s %9s %9s\n", "fixture", "size", "PSNR dB", "max err", "off>2 %", "alpha");
    double worstPsnr = 1e9;
    int worstMax = 0;
    for (const Fixture& fixture : fixtures) {
        HdImage image;
        hdUpscaleScripted(fixture.indices.data(), fixture.width, fixture.height, fixture.width, fixture.palette, fixture.tile, &image);
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
        CHECK(psnr >= 40.0);
        CHECK(maxError <= 24);
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
    return job;
}

static void testPoolAndSpeed(const std::vector<Fixture>& fixtures)
{
    // Single thread, per output pixel.
    size_t pixels = 0;
    auto start = std::chrono::steady_clock::now();
    int rounds = 0;
    do {
        for (const Fixture& fixture : fixtures) {
            HdImage image;
            hdUpscaleScripted(fixture.indices.data(), fixture.width, fixture.height, fixture.width, fixture.palette, fixture.tile, &image);
            pixels += image.pixels.size();
        }
        rounds++;
    } while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < 1.0);
    double single = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / pixels;
    printf("single thread: %.1f ns per output pixel (%d rounds)\n", single, rounds);

    // Pool: the same frames many times over, all threads.
    HdUpscalePool pool;
    pool.start(0, std::string());
    CHECK(pool.running());
    std::vector<HdImage> reference;
    for (const Fixture& fixture : fixtures) {
        HdImage image;
        hdUpscaleScripted(fixture.indices.data(), fixture.width, fixture.height, fixture.width, fixture.palette, fixture.tile, &image);
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
    printf("pool (%d threads): %.1f ns per output pixel\n", pool.threadCount(), seconds * 1e9 / std::max<size_t>(poolPixels, 1));

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
        testAgainstPython(fixtures);
        testDeterministicAndKey(fixtures);
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
