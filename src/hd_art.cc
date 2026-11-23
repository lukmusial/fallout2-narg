#include "hd_art.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <list>
#include <unordered_map>
#include <vector>

#include "art.h"
#include "color.h"
#include "db.h"
#include "debug.h"
#include "hd_format.h"
#include "settings.h"
#include "window_manager.h"

namespace fallout {

namespace {

    enum class AssetKind {
        None,
        Png,
        Fhd,
    };

    struct Asset {
        AssetKind kind = AssetKind::None;
        std::string file; // resolved HD file path
        HdFhdInfo fhd;
    };

    struct CachedFrame {
        std::string key;
        std::shared_ptr<const HdImage> image;
        size_t bytes;
    };

    struct HdArtState {
        bool initialized = false;
        std::string pack;

        // Manifest of the active pack: art path -> pack ("*" = default).
        std::unordered_map<std::string, std::string> manifest;
        std::string manifestDefault;

        std::unordered_map<std::string, Asset> assets;

        std::list<CachedFrame> lru; // most recently used first
        std::unordered_map<std::string, std::list<CachedFrame>::iterator> frames;
        size_t bytes = 0;

        unsigned long long hits = 0;
        unsigned long long misses = 0;
    };

    HdArtState gHdArt;

    // .dat archives (ce.dat, master.dat) store names with backslashes; loose
    // files accept either, so look everything up in the engine's own form.
    std::string dbPath(const std::string& path)
    {
        std::string result = path;
        std::replace(result.begin(), result.end(), '/', '\\');
        return result;
    }

    bool readWholeFile(const std::string& path, std::vector<unsigned char>& data)
    {
        std::string name = dbPath(path);
        int size = 0;
        if (dbGetFileSize(name.c_str(), &size) != 0 || size <= 0) {
            return false;
        }
        data.resize(size);
        return dbGetFileContents(name.c_str(), data.data()) == 0;
    }

    bool fileExists(const std::string& rawPath)
    {
        std::string path = dbPath(rawPath);
        int size = 0;
        return dbGetFileSize(path.c_str(), &size) == 0 && size > 0;
    }

    void loadManifest()
    {
        gHdArt.manifest.clear();
        gHdArt.manifestDefault.clear();
        if (gHdArt.pack.empty()) {
            return;
        }

        std::vector<unsigned char> data;
        if (!readWholeFile("hd/" + gHdArt.pack + "/manifest.txt", data)) {
            return;
        }

        std::string text(data.begin(), data.end());
        hdManifestParse(text, &gHdArt.manifest, &gHdArt.manifestDefault);

        debugPrint("HD: pack %s manifest: %zu entries, default %s\n", gHdArt.pack.c_str(), gHdArt.manifest.size(), gHdArt.manifestDefault.c_str());
    }

    void clearCache()
    {
        gHdArt.assets.clear();
        gHdArt.lru.clear();
        gHdArt.frames.clear();
        gHdArt.bytes = 0;
    }

    void ensureInitialized()
    {
        if (!gHdArt.initialized) {
            gHdArt.initialized = true;
            gHdArt.pack = settings.graphics.hd_pack;
            loadManifest();
        }
    }

    const std::string& packForAsset(const std::string& key)
    {
        auto it = gHdArt.manifest.find(key);
        if (it != gHdArt.manifest.end()) {
            return it->second;
        }
        if (!gHdArt.manifestDefault.empty()) {
            return gHdArt.manifestDefault;
        }
        return gHdArt.pack;
    }

    const Asset& resolveAsset(const std::string& key)
    {
        auto found = gHdArt.assets.find(key);
        if (found != gHdArt.assets.end()) {
            return found->second;
        }

        Asset asset;
        if (!gHdArt.pack.empty()) {
            std::string base = "hd/" + packForAsset(key) + "/" + key;
            size_t dot = base.find_last_of('.');
            if (dot != std::string::npos && base.find('/', dot) == std::string::npos) {
                base.resize(dot);
            }

            std::vector<unsigned char> data;
            std::string error;
            if (readWholeFile(base + ".fhd", data)) {
                // The table is small; frames are read on demand.
                if (hdFhdParse(data.data(), data.size(), &asset.fhd, &error)) {
                    asset.kind = AssetKind::Fhd;
                    asset.file = base + ".fhd";
                } else {
                    debugPrint("HD: %s.fhd: %s\n", base.c_str(), error.c_str());
                }
            }

            if (asset.kind == AssetKind::None && fileExists(base + ".png")) {
                asset.kind = AssetKind::Png;
                asset.file = base + ".png";
            }
        }

        return gHdArt.assets.emplace(key, std::move(asset)).first->second;
    }

    std::shared_ptr<const HdImage> cacheGet(const std::string& key)
    {
        auto it = gHdArt.frames.find(key);
        if (it == gHdArt.frames.end()) {
            gHdArt.misses++;
            return nullptr;
        }

        gHdArt.hits++;
        gHdArt.lru.splice(gHdArt.lru.begin(), gHdArt.lru, it->second);
        return it->second->image;
    }

