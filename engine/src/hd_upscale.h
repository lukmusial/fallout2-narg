#ifndef FALLOUT_HD_UPSCALE_H_
#define FALLOUT_HD_UPSCALE_H_

#include <stdint.h>

#include <condition_variable>
#include <deque>
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
// pixels 0xAARRGGBB (alpha 0 or 255).
void hdUpscaleScripted(const unsigned char* indices, int width, int height, int pitch, const unsigned char* palette8,
    bool tile, HdImage* dest);

// FNV-1a 64 of everything the output depends on (pixels, size, palette, tile
// flag, algorithm version): the disk cache key.
uint64_t hdUpscaleKey(const unsigned char* indices, int width, int height, int pitch, const unsigned char* palette8,
    bool tile);

// Disk cache files: <dir>/<xx>/<key>.rgba (raw 0xAARRGGBB after a small
// header). Safe from any thread (plain stdio, write to a temporary name and
// rename). Return false on a miss or error.
bool hdUpscaleCacheRead(const std::string& dir, uint64_t key, HdImage* image);
bool hdUpscaleCacheWrite(const std::string& dir, uint64_t key, const HdImage& image);

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
};

struct HdUpscaleStats {
    unsigned long long submitted = 0;
    unsigned long long computed = 0; // by workers
    unsigned long long computedInline = 0; // by a caller that needed it (taken from the queue)
    unsigned long long diskHits = 0;
    unsigned long long diskWrites = 0;
    unsigned long long waited = 0; // a caller waited for a running job
    double computeMs = 0; // summed over threads
    unsigned long long outputPixels = 0;
};

class HdUpscalePool {
public:
    HdUpscalePool() = default;
    ~HdUpscalePool();
    HdUpscalePool(const HdUpscalePool&) = delete;
    HdUpscalePool& operator=(const HdUpscalePool&) = delete;

    // threads <= 0: hardware threads - 1 (1..8). `cacheDir` empty: no disk
    // cache.
    void start(int threads, const std::string& cacheDir);
    void stop();
    bool running() const { return !workers_.empty(); }
    int threadCount() const { return static_cast<int>(workers_.size()); }

    // Queues a job unless one with that key is queued, running or done.
    void submit(HdUpscaleJob job);

    // Drops queued jobs (not running or done ones).
    void cancelQueued();

    // The result for `key`: done -> returned (and forgotten); queued -> run
    // on the calling thread now; running -> waits for it. Null when the pool
    // has never seen the key (or it failed).
    std::shared_ptr<HdImage> take(const std::string& key);

    // Moves every finished result out (key, image).
    void drain(const std::function<void(const std::string&, std::shared_ptr<HdImage>)>& sink);

    // Jobs queued or running.
    int pending();
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
    };

    void workerMain();

    std::mutex mutex_;
    std::condition_variable workAvailable_;
    std::condition_variable jobDone_;
    std::deque<std::string> queue_;
    std::unordered_map<std::string, Entry> entries_;
    int running_ = 0;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
    std::string cacheDir_;
    HdUpscaleStats stats_;
};

} // namespace fallout

#endif /* FALLOUT_HD_UPSCALE_H_ */
