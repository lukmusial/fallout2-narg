#include "hd_format.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lodepng.h>

namespace fallout {

static uint32_t readU32(const unsigned char* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

static uint16_t readU16(const unsigned char* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

const HdFhdEntry* HdFhdInfo::find(int frame, int direction) const
{
    if (directions == 1) {
        direction = 0;
    }

    if (frame < 0 || frame >= frames || direction < 0 || direction >= directions) {
        return nullptr;
    }

    const HdFhdEntry* entry = &entries[static_cast<size_t>(direction) * frames + frame];
    return entry->size != 0 ? entry : nullptr;
}

static bool fail(std::string* error, const char* message)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool hdFhdParse(const unsigned char* data, size_t size, HdFhdInfo* info, std::string* error)
{
    constexpr size_t kHeaderSize = 12;
    if (size < kHeaderSize || memcmp(data, "FHD1", 4) != 0) {
        return fail(error, "not an FHD1 file");
    }

    int directions = readU16(data + 4);
    int frames = readU16(data + 6);
    uint32_t flags = readU32(data + 8);
    if (directions < 1 || directions > kHdFhdMaxDirections || frames < 1) {
        return fail(error, "bad direction or frame count");
    }
    if (flags != 0) {
        return fail(error, "unsupported flags");
    }

    size_t count = static_cast<size_t>(directions) * frames;
    if (size < kHeaderSize + count * 8) {
        return fail(error, "truncated entry table");
    }

    info->directions = directions;
    info->frames = frames;
    info->entries.resize(count);
    for (size_t index = 0; index < count; index++) {
        const unsigned char* p = data + kHeaderSize + index * 8;
        HdFhdEntry& entry = info->entries[index];
        entry.offset = readU32(p);
        entry.size = readU32(p + 4);
        if (entry.size != 0 && (entry.offset > size || entry.size > size - entry.offset)) {
            return fail(error, "entry outside the file");
        }
    }

    return true;
}

std::string hdArtNormalizePath(const char* artPath)
{
    std::string path = artPath != nullptr ? artPath : "";
    for (char& ch : path) {
        ch = ch == '\\' ? '/' : static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    }

    size_t art = path.rfind("art/");
    if (art != std::string::npos && (art == 0 || path[art - 1] == '/')) {
        path.erase(0, art);
    }
    while (path.compare(0, 2, "./") == 0) {
        path.erase(0, 2);
    }
    return path;
}

static std::string trim(const std::string& value)
{
    size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return std::string();
    }
    size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

void hdManifestParse(const std::string& text, std::unordered_map<std::string, std::string>* entries, std::string* defaultPack)
{
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        start = end + 1;

        size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line.resize(comment);
        }
        size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }

        std::string key = trim(line.substr(0, equals));
        std::string value = trim(line.substr(equals + 1));
        if (key.empty() || value.empty()) {
            continue;
        }
        if (key == "*") {
            *defaultPack = value;
        } else {
            (*entries)[hdArtNormalizePath(key.c_str())] = value;
        }
    }
}

std::vector<std::string> hdPackList(const std::string& value)
{
    std::vector<std::string> packs;
    size_t start = 0;
    while (start < value.size()) {
        size_t end = value.find_first_of(" \t\r\n,", start);
        if (end == std::string::npos) {
            end = value.size();
        }
        if (end > start) {
            packs.push_back(value.substr(start, end - start));
        }
        start = end + 1;
    }
    return packs;
}

bool hdImageDecodePng(const unsigned char* data, size_t size, HdImage* image, std::string* error)
{
    std::vector<unsigned char> rgba;
    unsigned width = 0;
    unsigned height = 0;
    unsigned code = lodepng::decode(rgba, width, height, data, size, LCT_RGBA, 8);
    if (code != 0) {
        return fail(error, lodepng_error_text(code));
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

uint64_t hdLegacyFrameHash(const unsigned char* pixels, int width, int height, int pitch)
{
    constexpr uint64_t kPrime = 0x100000001B3ULL;
    uint64_t hash = 0xCBF29CE484222325ULL;
    auto add = [&hash](unsigned char byte) {
        hash = (hash ^ byte) * kPrime;
    };

    add(static_cast<unsigned char>(width & 0xFF));
    add(static_cast<unsigned char>((width >> 8) & 0xFF));
    add(static_cast<unsigned char>(height & 0xFF));
    add(static_cast<unsigned char>((height >> 8) & 0xFF));
    if (pixels == nullptr) {
        return hash;
    }
    for (int y = 0; y < height; y++) {
        const unsigned char* row = pixels + static_cast<size_t>(y) * pitch;
        for (int x = 0; x < width; x++) {
            add(row[x]);
        }
    }
    return hash;
}

std::string hdLegacyKey(const std::string& artPath, int direction, int frame)
{
    return artPath + "#" + std::to_string(direction) + "/" + std::to_string(frame);
}

size_t hdLegacyParse(const std::string& text, std::unordered_map<std::string, uint64_t>* entries)
{
    size_t count = 0;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        start = end + 1;

        size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line.resize(comment);
        }

        // The last three fields are numbers; the path is everything before
        // them (art names may contain spaces, e.g. RPU's "rifle bb.frm").
        std::string fields[3];
        line = trim(line);
        for (int index = 2; index >= 0; index--) {
            size_t space = line.find_last_of(" \t");
            if (space == std::string::npos) {
                line.clear();
                break;
            }
            fields[index] = line.substr(space + 1);
            line = trim(line.substr(0, space));
        }
        std::string path = line;
        int direction = 0;
        int frame = 0;
        char hex[32];
        char extra = 0;
        if (path.empty()
            || sscanf(fields[0].c_str(), "%d%c", &direction, &extra) != 1
            || sscanf(fields[1].c_str(), "%d%c", &frame, &extra) != 1
            || fields[2].size() >= sizeof(hex)) {
            continue;
        }
        strcpy(hex, fields[2].c_str());
        if (direction < 0 || direction >= kHdFhdMaxDirections || frame < 0 || strlen(hex) != 16) {
            continue;
        }
        char* parsedEnd = nullptr;
        uint64_t hash = strtoull(hex, &parsedEnd, 16);
        if (parsedEnd == nullptr || *parsedEnd != '\0') {
            continue;
        }

        (*entries)[hdLegacyKey(hdArtNormalizePath(path.c_str()), direction, frame)] = hash;
        count++;
    }
    return count;
}

} // namespace fallout
