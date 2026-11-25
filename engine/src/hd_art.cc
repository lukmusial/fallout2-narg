#include "hd_art.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "art.h"
#include "color.h"
#include "db.h"
#include "debug.h"
#include "hd_format.h"
#include "hd_upscale.h"
#include "platform_compat.h"
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

        // Runtime scripted upscale (Phase 2.22): frames without a pack
        // image. Results live in the LRU above ("rt<palette>:" keys).
        HdUpscalePool pool;
        bool poolStarted = false;
        unsigned char palette8[256 * 3] = {};
        bool paletteKnown = false;
        std::string paletteTag;
        // Art paths whose legacy art cannot be read (no runtime image).
        std::unordered_set<std::string> runtimeMissing;
        // Frames (frameKey "hd:") drawn from the runtime upscale.
        std::unordered_set<std::string> runtimeServed;
        unsigned long long runtimeInline = 0; // upscaled on first draw
        double runtimeInlineMs = 0;
        double runtimeInlineMaxMs = 0;
        unsigned long long runtimeFromPool = 0; // ready (or finished) in the background
        double runtimeTakeMs = 0; // main-thread time getting those (waiting, or running a queued one)
        unsigned long long preupscaleQueued = 0;
        // The last batch queued (start of the game, map load): when, how
        // many frames, main-thread time spent queueing them.
        std::chrono::steady_clock::time_point batchStart;
        bool batchActive = false;
        int batchQueued = 0;
        double batchQueueMs = 0;
        std::string batchName;
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
        gHdArt.runtimeServed.clear();
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

    // The pack to draw a frame from: the first with an image of it made from
    // the running game's frame; empty when none. Decided once per frame.
    const std::string& pickPack(const std::string& path, int frame, int direction)
    {
        static const std::string kNone;
        const std::vector<std::string>& packs = packsForAsset(path);
        bool inPack = false;
        for (const std::string& pack : packs) {
            if (resolveAsset(pack, path).kind != AssetKind::None) {
                inPack = true;
                break;
            }
        }
        if (!inPack) {
            return kNone;
        }

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
        return chosen->second;
    }

    // --- runtime scripted upscale (Phase 2.22) ---------------------------------

    bool runtimeEnabled()
    {
        return settings.graphics.hd && !gHdArt.pack.empty()
            && compat_stricmp(settings.graphics.hd_upscale.c_str(), "scripted") == 0;
    }

    // Whether frames of `path` (normalised) without a pack image are
    // upscaled at runtime: legacy art only (not the packs' ground variants),
    // critters only with hd_upscale_critters.
    bool runtimeAllowed(const std::string& path)
    {
        if (!runtimeEnabled() || path.compare(0, 4, "art/") != 0) {
            return false;
        }
        if (path.compare(0, 19, "art/tiles/variants/") == 0) {
            return false;
        }
        if (path.compare(0, 13, "art/critters/") == 0 && !settings.graphics.hd_upscale_critters) {
            return false;
        }
        return true;
    }

    bool isTilePath(const std::string& path)
    {
        return path.compare(0, 10, "art/tiles/") == 0;
    }

    // The base palette (8 bits per channel) and a short tag naming it, part
    // of every runtime key: art shown through another palette (the death
    // screen, endings) is upscaled again with it.
    const std::string& currentPalette(unsigned char* palette8)
    {
        unsigned char now[256 * 3];
        for (int index = 0; index < 256 * 3; index++) {
            now[index] = colorExpand6(_cmap[index]);
        }
        if (!gHdArt.paletteKnown || memcmp(now, gHdArt.palette8, sizeof(now)) != 0) {
            memcpy(gHdArt.palette8, now, sizeof(now));
            gHdArt.paletteKnown = true;
            uint64_t hash = 0xcbf29ce484222325ull;
            for (unsigned char byte : now) {
                hash = (hash ^ byte) * 0x100000001b3ull;
            }
            char tag[24];
            snprintf(tag, sizeof(tag), "%08x", static_cast<unsigned int>(hash ^ (hash >> 32)));
            gHdArt.paletteTag = tag;
        }
        memcpy(palette8, gHdArt.palette8, sizeof(now));
        return gHdArt.paletteTag;
    }

    std::string runtimeKey(const std::string& tag, const std::string& path, int frame, int direction)
    {
        return frameKey(("rt" + tag + ":").c_str(), path, frame, direction);
    }

    void ensurePool()
    {
        if (gHdArt.poolStarted) {
            return;
        }
        gHdArt.poolStarted = true;
        std::string cacheDir;
        if (settings.graphics.hd_upscale_cache) {
            cacheDir = settings.system.master_patches_path + "/hd/cache";
        }
        gHdArt.pool.start(settings.graphics.hd_upscale_threads, cacheDir);
        debugPrint("HD: runtime upscale: %d background thread(s), disk cache %s\n", gHdArt.pool.threadCount(),
            cacheDir.empty() ? "off" : cacheDir.c_str());
    }

    // Frames the background threads finished go into the LRU.
    void drainPool()
    {
        if (!gHdArt.poolStarted) {
            return;
        }
        gHdArt.pool.drain([](const std::string& key, std::shared_ptr<HdImage> image) {
            if (gHdArt.frames.find(key) == gHdArt.frames.end()) {
                cachePut(key, std::move(image));
            }
        });
    }

    HdUpscaleJob makeJob(const std::string& key, const std::string& path, const unsigned char* data, int width, int height,
        int pitch, const unsigned char* palette8)
    {
        HdUpscaleJob job;
        job.key = key;
        job.width = width;
        job.height = height;
        job.indices.resize(static_cast<size_t>(width) * height);
        for (int y = 0; y < height; y++) {
            memcpy(job.indices.data() + static_cast<size_t>(y) * width, data + static_cast<size_t>(y) * pitch, width);
        }
        memcpy(job.palette8, palette8, sizeof(job.palette8));
        job.tile = isTilePath(path);
        job.precise = settings.graphics.hd_upscale_precision >= 16;
        return job;
    }

    // Directions with their own frames: art with one direction points every
    // direction at the same data and is looked up as direction 0.
    bool artHasDirection(Art* art, int direction)
    {
        return direction == 0 || art->dataOffsets[direction] != art->dataOffsets[0];
    }

    // Queues every frame of `art` (the legacy art of `path`) that would be
    // upscaled at runtime and is not ready yet, except (skipFrame,
    // skipDirection).
    int queueArtFrames(const std::string& path, Art* art, int skipFrame, int skipDirection)
    {
        if (!settings.graphics.hd_upscale_preload || art == nullptr) {
            return 0;
        }
        ensurePool();
        unsigned char palette8[256 * 3];
        const std::string& tag = currentPalette(palette8);
        int queued = 0;
        int frames = artGetFrameCount(art);
        for (int direction = 0; direction < ROTATION_COUNT; direction++) {
            if (!artHasDirection(art, direction)) {
                continue;
            }
            for (int frame = 0; frame < frames; frame++) {
                if (frame == skipFrame && direction == skipDirection) {
                    continue;
                }
                if (!pickPack(path, frame, direction).empty()) {
                    continue;
                }
                std::string key = runtimeKey(tag, path, frame, direction);
                if (gHdArt.frames.find(key) != gHdArt.frames.end()) {
                    continue;
                }
                int width = 0;
                int height = 0;
                int xOffset = 0;
                int yOffset = 0;
                unsigned char* data = artGetFrameData(art, frame, static_cast<Rotation>(direction), &width, &height, &xOffset, &yOffset);
                if (data == nullptr || width <= 0 || height <= 0) {
                    continue;
                }
                HdUpscaleJob job = makeJob(key, path, data, width, height, width, palette8);
                // First frames first: tiles, walls and most scenery have
                // one; critters and animations show their first frame (or
                // the one they rest on) when the map appears.
                job.urgent = frame == 0;
                gHdArt.pool.submit(std::move(job));
                queued++;
            }
        }
        gHdArt.preupscaleQueued += queued;
        return queued;
    }

    // The runtime image of a frame no pack has: from the LRU, from the
    // background threads (waiting for a running job, running a queued one
    // here), else upscaled now. `data` (width x height, pitch) is the legacy
    // frame when the caller has it; otherwise it is read from the art file.
    std::shared_ptr<const HdImage> runtimeImage(const std::string& path, int frame, int direction, const unsigned char* data,
        int width, int height, int pitch)
    {
        unsigned char palette8[256 * 3];
        const std::string& tag = currentPalette(palette8);
        std::string key = runtimeKey(tag, path, frame, direction);

        drainPool();
        std::shared_ptr<const HdImage> cached = cacheGet(key);
        if (cached != nullptr) {
            return cached;
        }

        ensurePool();
        auto taking = std::chrono::steady_clock::now();
        std::shared_ptr<HdImage> image = gHdArt.pool.take(key);
        if (image != nullptr) {
            gHdArt.runtimeTakeMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - taking).count();
            gHdArt.runtimeFromPool++;
            cachePut(key, image);
            return image;
        }

        HdUpscaleJob job;
        if (data != nullptr && width > 0 && height > 0) {
            job = makeJob(key, path, data, width, height, pitch, palette8);
        } else {
            // Reading the art file from inside an art cache read (a present
            // during a file read) could re-enter the file layer: draw the
            // legacy frame this time.
            if (gHdArt.runtimeMissing.count(path) != 0 || artIsLoading() || direction < 0 || direction >= ROTATION_COUNT) {
                return nullptr;
            }
            Art* art = loadLegacyArt(path);
            if (art == nullptr) {
                gHdArt.runtimeMissing.insert(path);
                return nullptr;
            }
            int xOffset = 0;
            int yOffset = 0;
            unsigned char* frameData = artGetFrameData(art, frame, static_cast<Rotation>(direction), &width, &height, &xOffset, &yOffset);
            if (frameData == nullptr || width <= 0 || height <= 0) {
                internal_free(art);
                return nullptr;
            }
            job = makeJob(key, path, frameData, width, height, width, palette8);
            // The art's other frames (animations, button states) follow in
            // the background.
            queueArtFrames(path, art, frame, direction);
            internal_free(art);
        }

        auto started = std::chrono::steady_clock::now();
        image = gHdArt.pool.run(job, true);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        gHdArt.runtimeInline++;
        gHdArt.runtimeInlineMs += ms;
        gHdArt.runtimeInlineMaxMs = std::max(gHdArt.runtimeInlineMaxMs, ms);
        debugPrint("HD: upscaled %s (direction %d, frame %d, %dx%d) on first draw: %.2f ms\n", path.c_str(), direction, frame,
            job.width, job.height, ms);
        cachePut(key, image);
        return image;
    }

    // The image to draw for a frame: the pack's (hdArtFindFrame), else the
    // runtime upscale (hd_upscale=scripted), else null.
    std::shared_ptr<const HdImage> findFrame(const char* artPath, int frame, int direction, const unsigned char* data,
        int width, int height, int pitch)
    {
        ensureInitialized();
        if (gHdArt.pack.empty()) {
            return nullptr;
        }

        std::string path = hdArtNormalizePath(artPath);
        const std::string& pack = pickPack(path, frame, direction);
        if (pack.empty()) {
            if (!runtimeAllowed(path)) {
                return nullptr;
            }
            std::shared_ptr<const HdImage> image = runtimeImage(path, frame, direction, data, width, height, pitch);
            if (image != nullptr) {
                gHdArt.runtimeServed.insert(frameKey("hd:", path, frame, direction));
            }
            return image;
        }

        std::string key = frameKey("hd:", path, frame, direction);
        std::shared_ptr<const HdImage> image = cacheGet(key);
        if (image != nullptr) {
            return image;
        }

        auto loaded = std::make_shared<HdImage>();
        if (!loadAssetFrame(resolveAsset(pack, path), frame, direction, loaded.get())) {
            return nullptr;
        }

        cachePut(key, loaded);
        return loaded;
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
    return findFrame(artPath, frame, direction, nullptr, 0, 0, 0);
}