    void cachePut(const std::string& key, std::shared_ptr<const HdImage> image)
    {
        size_t bytes = image->pixels.size() * sizeof(uint32_t) + sizeof(HdImage);
        gHdArt.lru.push_front({ key, std::move(image), bytes });
        gHdArt.frames[key] = gHdArt.lru.begin();
        gHdArt.bytes += bytes;

        size_t budget = static_cast<size_t>(std::max(settings.graphics.hd_cache_mb, 1)) * 1024 * 1024;
        while (gHdArt.bytes > budget && gHdArt.lru.size() > 1) {
            CachedFrame& last = gHdArt.lru.back();
            gHdArt.bytes -= last.bytes;
            gHdArt.frames.erase(last.key);
            gHdArt.lru.pop_back();
        }
    }

    bool loadAssetFrame(const Asset& asset, int frame, int direction, HdImage* image)
    {
        std::string error;
        if (asset.kind == AssetKind::Png) {
            if (frame != 0 || direction != 0) {
                return false;
            }
            std::vector<unsigned char> data;
            if (!readWholeFile(asset.file, data)) {
                return false;
            }
            if (!hdImageDecodePng(data.data(), data.size(), image, &error)) {
                debugPrint("HD: %s: %s\n", asset.file.c_str(), error.c_str());
                return false;
            }
            return true;
        }

        if (asset.kind == AssetKind::Fhd) {
            const HdFhdEntry* entry = asset.fhd.find(frame, direction);
            if (entry == nullptr) {
                return false;
            }

            File* stream = fileOpen(asset.file.c_str(), "rb");
            if (stream == nullptr) {
                return false;
            }
            std::vector<unsigned char> data(entry->size);
            bool ok = fileSeek(stream, static_cast<long>(entry->offset), SEEK_SET) == 0
                && fileRead(data.data(), 1, data.size(), stream) == data.size();
            fileClose(stream);

            if (!ok || !hdImageDecodePng(data.data(), data.size(), image, &error)) {
                debugPrint("HD: %s frame %d/%d: %s\n", asset.file.c_str(), direction, frame, ok ? error.c_str() : "read error");
                return false;
            }
            return true;
        }

        return false;
    }

