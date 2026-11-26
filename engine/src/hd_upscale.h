#ifndef FALLOUT_HD_UPSCALE_H_
#define FALLOUT_HD_UPSCALE_H_

#include <stdint.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "hd_layer.h"

namespace fallout {

// "Scripted" 2x upscale of 8-bit art at runtime (Phase 2.22): a C++ port of
// tools/hd/enhance_scripted.py (enhance_indices + hdlib.compose_rgba), the
// deterministic, non-ML enhancer the offline "scripted" HD pack is made with.
//
//   1. palette -> RGB; transparent (index 0) and colour-cycling (229..254)
//      pixels are holes, filled from their neighbours. Floor and roof tiles
//      are first surrounded by copies of themselves on the isometric lattice
//      so their edges see a continuation (seamless).
//   2. palette-aware de-dither: 3x3 bilateral filter, range sigma per pixel =
//      distance from its palette colour to the nearest other palette colour.
//   3. edge-directed 2x: directional cubic convolution (DCCI).
//   4. gradient restoration at 2x (5x5 range-limited smoothing) and a light
//      edge-aware unsharp mask; part of the removed dither re-added as grain.
//   5. alpha: nearest 2x of the silhouette; cycling pixels alpha 0 (the
//      legacy palette animation shows through).
//
// Not bit-exact with numpy (float order, a polynomial exp), the same picture
// within rounding: tests/unit/hd_upscale_test.cc compares with fixtures the
// Python made (tools/hd/make_upscale_fixtures.py).

// Changes whenever the output of hdUpscaleScripted changes (disk cache key).
constexpr int kHdUpscaleVersion = 1;

// Margin added around a tile (source pixels) for the lattice copies.
constexpr int kHdUpscaleTilePad = 8;

// `indices` (width x height, row pitch `pitch`), `palette8` 256 RGB entries
// (8 bits per channel); `tile` for floor/roof tiles. `dest` is 2x the size,
// pixels 0xAARRGGBB (alpha 0 or 255). With `threads` > 1 a large image is
// split into horizontal strips (with enough overlap that the result is the
// same) upscaled on that many threads.
//
// `precise` (the palette boost, `[graphics] hd_upscale_precision=16`): the
// palette's 6-bit values (palette8 >> 2: the engine's colorExpand6 and the
// tools' << 2 both keep them) are expanded at full precision, v * 255 / 63,
// and every step works on floats (finer than 16 bits per channel); a
// de-banding pass turns the one-palette-step bands of smooth areas into
// ramps (edges of several steps stay; done at source resolution on the
// de-dithered base, where the bands are), and the result is rounded to 8
// bits with a light 4x4 ordered dither. Without it the output is the Python's
// (8-bit palette, no de-banding, plain rounding).
//
// `context` (tiles): the tile's real neighbours on the map at the 8 nearest
// lattice positions ([row delta + 1][column delta + 1], screen columns; each
// packed, the tile's size), drawn around it instead of copies of itself, so
// its edges continue into what is really next to it (no seams).
struct HdTileContext {
    const unsigned char* neighbour[3][3] = {};
};

void hdUpscaleScripted(const unsigned char* indices, int width, int height, int pitch, const unsigned char* palette8,
    bool tile, HdImage* dest, int threads = 1, bool precise = false, const HdTileContext* context = nullptr);

// Time spent per stage (ns, all threads): 0 palette/holes/steps, 1
// de-dither, 2 DCCI, 3 gradient restoration, 4 de-banding, 5 grain, 6
// output. `reset` clears the counters.
constexpr int kHdUpscaleStages = 7;
void hdUpscaleStageTimes(unsigned long long* ns, bool reset = false);

// FNV-1a 64 of everything the output depends on (pixels, size, palette, tile
// flag, algorithm version): the disk cache key.
uint64_t hdUpscaleKey(const unsigned char* indices, int width, int height, int pitch, const unsigned char* palette8,
    bool tile, bool precise = false, const HdTileContext* context = nullptr);

// Disk cache files: <dir>/<xx>/<key>.rgba (raw 0xAARRGGBB after a small
// header). Safe from any thread (plain stdio, write to a temporary name and
// rename). Return false on a miss or error.
bool hdUpscaleCacheRead(const std::string& dir, uint64_t key, HdImage* image);
bool hdUpscaleCacheWrite(const std::string& dir, uint64_t key, const HdImage& image);
// Removes the least recently used files (reads refresh a file's time) until
// the cache is at most 90% of `maxBytes` when it is above it; returns the
// size left.
unsigned long long hdUpscaleCacheTrim(const std::string& dir, unsigned long long maxBytes, unsigned long long* removedBytes = nullptr);

// Background upscaling of whole frames on a small pool of threads. Jobs carry
// their own copy of the pixels and palette: workers never touch the art
// cache, the file database or any other game state (only the optional disk
// cache folder).
struct HdUpscaleJob {
    std::string key; // caller's identity of the frame
    std::vector<unsigned char> indices; // width x height, packed
    int width = 0;
    int height = 0;
    unsigned char palette8[256 * 3];
    bool tile = false;
    bool precise = false; // hdUpscaleScripted's palette boost
    // Tiles: the neighbours (HdTileContext), [r + 1][c + 1], empty = itself.
    std::vector<unsigned char> context[3][3];
    // Lower first (e.g. the distance from the view in pixels).
    float priority = 0.0f;
    // Progress is counted per group (hdUpscaleJob groups: 0 general, the
    // scene a map load waits for, background work); cancelQueued can drop
    // one group.
    int group = 0;
};

struct HdUpscaleStats {
    unsigned long long submitted = 0;
    unsigned long long computed = 0; // by workers
    unsigned long long computedInline = 0; // by a caller (take or help)
    unsigned long long diskHits = 0;
    unsigned long long diskWrites = 0;
    unsigned long long waited = 0; // a caller waited for a running job
    double computeMs = 0; // summed over threads
    unsigned long long outputPixels = 0;
    // When the pool last ran out of work (nothing queued or running).
    std::chrono::steady_clock::time_point idleSince;
    unsigned long long cacheBytes = 0; // disk cache size at the last check
    unsigned long long cacheTrimmedBytes = 0;
};

class HdUpscalePool {
public:
    static constexpr int kGroups = 4;

