#include "hd_art.h"

#include <string.h>

#include <string>
#include <vector>

#include <lodepng.h>

#include "db.h"
#include "debug.h"
#include "settings.h"
#include "window_manager.h"

namespace fallout {

bool hdImageLoadPng(const char* path, HdImage* image)
{
    int size = 0;
    if (dbGetFileSize(path, &size) != 0 || size <= 0) {
        return false;
    }

    std::vector<unsigned char> encoded(size);
    if (dbGetFileContents(path, encoded.data()) != 0) {
        return false;
    }

    std::vector<unsigned char> rgba;
    unsigned width = 0;
    unsigned height = 0;
    unsigned error = lodepng::decode(rgba, width, height, encoded, LCT_RGBA, 8);
    if (error != 0) {
        debugPrint("HD: cannot decode %s: %s\n", path, lodepng_error_text(error));
        return false;
    }

    image->width = static_cast<int>(width);
    image->height = static_cast<int>(height);
    image->pixels.resize(static_cast<size_t>(width) * height);
    for (size_t i = 0; i < image->pixels.size(); i++) {
        const unsigned char* p = rgba.data() + i * 4;
        image->pixels[i] = (static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
    }
    return true;
}

bool hdArtDrawInterface(int win, const char* artPath, int x, int y, int width, int height)
{
    if (!windowHdIsActive() || settings.graphics.hd_pack.empty()) {
        return false;
    }

    std::string path = "hd/" + settings.graphics.hd_pack + "/" + artPath;
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) {
        path.replace(dot, std::string::npos, ".png");
    }

    HdImage image;
    if (!hdImageLoadPng(path.c_str(), &image)) {
        return false;
    }

    return windowHdDrawImage(win, x, y, width, height, image);
}

} // namespace fallout
