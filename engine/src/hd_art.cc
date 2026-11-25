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
#include "memory.h"
#include "settings.h"
#include "stimpak.h"
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
        // Legacy source check per frame (direction << 16 | frame): true when
        // the running game's frame is the one the pack's image was made from.
        std::unordered_map<int, bool> legacyOk;
    };

    struct CachedFrame {
        std::string key;
        std::shared_ptr<const HdImage> image;
        size_t bytes;
    };

    struct HdArtState {
        bool initialized = false;
        std::string pack;
        unsigned int generation = 0;

        // Manifest of the active pack: art path -> packs to try in order
        // ("*" = default).
        std::unordered_map<std::string, std::vector<std::string>> manifest;
        std::vector<std::string> manifestDefault;
        std::vector<std::string> activePack;

        // By "<pack>:<art path>".
        std::unordered_map<std::string, Asset> assets;
        // The pack each frame is drawn from (frameKey without prefix), empty
        // when none has a usable image.
        std::unordered_map<std::string, std::string> framePacks;

        std::list<CachedFrame> lru; // most recently used first
        std::unordered_map<std::string, std::list<CachedFrame>::iterator> frames;
        size_t bytes = 0;

        unsigned long long hits = 0;
        unsigned long long misses = 0;

        // legacy.txt of each pack used (empty: the pack has none, no check).
        std::unordered_map<std::string, std::unordered_map<std::string, uint64_t>> legacyTables;
        // Hashes of the running game's frames, by art path (hdLegacyKey).
        std::unordered_map<std::string, std::unordered_map<std::string, uint64_t>> runningHashes;
        int legacyChecked = 0;
        int legacyRejected = 0;
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
        gHdArt.activePack.clear();
        if (gHdArt.pack.empty()) {
            return;
        }
        gHdArt.activePack.push_back(gHdArt.pack);

        std::vector<unsigned char> data;
        if (!readWholeFile("hd/" + gHdArt.pack + "/manifest.txt", data)) {
            return;
        }

        std::string text(data.begin(), data.end());
        std::unordered_map<std::string, std::string> entries;
        std::string defaultPack;
        hdManifestParse(text, &entries, &defaultPack);
        for (const auto& entry : entries) {
            gHdArt.manifest[entry.first] = hdPackList(entry.second);
        }
        gHdArt.manifestDefault = hdPackList(defaultPack);

        debugPrint("HD: pack %s manifest: %zu entries, default %s\n", gHdArt.pack.c_str(), gHdArt.manifest.size(), defaultPack.c_str());
    }

    void clearCache()
    {
        gHdArt.assets.clear();
        gHdArt.framePacks.clear();
        gHdArt.lru.clear();
        gHdArt.frames.clear();
        gHdArt.bytes = 0;
        gHdArt.legacyTables.clear();
        gHdArt.runningHashes.clear();
        gHdArt.legacyChecked = 0;
        gHdArt.legacyRejected = 0;
    }

    void ensureInitialized()
    {
        if (!gHdArt.initialized) {
            gHdArt.initialized = true;
            gHdArt.pack = settings.graphics.hd_pack;
            loadManifest();
        }
    }

    // The packs to look in for an asset, in order. A manifest may list several
    // (`* = ai rpu`): a pack made from a mod's art then follows the normal one
    // and is used where the running game shows the mod's frame (legacy.txt).
    const std::vector<std::string>& packsForAsset(const std::string& key)
    {
        auto it = gHdArt.manifest.find(key);
        if (it != gHdArt.manifest.end()) {
            return it->second;
        }
        if (!gHdArt.manifestDefault.empty()) {
            return gHdArt.manifestDefault;
        }
        return gHdArt.activePack;
    }

    Asset& resolveAsset(const std::string& pack, const std::string& key)
    {
        std::string id = pack + ":" + key;
        auto found = gHdArt.assets.find(id);
        if (found != gHdArt.assets.end()) {
            return found->second;
        }

        Asset asset;
        if (!pack.empty()) {
            std::string base = "hd/" + pack + "/" + key;
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

        return gHdArt.assets.emplace(id, std::move(asset)).first->second;
    }

    bool assetHasFrame(const Asset& asset, int frame, int direction)
    {
        if (asset.kind == AssetKind::Png) {
            return frame == 0 && direction == 0;
        }
        return asset.kind == AssetKind::Fhd && asset.fhd.find(frame, direction) != nullptr;
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

    const std::unordered_map<std::string, uint64_t>& legacyTable(const std::string& pack)
    {
        auto found = gHdArt.legacyTables.find(pack);
        if (found != gHdArt.legacyTables.end()) {
            return found->second;
        }

        std::unordered_map<std::string, uint64_t> table;
        std::vector<unsigned char> data;
        if (readWholeFile("hd/" + pack + "/legacy.txt", data)) {
            hdLegacyParse(std::string(data.begin(), data.end()), &table);
            debugPrint("HD: pack %s: legacy sources of %zu frames\n", pack.c_str(), table.size());
        }
        return gHdArt.legacyTables.emplace(pack, std::move(table)).first->second;
    }

    // The legacy art the game draws for `path` (normalised): the localized
    // version when there is one, as the art cache and named art do.
    Art* loadLegacyArt(const std::string& path)
    {
        const std::string& language = settings.system.language;
        if (!language.empty() && path.compare(0, 4, "art/") == 0) {
            std::string localized = dbPath("art/" + language + "/" + path.substr(4));
            if (fileExists(localized)) {
                Art* art = artLoad(localized.c_str());
                if (art != nullptr) {
                    return art;
                }
            }
        }
        std::string name = dbPath(path);
        return artLoad(name.c_str());
    }

    const std::unordered_map<std::string, uint64_t>& runningLegacyHashes(const std::string& path)
    {
        auto found = gHdArt.runningHashes.find(path);
        if (found != gHdArt.runningHashes.end()) {
            return found->second;
        }

        std::unordered_map<std::string, uint64_t> hashes;
        Art* art = loadLegacyArt(path);
        if (art != nullptr) {
            int frames = artGetFrameCount(art);
            for (int direction = 0; direction < ROTATION_COUNT; direction++) {
                for (int frame = 0; frame < frames; frame++) {
                    int width = 0;
                    int height = 0;
                    int xOffset = 0;
                    int yOffset = 0;
                    unsigned char* data = artGetFrameData(art, frame, static_cast<Rotation>(direction), &width, &height, &xOffset, &yOffset);
                    if (data != nullptr && width > 0 && height > 0) {
                        hashes[hdLegacyKey(path, direction, frame)] = hdLegacyFrameHash(data, width, height, width);
                    }
                }
            }
            internal_free(art);
        }
        return gHdArt.runningHashes.emplace(path, std::move(hashes)).first->second;
    }

    // Whether the pack's image of `path` (normalised) may be drawn: the
    // running game's legacy frame must be the one the image was made from
    // (the pack's legacy.txt). Packs without legacy.txt, frames it does not
    // list and `[graphics] hd_verify=0` are not checked. Ground variants
    // (art/tiles/variants/<tile>/<k>) follow their tile.
    bool legacyFrameMatches(Asset& asset, const std::string& pack, const std::string& path, int frame, int direction)
    {
        if (!settings.graphics.hd_verify) {
            return true;
        }

        int slot = (direction << 16) | (frame & 0xFFFF);
        auto known = asset.legacyOk.find(slot);
        if (known != asset.legacyOk.end()) {
            return known->second;
        }

        bool ok = true;
        const auto& table = legacyTable(pack);
        if (!table.empty()) {
            std::string source = path;
            int sourceFrame = frame;
            int sourceDirection = direction;
            static const std::string kVariants = "art/tiles/variants/";
            if (path.compare(0, kVariants.size(), kVariants) == 0) {
                size_t slash = path.find('/', kVariants.size());
                source = "art/tiles/" + path.substr(kVariants.size(), slash - kVariants.size()) + ".frm";
                sourceFrame = 0;
                sourceDirection = 0;
            }

            std::string key = hdLegacyKey(source, sourceDirection, sourceFrame);
            auto expected = table.find(key);
            if (expected == table.end() && sourceDirection != 0) {
                // One-direction art is listed as direction 0.
                key = hdLegacyKey(source, 0, sourceFrame);
                expected = table.find(key);
            }
            if (expected != table.end()) {
                gHdArt.legacyChecked++;
                const auto& running = runningLegacyHashes(source);
                auto actual = running.find(key);
                ok = actual != running.end() && actual->second == expected->second;
                if (!ok) {
                    debugPrint("HD: %s (direction %d, frame %d) is not the art pack %s was made from\n",
                        source.c_str(), sourceDirection, sourceFrame, pack.c_str());
                }
            }
        }

        asset.legacyOk.emplace(slot, ok);
        return ok;
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
    gHdArt.generation++;
    clearCache();
    loadManifest();
    debugPrint("HD: pack set to '%s'\n", gHdArt.pack.c_str());
}

const std::string& hdArtGetPack()
{
    ensureInitialized();
    return gHdArt.pack;
}

unsigned int hdArtGeneration()
{
    return gHdArt.generation;
}

std::shared_ptr<const HdImage> hdArtFindFrame(const char* artPath, int frame, int direction)
{
    ensureInitialized();
    if (gHdArt.pack.empty()) {
        return nullptr;
    }

    std::string path = hdArtNormalizePath(artPath);
    const std::vector<std::string>& packs = packsForAsset(path);
    bool inPack = false;
    for (const std::string& pack : packs) {
        if (resolveAsset(pack, path).kind != AssetKind::None) {
            inPack = true;
            break;
        }
    }
    if (!inPack) {
        return nullptr;
    }

    // The first pack with an image of this frame made from the running
    // game's frame; decided once per frame.
    std::string key = frameKey("hd:", path, frame, direction);
    auto chosen = gHdArt.framePacks.find(key);
    if (chosen == gHdArt.framePacks.end()) {
        std::string pick;
        bool rejected = false;
        for (const std::string& pack : packs) {
            Asset& candidate = resolveAsset(pack, path);
            if (!assetHasFrame(candidate, frame, direction)) {
                continue;
            }
            if (legacyFrameMatches(candidate, pack, path, frame, direction)) {
                pick = pack;
                break;
            }
            rejected = true;
        }
        if (pick.empty() && rejected) {
            gHdArt.legacyRejected++;
            debugPrint("HD: no pack has an image of %s (direction %d, frame %d) as the game draws it; drawing the game's own frame\n",
                path.c_str(), direction, frame);
        }
        chosen = gHdArt.framePacks.emplace(key, pick).first;
    }
    if (chosen->second.empty()) {
        return nullptr;
    }

    std::shared_ptr<const HdImage> image = cacheGet(key);
    if (image != nullptr) {
        return image;
    }

    auto loaded = std::make_shared<HdImage>();
    if (!loadAssetFrame(resolveAsset(chosen->second, path), frame, direction, loaded.get())) {
        return nullptr;
    }

    cachePut(key, loaded);
    return loaded;
}

std::string hdArtFramePack(const char* artPath, int frame, int direction)
{
    auto found = gHdArt.framePacks.find(frameKey("hd:", hdArtNormalizePath(artPath), frame, direction));
    return found != gHdArt.framePacks.end() ? found->second : std::string();
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

bool hdArtDrawImageOffset(int win, const FrmImage& image, int srcOffset, int width, int height, int destOffset, int destPitch)
{
    if (!windowHdIsActive() || !image.isLocked() || image.getWidth() <= 0 || destPitch <= 0) {
        return false;
    }

    return hdArtDrawImageRegion(win, image, srcOffset % image.getWidth(), srcOffset / image.getWidth(), width, height,
        destOffset % destPitch, destOffset / destPitch);
}

bool hdArtDrawFrameRegion(int win, const FrmId& frmId, int frame, const unsigned char* legacyData, int legacyWidth,
    int legacyHeight, int srcX, int srcY, int width, int height, int destX, int destY)
{
    if (!windowHdIsActive() || !frmId.valid()) {
        return false;
    }

    const char* path = frmId.filePath();
    if (path == nullptr) {
        return false;
    }

    std::shared_ptr<const HdImage> hd = hdArtFindFrame(path, frame, 0);
    if (hd == nullptr) {
        return false;
    }

    return hdArtDrawRegion(win, *hd, legacyData, legacyWidth, legacyHeight, srcX, srcY, width, height, destX, destY);
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

void hdArtRender(int win, const FrmId& frmId, int x, int y, int width, int height)
{
    unsigned char* buffer = windowGetBuffer(win);
    int pitch = windowGetWidth(win);
    if (buffer == nullptr) {
        return;
    }

    std::shared_ptr<const HdImage> hd;
    HdLayer* layer = nullptr;
    int drawX = 0;
    int drawY = 0;
    int drawWidth = 0;
    int drawHeight = 0;
    if (windowHdIsActive() && frmId.valid()) {
        FrmImage frmImage;
        if (frmImage.lock(frmId)) {
            hd = hdArtFindImage(frmImage);
            if (hd != nullptr) {
                artRenderGetRect(frmImage.getWidth(), frmImage.getHeight(), width, height, &drawX, &drawY, &drawWidth, &drawHeight);
                layer = windowHdGetLayer(win);
            }
        }
    }

    if (layer == nullptr) {
        artRender(frmId, buffer + pitch * y + x, width, height, pitch);
        return;
    }

    unsigned char basePalette8[256 * 3];
    windowHdGetBasePalette(basePalette8);

    std::vector<uint32_t> under;
    hdLayerCapture(layer, buffer, pitch, x + drawX, y + drawY, drawWidth, drawHeight, basePalette8, &under);
    artRender(frmId, buffer + pitch * y + x, width, height, pitch);
    hdLayerDrawOver(layer, buffer, pitch, x + drawX, y + drawY, drawWidth, drawHeight, *hd, under.data(), basePalette8);
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
    stats->legacyChecked = gHdArt.legacyChecked;
    stats->legacyRejected = gHdArt.legacyRejected;
}

// Stimpak's screens (hub, pickers, lists): HD art for the images and
// buttons they draw, as for the game's own screens.
void hdArtAttachStimpak()
{
    StimpakRenderHooks hooks;
    hooks.imageDrawn = [](int window, const FrmImage& image, int srcX, int srcY, int width, int height, int destX, int destY) {
        hdArtDrawImageRegion(window, image, srcX, srcY, width, height, destX, destY);
    };
    hooks.buttonArt = [](int button, const FrmImage* normal, const FrmImage* pressed) {
        hdArtSetButtonArt(button, normal, pressed);
    };
    stimpakSetRenderHooks(hooks);
}

} // namespace fallout