std::string hdArtFramePack(const char* artPath, int frame, int direction)
{
    std::string key = frameKey("hd:", hdArtNormalizePath(artPath), frame, direction);
    auto found = gHdArt.framePacks.find(key);
    if (found != gHdArt.framePacks.end() && !found->second.empty()) {
        return found->second;
    }
    return gHdArt.runtimeServed.count(key) != 0 ? std::string("runtime") : std::string();
}

std::shared_ptr<const HdImage> hdArtGetFrame(const char* artPath, int frame, int direction,
    const unsigned char* legacyData, int width, int height, int pitch, bool* fromLegacy)
{
    std::shared_ptr<const HdImage> image = findFrame(artPath, frame, direction, legacyData, width, height, pitch);
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
        : findFrame(path, frame, direction, legacyData, width, height, legacyPitch);
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

    return findFrame(path.c_str(), image.getFrame(), 0, image.getData(), image.getWidth(), image.getHeight(), image.getWidth());
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

    std::shared_ptr<const HdImage> hd = findFrame(path, frame, 0, legacyData, legacyWidth, legacyHeight, legacyWidth);
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

    stats->runtimeServed = static_cast<int>(gHdArt.runtimeServed.size());
    stats->runtimeInline = gHdArt.runtimeInline;
    stats->runtimeInlineMs = gHdArt.runtimeInlineMs;
    stats->runtimeInlineMaxMs = gHdArt.runtimeInlineMaxMs;
    stats->runtimeFromPool = gHdArt.runtimeFromPool;
    stats->runtimeTakeMs = gHdArt.runtimeTakeMs;
    stats->preupscaleQueued = gHdArt.preupscaleQueued;
    if (gHdArt.poolStarted) {
        HdUpscaleStats pool = gHdArt.pool.stats();
        stats->upscaleThreads = gHdArt.pool.threadCount();
        stats->upscalePending = gHdArt.pool.pending();
        stats->upscaleComputed = pool.computed;
        stats->upscaleComputedInline = pool.computedInline;
        stats->upscaleDiskHits = pool.diskHits;
        stats->upscaleDiskWrites = pool.diskWrites;
        stats->upscaleComputeMs = pool.computeMs;
        stats->upscaleOutputPixels = pool.outputPixels;
        if (gHdArt.batchActive) {
            stats->batchName = gHdArt.batchName;
            stats->batchQueued = gHdArt.batchQueued;
            stats->batchQueueMs = gHdArt.batchQueueMs;
            if (stats->upscalePending == 0 && pool.idleSince > gHdArt.batchStart) {
                stats->batchDoneMs = std::chrono::duration<double, std::milli>(pool.idleSince - gHdArt.batchStart).count();
            }
        }
    }
}

