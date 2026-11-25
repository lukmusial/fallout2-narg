// Unit tests for HD art formats (src/hd_format.cc): .fhd container parsing,
// PNG decoding, art path normalisation and pack manifests. No game data.

#include "hd_format.h"

#include <stdio.h>
#include <string.h>

#include <lodepng.h>

using namespace fallout;

static int gFailures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                #condition);                                                 \
            gFailures++;                                                     \
        }                                                                    \
    } while (0)

static std::vector<unsigned char> encodePng(int width, int height, unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    std::vector<unsigned char> rgba(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = r;
        rgba[i + 1] = g;
        rgba[i + 2] = b;
        rgba[i + 3] = a;
    }
    std::vector<unsigned char> png;
    lodepng::encode(png, rgba, width, height, LCT_RGBA, 8);
    return png;
}

static void put16(std::vector<unsigned char>& out, size_t at, unsigned value)
{
    out[at] = value & 0xFF;
    out[at + 1] = (value >> 8) & 0xFF;
}

static void put32(std::vector<unsigned char>& out, size_t at, unsigned value)
{
    for (int i = 0; i < 4; i++) {
        out[at + i] = (value >> (8 * i)) & 0xFF;
    }
}

// Builds an .fhd with `directions` x `frames` entries; `pngs[i]` empty = no image.
static std::vector<unsigned char> buildFhd(int directions, int frames, const std::vector<std::vector<unsigned char>>& pngs)
{
    size_t count = static_cast<size_t>(directions) * frames;
    std::vector<unsigned char> out(12 + count * 8, 0);
    memcpy(out.data(), "FHD1", 4);
    put16(out, 4, directions);
    put16(out, 6, frames);
    put32(out, 8, 0);
    for (size_t i = 0; i < count; i++) {
        if (pngs[i].empty()) {
            continue;
        }
        put32(out, 12 + i * 8, static_cast<unsigned>(out.size()));
        put32(out, 12 + i * 8 + 4, static_cast<unsigned>(pngs[i].size()));
        out.insert(out.end(), pngs[i].begin(), pngs[i].end());
    }
    return out;
}

static void testFhd()
{
    std::vector<std::vector<unsigned char>> pngs = {
        encodePng(4, 2, 255, 0, 0, 255), // dir 0 frame 0
        {}, // dir 0 frame 1: no HD image
        encodePng(2, 2, 0, 0, 255, 128), // dir 1 frame 0
        encodePng(2, 2, 0, 255, 0, 255), // dir 1 frame 1
    };
    std::vector<unsigned char> file = buildFhd(2, 2, pngs);

    HdFhdInfo info;
    std::string error;
    CHECK(hdFhdParse(file.data(), file.size(), &info, &error));
    CHECK(info.directions == 2 && info.frames == 2);
    CHECK(info.find(1, 0) == nullptr);
    CHECK(info.find(2, 0) == nullptr);
    CHECK(info.find(0, 2) == nullptr);

    const HdFhdEntry* entry = info.find(1, 1);
    CHECK(entry != nullptr);
    if (entry != nullptr) {
        HdImage image;
        CHECK(hdImageDecodePng(file.data() + entry->offset, entry->size, &image));
        CHECK(image.width == 2 && image.height == 2);
        CHECK(image.pixels[0] == 0xFF00FF00);
    }

    entry = info.find(0, 1);
    CHECK(entry != nullptr);
    if (entry != nullptr) {
        HdImage image;
        CHECK(hdImageDecodePng(file.data() + entry->offset, entry->size, &image));
        CHECK(image.pixels[3] == 0x800000FF); // straight alpha kept
    }

    // A single-direction file serves every direction.
    std::vector<unsigned char> single = buildFhd(1, 1, { encodePng(2, 2, 1, 2, 3, 255) });
    CHECK(hdFhdParse(single.data(), single.size(), &info));
    CHECK(info.find(0, 4) != nullptr);

    // Malformed files are rejected.
    std::vector<unsigned char> bad = file;
    bad[0] = 'X';
    CHECK(!hdFhdParse(bad.data(), bad.size(), &info, &error));
    bad = file;
    put32(bad, 12 + 8 + 0, 0x7FFFFFF0); // dir 0 frame 1 points outside... (size 0 entry is ignored)
    put32(bad, 12 + 0, 0x7FFFFFF0); // dir 0 frame 0 offset outside the file
    CHECK(!hdFhdParse(bad.data(), bad.size(), &info, &error));
    CHECK(!hdFhdParse(file.data(), 20, &info, &error)); // truncated table
    bad = file;
    put16(bad, 4, 7); // too many directions
    CHECK(!hdFhdParse(bad.data(), bad.size(), &info, &error));
}

