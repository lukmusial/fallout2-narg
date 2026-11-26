#include "iface_bar_art.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "art.h"
#include "db.h"
#include "debug.h"
#include "game_config.h"
#include "memory.h"
#include "platform_compat.h"
#include "settings.h"

namespace fallout {

namespace {

    constexpr int kIfaceWidth = 640;

    // The Russian redraw of HR_IFACE_800.FRM bundled in f2_res.dat (size and
    // FNV-1a 64 of the file).
    constexpr int kBundledRussianSize = 79274;
    constexpr uint64_t kBundledRussianHash = 0xca4a7401fee7332fULL;

    uint64_t fnv1a64(const unsigned char* data, size_t size)
    {
        uint64_t hash = 0xcbf29ce484222325ULL;
        for (size_t index = 0; index < size; index++) {
            hash = (hash ^ data[index]) * 0x100000001b3ULL;
        }
        return hash;
    }

    // The bar width named by `path` (HR_IFACE_<width>.FRM, any case or
    // separator), or 0.
    int wideBarWidth(const char* path)
    {
        const char* fileName = path;
        for (const char* ch = path; *ch != '\0'; ch++) {
            if (*ch == '\\' || *ch == '/') {
                fileName = ch + 1;
            }
        }

        constexpr char kPrefix[] = "hr_iface_";
        constexpr size_t kPrefixLength = sizeof(kPrefix) - 1;
        if (strlen(fileName) <= kPrefixLength || compat_strnicmp(fileName, kPrefix, kPrefixLength) != 0) {
            return 0;
        }

        int width = 0;
        int length = 0;
        if (sscanf(fileName + kPrefixLength, "%d%n", &width, &length) != 1) {
            return 0;
        }
        if (compat_stricmp(fileName + kPrefixLength + length, ".frm") != 0) {
            return 0;
        }
        return width;
    }

    // Whether the game's own file for `path` is missing or the bundled Russian
    // redraw.
    bool shouldBuild(const char* path)
    {
        int size = 0;
        if (dbGetFileSize(path, &size) != 0 || size <= 0) {
            return true;
        }
        if (size != kBundledRussianSize) {
            return false;
        }
        std::vector<unsigned char> data(size);
        if (dbGetFileContents(path, data.data()) != 0) {
            return false;
        }
        return fnv1a64(data.data(), data.size()) == kBundledRussianHash;
    }

    // The 640-pixel bar the game shows: the localized one when there is one.
    Art* loadIface()
    {
        const std::string& language = settings.system.language;
        if (!language.empty() && compat_stricmp(language.c_str(), ENGLISH) != 0) {
            std::string localized = "art\\" + language + "\\intrface\\iface.frm";
            int size = 0;
            if (dbGetFileSize(localized.c_str(), &size) == 0 && size > 0) {
                Art* art = artLoad(localized.c_str());
                if (art != nullptr) {
                    return art;
                }
            }
        }
        return artLoad("art\\intrface\\iface.frm");
    }

    // A one-frame Art as artLoad returns for a one-frame FRM (header as
    // ce-frm2png writes it: version 4, 10 fps, no offsets).
    Art* allocateSingleFrame(int width, int height, const std::vector<unsigned char>& pixels)
    {
        const size_t pixelCount = static_cast<size_t>(width) * height;
        const int padding = static_cast<int>((sizeof(int) - sizeof(Art) % sizeof(int)) % sizeof(int));

        Art header = {};
        header.version = 4;
        header.framesPerSecond = 10;
        header.actionFrame = 0;
        header.frameCount = 1;
        header.dataSize = static_cast<int>(sizeof(ArtFrame) + pixelCount);
        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            header.dataOffsets[rotation] = 0;
            header.padding[rotation] = padding;
        }

        // As artGetDataSize: room for the worst-case frame padding.
        const size_t dataSize = sizeof(Art) + header.dataSize + (sizeof(int) - 1);
        unsigned char* data = reinterpret_cast<unsigned char*>(internal_malloc(dataSize));
        if (data == nullptr) {
            return nullptr;
        }
        memset(data, 0, dataSize);
        Art* art = reinterpret_cast<Art*>(data);
        *art = header;

        ArtFrame* frame = reinterpret_cast<ArtFrame*>(data + sizeof(Art) + padding);
        frame->width = static_cast<short>(width);
        frame->height = static_cast<short>(height);
        frame->size = static_cast<int>(pixelCount);
        frame->x = 0;
        frame->y = 0;
        memcpy(reinterpret_cast<unsigned char*>(frame) + sizeof(ArtFrame), pixels.data(), pixelCount);
        return art;
    }

} // namespace

Art* interfaceBarArtLoad(const char* path)
{
    if (path == nullptr) {
        return nullptr;
    }
    const int width = wideBarWidth(path);
    if (width <= kIfaceWidth || !shouldBuild(path)) {
        return nullptr;
    }

    Art* iface = loadIface();
    if (iface == nullptr) {
        return nullptr;
    }

    int ifaceWidth = 0;
    int height = 0;
    const unsigned char* data = artGetFrameData(iface, 0, ROTATION_NE, &ifaceWidth, &height, nullptr, nullptr);
    std::vector<unsigned char> pixels;
    Art* art = nullptr;
    if (data != nullptr && interfaceBarArtBuild(data, ifaceWidth, height, width, pixels)) {
        art = allocateSingleFrame(width, height, pixels);
    }
    internal_free(iface);

    if (art != nullptr) {
        debugPrint("INTRFACE: %s built from the game's iface.frm\n", path);
    }
    return art;
}

} // namespace fallout