// Early interface art (hdArtPreupscaleInterface): the main menu, load/save,
// options and preferences, new character, the interface bar, inventory,
// character screen, Pip-Boy and skilldex; the main menu and the bar first,
// then the largest. Collected from the "upscaled ... on first draw" lines of
// debug.log (tools/dev/hd_run.py with hd_upscale_preload=0).
static const char* const kEarlyInterfaceArt[] = {
    "art/intrface/mainmenu_sp.png",
    "art/intrface/menuup.frm",
    "art/intrface/menudown.frm",
    "art/intrface/mainmenu.frm",
    "art/intrface/iface.frm",
    "art/intrface/prefscrn.frm",
    "art/intrface/pip.frm",
    "art/intrface/pickchar.frm",
    "art/intrface/lsgame.frm",
    "art/intrface/edtredt.frm",
    "art/intrface/invbox2.png",
    "art/intrface/combat.frm",
    "art/intrface/skldxbox.frm",
    "art/intrface/opbase_help.png",
    "art/intrface/pipx.frm",
    "art/intrface/sattkbup.frm",
    "art/intrface/sattkbdn.frm",
    "art/intrface/perksfdr.frm",
    "art/intrface/prfbknbs.frm",
    "art/intrface/bignum.frm",
    "art/intrface/numbers.frm",
    "art/intrface/months.frm",
    "art/intrface/nameoff.frm",
    "art/intrface/opbtnoff.frm",
    "art/intrface/skldxoff.frm",
    "art/intrface/ageoff.frm",
    "art/intrface/sexoff.frm",
    "art/intrface/iface_apbar_e.frm",
    "art/intrface/slider.frm",
    "art/intrface/optiup.frm",
    "art/intrface/prflknbs.frm",
    "art/intrface/pipup.frm",
    "art/intrface/mapup.frm",
    "art/intrface/chaup.frm",
    "art/intrface/invbutup.frm",
    "art/intrface/alarmout.frm",
    "art/intrface/invupout.frm",
    "art/intrface/invupds.frm",
    "art/intrface/invdnout.frm",
    "art/intrface/invdnds.frm",
    "art/intrface/bigredup.frm",
    "art/intrface/sru.frm",
    "art/intrface/slu.frm",
    "art/intrface/prfxout.frm",
    "art/intrface/prfsldof.frm",
    "art/intrface/lilredup.frm",
    "art/intrface/snegoff.frm",
    "art/intrface/splsoff.frm",
    "art/intrface/uparwoff.frm",
    "art/intrface/dnarwoff.frm",
};