    std::string frameKey(const char* prefix, const std::string& path, int frame, int direction)
    {
        return std::string(prefix) + path + "#" + std::to_string(direction) + "/" + std::to_string(frame);
    }

} // namespace

void hdArtSetPack(const char* pack)
{
    gHdArt.initialized = true;
    gHdArt.pack = pack != nullptr ? pack : "";
    clearCache();
    loadManifest();
    debugPrint("HD: pack set to '%s'\n", gHdArt.pack.c_str());
}

const std::string& hdArtGetPack()
{
    ensureInitialized();
    return gHdArt.pack;
}

std::shared_ptr<const HdImage> hdArtFindFrame(const char* artPath, int frame, int direction)
{
    ensureInitialized();
    if (gHdArt.pack.empty()) {
        return nullptr;
    }

    std::string path = hdArtNormalizePath(artPath);
    const Asset& asset = resolveAsset(path);
    if (asset.kind == AssetKind::None) {
        return nullptr;
    }

    std::string key = frameKey("hd:", path, frame, direction);
    std::shared_ptr<const HdImage> image = cacheGet(key);
    if (image != nullptr) {
        return image;
    }

    auto loaded = std::make_shared<HdImage>();
    if (!loadAssetFrame(asset, frame, direction, loaded.get())) {
        return nullptr;
    }

    cachePut(key, loaded);
    return loaded;
}

std::shared_ptr<const HdImage> hdArtGetFrame(const char* artPath, int frame, int direction,
    const unsigned char* legacyData, int width, int height, int pitch, bool* fromLegacy)
{
    std::shared_ptr<const HdImage> image = hdArtFindFrame(artPath, frame, direction);
    if (fromLegacy != nullptr) {
        *fromLegacy = image == nullptr;
    }
    if (image != nullptr || legacyData == nullptr || width <= 0 || height <= 0) {
        return image;
    }

    std::string key = frameKey("legacy:", hdArtNormalizePath(artPath), frame, direction);
    image = cacheGet(key);
    if (image != nullptr) {
        return image;
    }

    unsigned char palette8[256 * 3];
    for (int index = 0; index < 256 * 3; index++) {
        palette8[index] = colorExpand6(_cmap[index]);
    }

    auto expanded = std::make_shared<HdImage>();
    hdImageFromIndexed(legacyData, width, height, pitch, palette8, true, expanded.get());
    cachePut(key, expanded);
    return expanded;
}

bool hdArtDrawInterface(int win, const char* artPath, int x, int y, int width, int height)
{
    if (!windowHdIsActive()) {
        return false;
    }

    std::shared_ptr<const HdImage> image = hdArtFindFrame(artPath, 0, 0);
    if (image == nullptr) {
        return false;
    }

    return windowHdDrawImage(win, x, y, width, height, *image);
}

bool hdArtDrawFrame(int win, const FrmId& frmId, int frame, int direction, int x, int y, int width, int height,
    const unsigned char* legacyData, int legacyPitch, bool allowFallback)
{
    if (!windowHdIsActive() || !frmId.valid()) {
        return false;
    }

    const char* path = frmId.filePath();
    if (path == nullptr) {
        return false;
    }

    std::shared_ptr<const HdImage> image = allowFallback
        ? hdArtGetFrame(path, frame, direction, legacyData, width, height, legacyPitch)
        : hdArtFindFrame(path, frame, direction);
    if (image == nullptr) {
        return false;
    }

    return windowHdDrawImage(win, x, y, width, height, *image);
}

std::shared_ptr<const HdImage> hdArtFindImage(const FrmImage& image)
{
    if (!image.isLocked()) {
        return nullptr;
    }

    std::string path = image.getPath();
    if (path.empty()) {
        return nullptr;
    }

    return hdArtFindFrame(path.c_str(), image.getFrame(), 0);
}

bool hdArtDrawImage(int win, const FrmImage& image, int x, int y)
{
    return hdArtDrawImageRegion(win, image, 0, 0, image.getWidth(), image.getHeight(), x, y);
}

bool hdArtDrawImageRegion(int win, const FrmImage& image, int srcX, int srcY, int width, int height, int destX, int destY)
{
    if (!windowHdIsActive()) {
        return false;
    }

    std::shared_ptr<const HdImage> hd = hdArtFindImage(image);
    if (hd == nullptr) {
        return false;
    }

    return hdArtDrawRegion(win, *hd, image.getData(), image.getWidth(), image.getHeight(), srcX, srcY, width, height, destX, destY);
}

bool hdArtDrawRegion(int win, const HdImage& hd, const unsigned char* source, int sourceWidth, int sourceHeight,
    int srcX, int srcY, int width, int height, int destX, int destY)
{
    if (!windowHdIsActive() || source == nullptr || sourceWidth <= 0 || sourceHeight <= 0) {
        return false;
    }

    // The whole image is placed so that (srcX, srcY) lands on (destX, destY),
    // then clipped to the region.
    int originX = destX - srcX;
    int originY = destY - srcY;
    HdDrawOptions options;
    options.clipLeft = destX;
    options.clipTop = destY;
    options.clipRight = destX + width;
    options.clipBottom = destY + height;
    options.source = source;
    options.sourcePitch = sourceWidth;
    return windowHdDrawImageEx(win, originX, originY, sourceWidth, sourceHeight, hd, options);
}

int hdArtSetButtonArt(int btn, const FrmImage* normal, const FrmImage* pressed, const FrmImage* hover)
{
    std::string paths[3];
    const unsigned char* sources[3] = {};
    const FrmImage* images[3] = { normal, pressed, hover };
    for (int slot = 0; slot < 3; slot++) {
        if (images[slot] != nullptr && images[slot]->isLocked()) {
            paths[slot] = images[slot]->getPath();
            sources[slot] = images[slot]->getData();
        }
    }

    return buttonSetHdArt(btn,
        paths[0].empty() ? nullptr : paths[0].c_str(),
        sources[0],
        paths[1].empty() ? nullptr : paths[1].c_str(),
        sources[1],
        paths[2].empty() ? nullptr : paths[2].c_str(),
        sources[2]);
}

void hdArtAttachButtonArt(int win, const FrmImage* images, int count, unsigned char* const* copies)
{
    Window* window = windowGetWindow(win);
    if (window == nullptr || !windowHdIsActive()) {
        return;
    }

    auto find = [&](const unsigned char* data) -> const FrmImage* {
        if (data == nullptr) {
            return nullptr;
        }
        for (int index = 0; index < count; index++) {
            if (!images[index].isLocked()) {
                continue;
            }
            if (images[index].getData() == data) {
                return &images[index];
            }
            if (copies != nullptr && copies[index] == data && copies[index] != reinterpret_cast<unsigned char*>(-1)) {
                return &images[index];
            }
        }
        return nullptr;
    };

    for (Button* button = window->buttonListHead; button != nullptr; button = button->next) {
        if (button->hdArt != nullptr || (button->flags & BUTTON_FLAG_GRAPHIC) == 0) {
            continue;
        }

        const FrmImage* normal = find(button->normalImage);
        const FrmImage* pressed = find(button->pressedImage);
        const FrmImage* hover = find(button->hoverImage);
        if (normal != nullptr || pressed != nullptr || hover != nullptr) {
            hdArtSetButtonArt(button->id, normal, pressed, hover);
        }
    }
}

void hdArtGetStats(HdArtStats* stats)
{
    stats->assetsResolved = static_cast<int>(gHdArt.assets.size());
    stats->assetsWithHd = 0;
    for (const auto& entry : gHdArt.assets) {
        if (entry.second.kind != AssetKind::None) {
            stats->assetsWithHd++;
        }
    }
    stats->framesCached = static_cast<int>(gHdArt.lru.size());
    stats->bytesCached = gHdArt.bytes;
    stats->hits = gHdArt.hits;
    stats->misses = gHdArt.misses;
}

} // namespace fallout