    HdUpscalePool() = default;
    ~HdUpscalePool();
    HdUpscalePool(const HdUpscalePool&) = delete;
    HdUpscalePool& operator=(const HdUpscalePool&) = delete;

    // threads <= 0: hardware threads - 1 (1..8). `cacheDir` empty: no disk
    // cache; `cacheMaxBytes` > 0: its size cap.
    void start(int threads, const std::string& cacheDir, unsigned long long cacheMaxBytes = 0);
    void stop();
    bool running() const { return !workers_.empty(); }
    int threadCount() const { return static_cast<int>(workers_.size()); }

    // Queues a job unless one with that key is queued, running or done (a
    // queued one with a later priority is moved up).
    void submit(HdUpscaleJob job);

    // Drops queued jobs (not running or done ones), of one group or all
    // (group < 0).
    void cancelQueued(int group = -1);

    // The result for `key`: done -> returned (and forgotten); queued -> run
    // on the calling thread now; running -> waits for it. Null when the pool
    // has never seen the key (or it failed).
    // `ready` (optional): whether it was done already (no waiting or work).
    std::shared_ptr<HdImage> take(const std::string& key, bool* ready = nullptr);

    // Runs the first queued job on the calling thread (its result is kept
    // as done). False when nothing is queued.
    bool help();

    // Moves every finished result out (key, image, job group).
    void drain(const std::function<void(const std::string&, std::shared_ptr<HdImage>, int)>& sink);

    // Jobs queued or running (in total, or of a group).
    int pending();
    int pending(int group);
    // Jobs of a group submitted since its counters were reset, and how many
    // of them are finished.
    void groupProgress(int group, int* total, int* done);
    void resetGroup(int group);
    // Blocks until nothing is queued or running.
    void waitIdle();

    HdUpscaleStats stats();
    const std::string& cacheDir() const { return cacheDir_; }

    // Runs one job (disk cache first, then compute, then disk cache write)
    // on the calling thread.
    std::shared_ptr<HdImage> run(const HdUpscaleJob& job, bool inlineRun);

private:
    enum class State {
        Queued,
        Running,
        Done,
    };

    struct Entry {
        State state = State::Queued;
        std::unique_ptr<HdUpscaleJob> job;
        std::shared_ptr<HdImage> image;
        std::pair<float, unsigned long long> order;
        int group = 0;
    };

    // Takes the first queued job (lock held); null when none.
    std::unique_ptr<HdUpscaleJob> popLocked(std::string* key);
    void finishLocked(const std::string& key, int group, std::shared_ptr<HdImage> image);
    void workerMain();
    void trimCache();

    std::mutex mutex_;
    std::condition_variable workAvailable_;
    std::condition_variable jobDone_;
    // Queued jobs by (priority, submission order).
    std::map<std::pair<float, unsigned long long>, std::string> queue_;
    unsigned long long sequence_ = 0;
    std::vector<std::string> done_; // finished since the last drain
    std::unordered_map<std::string, Entry> entries_;
    int running_ = 0;
    int pendingByGroup_[kGroups] = {};
    int totalByGroup_[kGroups] = {};
    int doneByGroup_[kGroups] = {};
    bool stopping_ = false;
    std::vector<std::thread> workers_;
    std::string cacheDir_;
    unsigned long long cacheMaxBytes_ = 0;
    std::atomic<unsigned long long> cacheWrittenSinceTrim_ { 0 };
    std::thread trimmer_;
    HdUpscaleStats stats_;
};

} // namespace fallout

#endif /* FALLOUT_HD_UPSCALE_H_ */