void hdArtPreupscaleInterface()
{
    ensureInitialized();
    if (!settings.graphics.hd_upscale_preload || !runtimeEnabled()) {
        return;
    }
    auto started = std::chrono::steady_clock::now();
    gHdArt.batchStart = started;
    gHdArt.batchActive = true;
    gHdArt.batchQueued = 0;
    gHdArt.batchName = "interface";
    int queued = 0;
    // Called at start and again when the main menu loads: art that cannot
    // be read yet at start (the CE main menu background) is queued then.
    static bool done[sizeof(kEarlyInterfaceArt) / sizeof(kEarlyInterfaceArt[0])] = {};
    for (size_t index = 0; index < sizeof(kEarlyInterfaceArt) / sizeof(kEarlyInterfaceArt[0]); index++) {
        if (done[index]) {
            continue;
        }
        std::string path = hdArtNormalizePath(kEarlyInterfaceArt[index]);
        if (!runtimeAllowed(path) || !pickPack(path, 0, 0).empty()) {
            done[index] = true;
            continue;
        }
        Art* art = loadLegacyArt(path);
        if (art == nullptr) {
            continue;
        }
        done[index] = true;
        queued += queueArtFrames(path, art, -1, -1);
        internal_free(art);
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    gHdArt.batchQueued = queued;
    gHdArt.batchQueueMs = ms;
    debugPrint("HD: queued %d interface frame(s) for the runtime upscale in %.1f ms\n", queued, ms);
}

bool hdArtUpscalesCritters()
{
    ensureInitialized();
    return runtimeEnabled() && settings.graphics.hd_upscale_critters;
}

void hdArtPreupscaleMapBegin()
{
    if (gHdArt.poolStarted) {
        // Frames of the previous map not upscaled yet are no longer needed
        // first; those still wanted are upscaled when drawn.
        gHdArt.pool.cancelQueued();
    }
    gHdArt.batchStart = std::chrono::steady_clock::now();
    gHdArt.batchActive = runtimeEnabled() && settings.graphics.hd_upscale_preload;
    gHdArt.batchQueued = 0;
    gHdArt.batchQueueMs = 0;
    gHdArt.batchName = "map";
}

void hdArtPreupscaleArt(const char* artPath, Art* art)
{
    ensureInitialized();
    if (artPath == nullptr || art == nullptr || !settings.graphics.hd_upscale_preload) {
        return;
    }
    std::string path = hdArtNormalizePath(artPath);
    if (!runtimeAllowed(path)) {
        return;
    }
    auto started = std::chrono::steady_clock::now();
    gHdArt.batchQueued += queueArtFrames(path, art, -1, -1);
    gHdArt.batchQueueMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

int hdArtUpscalePending()
{
    if (!gHdArt.poolStarted) {
        return 0;
    }
    return gHdArt.pool.pending();
}

void hdArtUpscaleWaitIdle()
{
    if (gHdArt.poolStarted) {
        gHdArt.pool.waitIdle();
        drainPool();
    }
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