static void testPngColourTypes()
{
    // Grey + palette PNGs decode to RGBA too.
    std::vector<unsigned char> grey = { 0, 128, 255, 64 };
    std::vector<unsigned char> png;
    CHECK(lodepng::encode(png, grey, 2, 2, LCT_GREY, 8) == 0);
    HdImage image;
    CHECK(hdImageDecodePng(png.data(), png.size(), &image));
    CHECK(image.pixels[1] == 0xFF808080);

    std::string error;
    unsigned char junk[16] = { 0 };
    CHECK(!hdImageDecodePng(junk, sizeof(junk), &image, &error));
    CHECK(!error.empty());
}

static void testPaths()
{
    CHECK(hdArtNormalizePath("art\\intrface\\MAINMENU.FRM") == "art/intrface/mainmenu.frm");
    CHECK(hdArtNormalizePath("ART\\CRITTERS\\HMJMPSAA.FR3") == "art/critters/hmjmpsaa.fr3");
    CHECK(hdArtNormalizePath("data/art/tiles/edg1000.frm") == "art/tiles/edg1000.frm");
    CHECK(hdArtNormalizePath("./art/misc/x.frm") == "art/misc/x.frm");
    CHECK(hdArtNormalizePath("cart/x.frm") == "cart/x.frm");
}

static void testManifest()
{
    std::unordered_map<std::string, std::string> entries;
    std::string fallback;
    hdManifestParse("# comment\n* = scripted\nART\\INTRFACE\\MAINMENU.FRM = ai  # inline\n\nbroken line\n art/tiles/a.frm=ai\r\n= nothing\n", &entries, &fallback);
    CHECK(fallback == "scripted");
    CHECK(entries.size() == 2);
    CHECK(entries["art/intrface/mainmenu.frm"] == "ai");
    CHECK(entries["art/tiles/a.frm"] == "ai");
}

static void testLegacy()
{
    // Reference values from tools/hd/legacy_art.py (frame_hash).
    unsigned char four[4] = { 1, 2, 3, 4 };
    CHECK(hdLegacyFrameHash(four, 2, 2, 2) == 0x9ee5a5d615aca5fdULL);
    CHECK(hdLegacyFrameHash(nullptr, 0, 0, 0) == 0x4d25767f9dce13f5ULL);
    std::vector<unsigned char> wide(300, 0);
    for (int i = 0; i < 256; i++) {
        wide[i] = static_cast<unsigned char>(i);
    }
    CHECK(hdLegacyFrameHash(wide.data(), 300, 1, 300) == 0xe0ed46c1d8fb5be7ULL);

    // The pitch skips the padding after each row.
    unsigned char padded[6] = { 1, 2, 99, 3, 4, 98 };
    CHECK(hdLegacyFrameHash(padded, 2, 2, 3) == 0x9ee5a5d615aca5fdULL);
    // Width and height are part of the hash.
    CHECK(hdLegacyFrameHash(four, 4, 1, 4) != hdLegacyFrameHash(four, 2, 2, 2));

    CHECK(hdLegacyKey("art/tiles/a.frm", 2, 7) == "art/tiles/a.frm#2/7");

    std::unordered_map<std::string, uint64_t> entries;
    size_t count = hdLegacyParse("# header\n"
                                 "ART\\TILES\\EDG5000.FRM 0 0 9ee5a5d615aca5fd\n"
                                 "art/items/chest1.frm 0 3 00000000000000ff\r\n"
                                 "art/x.frm 0 0 123\n" // short hash
                                 "art/x.frm 7 0 0123456789abcdef\n" // bad direction
                                 "art/x.frm 0 0 0123456789abcdeg\n" // not hex
                                 "broken\n",
        &entries);
    CHECK(count == 2);
    CHECK(entries.size() == 2);
    CHECK(entries["art/tiles/edg5000.frm#0/0"] == 0x9ee5a5d615aca5fdULL);
    CHECK(entries["art/items/chest1.frm#0/3"] == 0xffULL);
}

int main()
{
    testFhd();
    testPngColourTypes();
    testPaths();
    testManifest();
    testLegacy();

    if (gFailures != 0) {
        fprintf(stderr, "hd_format_test: %d check(s) failed\n", gFailures);
        return 1;
    }
    printf("hd_format_test: all checks passed\n");
    return 0;
}
