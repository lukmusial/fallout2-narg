#include "hd_upscale.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/utime.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <utime.h>
#endif

namespace fallout {

namespace {

    constexpr int kCycleFirst = 229;
    constexpr int kCycleLast = 254;
    constexpr float kLumaR = 0.299f;
    constexpr float kLumaG = 0.587f;
    constexpr float kLumaB = 0.114f;
    constexpr float kNoiseFloor = 6.0f;
    constexpr float kDeditherStrength = 0.55f;

    inline int clampi(int value, int low, int high)
    {
        return value < low ? low : (value > high ? high : value);
    }

    inline bool isHole(unsigned char index)
    {
        return index == 0 || (index >= kCycleFirst && index <= kCycleLast);
    }

    // e^x for x <= 0 (relative error ~2e-7): 2^(x log2 e) split into an
    // integer power (exponent bits) and a polynomial for the fraction.
    // Branch-free, so the filter loops vectorise.
    inline float fastExp(float x)
    {
        x = x < -87.0f ? -87.0f : x;
        float t = x * 1.44269504089f;
        float fi = std::floor(t);
        float f = t - fi;
        float p = 1.0f + f * (0.693147182f + f * (0.240226507f + f * (0.0555041087f + f * (0.00961812911f + f * (0.00133335581f + f * 0.000154035304f)))));
        int32_t bits = (static_cast<int32_t>(fi) + 127) << 23;
        float scale;
        memcpy(&scale, &bits, sizeof(scale));
        return p * scale;
    }

    struct Planes {
        int width = 0;
        int height = 0;
        std::vector<float> r;
        std::vector<float> g;
        std::vector<float> b;

        void init(int w, int h)
        {
            width = w;
            height = h;
            size_t n = static_cast<size_t>(w) * h;
            r.assign(n, 0.0f);
            g.assign(n, 0.0f);
            b.assign(n, 0.0f);
        }

        void release()
        {
            std::vector<float>().swap(r);
            std::vector<float>().swap(g);
            std::vector<float>().swap(b);
        }
    };

    struct Tap {
        int dy;
        int dx;
        float weight;
    };

    // hdlib.wrap_pad_tile without context: the tile drawn on the isometric
    // lattice around itself (offsets from src/tile.cc), then itself on top.
    void wrapPadTile(const unsigned char* indices, int width, int height, int pitch, int pad, std::vector<unsigned char>* canvas,
        const HdTileContext* context)
    {
        int canvasWidth = width + 2 * pad;
        int canvasHeight = height + 2 * pad;
        canvas->assign(static_cast<size_t>(canvasWidth) * canvasHeight, 0);

        auto blit = [&](int x0, int y0, const unsigned char* source, int sourcePitch) {
            for (int y = 0; y < height; y++) {
                int cy = y0 + y;
                if (cy < 0 || cy >= canvasHeight) {
                    continue;
                }
                for (int x = 0; x < width; x++) {
                    int cx = x0 + x;
                    unsigned char index = source[static_cast<size_t>(y) * sourcePitch + x];
                    if (cx >= 0 && cx < canvasWidth && index != 0) {
                        (*canvas)[static_cast<size_t>(cy) * canvasWidth + cx] = index;
                    }
                }
            }
        };

        // Column step (+48, -12), row step (+32, +24) on screen.
        for (int r = -2; r <= 2; r++) {
            for (int c = -2; c <= 2; c++) {
                if (r == 0 && c == 0) {
                    continue;
                }
                const unsigned char* other = nullptr;
                if (context != nullptr && r >= -1 && r <= 1 && c >= -1 && c <= 1) {
                    other = context->neighbour[r + 1][c + 1];
                }
                if (other != nullptr) {
                    blit(pad + c * 48 + r * 32, pad + c * -12 + r * 24, other, width);
                } else {
                    blit(pad + c * 48 + r * 32, pad + c * -12 + r * 24, indices, pitch);
                }
            }
        }
        blit(pad, pad, indices, pitch);
    }

    // Per palette index: distance to the nearest different usable entry.
    void paletteSteps(const float* palette8, float* steps)
    {
        for (int i = 0; i < 256; i++) {
            float best = std::numeric_limits<float>::infinity();
            for (int j = 1; j < 256; j++) {
                if (j >= kCycleFirst && j <= kCycleLast) {
                    continue;
                }
                float dr = palette8[i * 3] - palette8[j * 3];
                float dg = palette8[i * 3 + 1] - palette8[j * 3 + 1];
                float db = palette8[i * 3 + 2] - palette8[j * 3 + 2];
                float d = std::sqrt(dr * dr + dg * dg + db * db);
                if (d > 0.0f && d < best) {
                    best = d;
                }
            }
            if (!std::isfinite(best)) {
                best = 16.0f;
            }
            steps[i] = std::min(std::max(best, 6.0f), 40.0f);
        }
    }

    // hdlib.fill_holes: hole pixels take the mean of their known 8-neighbours,
    // growing inward one ring per iteration (at most 64). The rings are the
    // Chebyshev distance from the known pixels, so a breadth-first order
    // gives the same result in one pass.
    void fillHoles(Planes& rgb, const std::vector<unsigned char>& holes)
    {
        int w = rgb.width;
        int h = rgb.height;
        size_t n = static_cast<size_t>(w) * h;
        size_t known = 0;
        for (size_t i = 0; i < n; i++) {
            known += holes[i] ? 0 : 1;
        }
        if (known == n || known == 0) {
            return;
        }

        constexpr int kMaxIter = 64;
        std::vector<int> dist(n, std::numeric_limits<int>::max());
        std::vector<int> order;
        order.reserve(n);
        for (size_t i = 0; i < n; i++) {
            if (!holes[i]) {
                dist[i] = 0;
                order.push_back(static_cast<int>(i));
            } else {
                rgb.r[i] = 0.0f;
                rgb.g[i] = 0.0f;
                rgb.b[i] = 0.0f;
            }
        }

        for (size_t head = 0; head < order.size(); head++) {
            int p = order[head];
            int py = p / w;
            int px = p % w;
            int level = dist[p];
            if (level >= kMaxIter) {
                continue;
            }
            for (int dy = -1; dy <= 1; dy++) {
                int y = py + dy;
                if (y < 0 || y >= h) {
                    continue;
                }
                for (int dx = -1; dx <= 1; dx++) {
                    int x = px + dx;
                    if (x < 0 || x >= w || (dx == 0 && dy == 0)) {
                        continue;
                    }
                    size_t q = static_cast<size_t>(y) * w + x;
                    if (dist[q] == std::numeric_limits<int>::max()) {
                        dist[q] = level + 1;
                        order.push_back(static_cast<int>(q));
                    }
                }
            }
        }

        for (size_t head = known; head < order.size(); head++) {
            int p = order[head];
            int py = p / w;
            int px = p % w;
            int level = dist[p];
            float sr = 0.0f;
            float sg = 0.0f;
            float sb = 0.0f;
            int count = 0;
            for (int dy = -1; dy <= 1; dy++) {
                int y = py + dy;
                if (y < 0 || y >= h) {
                    continue;
                }
                for (int dx = -1; dx <= 1; dx++) {
                    int x = px + dx;
                    if (x < 0 || x >= w || (dx == 0 && dy == 0)) {
                        continue;
                    }
                    size_t q = static_cast<size_t>(y) * w + x;
                    if (dist[q] < level) {
                        sr += rgb.r[q];
                        sg += rgb.g[q];
                        sb += rgb.b[q];
                        count++;
                    }
                }
            }
            if (count > 0) {
                rgb.r[p] = sr / count;
                rgb.g[p] = sg / count;
                rgb.b[p] = sb / count;
            }
        }
    }

    // Range-limited (bilateral) filter with edge replication:
    // acc = sum w * n, w = tap weight * exp(-|n - c|^2 * invS2(c)) * valid(n).
    // `keepInvalid` (de-dither): pixels that are not valid keep their value;
    // otherwise (gradient restoration) pixels with no weight do.
#if defined(__GNUC__) || defined(__clang__)
#define HD_UPSCALE_SIMD 1
    // 4 floats: NEON on ARM, SSE2 on x86 (GCC/Clang vector extensions).
    typedef float V4 __attribute__((vector_size(16)));
    typedef int32_t I4 __attribute__((vector_size(16)));

    inline V4 load4(const float* p)
    {
        V4 v;
        memcpy(&v, p, sizeof(v));
        return v;
    }

    inline void store4(float* p, V4 v)
    {
        memcpy(p, &v, sizeof(v));
    }

    inline V4 splat4(float x)
    {
        return V4 { x, x, x, x };
    }

    // e^x for x <= 0, 4 at a time: 2^(x log2 e) with the integer part from
    // round-to-nearest (the 1.5 * 2^23 trick, no floor) and a degree-6
    // polynomial on [-0.5, 0.5] (relative error < 1e-7).
    inline V4 fastExp4(V4 x)
    {
        const V4 lowest = splat4(-87.0f);
        x = x < lowest ? lowest : x;
        V4 t = x * 1.44269504089f;
        const V4 magic = splat4(12582912.0f);
        V4 r = t + magic;
        I4 n = (I4)r - (I4)magic;
        V4 f = t - (r - magic);
        V4 p = 1.0f + f * (0.693147182f + f * (0.240226507f + f * (0.0555041087f + f * (0.00961812911f + f * (0.00133335581f + f * 0.000154035304f)))));
        I4 bits = (n + 127) << 23;
        return p * (V4)bits;
    }

    // All taps for 4 pixels at a time in registers (same tap order per
    // pixel as the scalar loops), for columns whose neighbours are all in
    // the row. Returns the first column not done.
    int bilateralInteriorSimd(int x0, int x1, const float* cr, const float* cg, const float* cb, const float* inv,
        const float* const* rowR, const float* const* rowG, const float* const* rowB, const float* const* rowV,
        const int* dx, const float* weights, int count, float* ar, float* ag, float* ab, float* ws)
    {
        int x = x0;
        for (; x + 4 <= x1; x += 4) {
            V4 sr = splat4(0.0f);
            V4 sg = splat4(0.0f);
            V4 sb = splat4(0.0f);
            V4 sw = splat4(0.0f);
            V4 r = load4(cr + x);
            V4 g = load4(cg + x);
            V4 b = load4(cb + x);
            V4 k = -load4(inv + x);
            for (int t = 0; t < count; t++) {
                int at = x + dx[t];
                V4 nr = load4(rowR[t] + at);
                V4 ng = load4(rowG[t] + at);
                V4 nb = load4(rowB[t] + at);
                V4 dr = nr - r;
                V4 dg = ng - g;
                V4 db = nb - b;
                V4 d2 = dr * dr + dg * dg + db * db;
                V4 wt = weights[t] * fastExp4(d2 * k) * load4(rowV[t] + at);
                sr += wt * nr;
                sg += wt * ng;
                sb += wt * nb;
                sw += wt;
            }
            store4(ar + x, sr);
            store4(ag + x, sg);
            store4(ab + x, sb);
            store4(ws + x, sw);
        }
        return x;
    }
#endif

    void bilateral(const Planes& in, const float* invS2, const float* valid, const std::vector<Tap>& taps, bool keepInvalid, Planes& out)
    {
        int w = in.width;
        int h = in.height;
        out.init(w, h);
        std::vector<float> accR(w);
        std::vector<float> accG(w);
        std::vector<float> accB(w);
        std::vector<float> wsum(w);

        for (int y = 0; y < h; y++) {
            std::fill(accR.begin(), accR.end(), 0.0f);
            std::fill(accG.begin(), accG.end(), 0.0f);
            std::fill(accB.begin(), accB.end(), 0.0f);
            std::fill(wsum.begin(), wsum.end(), 0.0f);

            size_t row = static_cast<size_t>(y) * w;
            const float* cr = in.r.data() + row;
            const float* cg = in.g.data() + row;
            const float* cb = in.b.data() + row;
            const float* inv = invS2 + row;
            float* ar = accR.data();
            float* ag = accG.data();
            float* ab = accB.data();
            float* ws = wsum.data();

            // Interior columns: all taps per 4 pixels in registers (SIMD).
            int radius = 0;
            for (const Tap& tap : taps) {
                radius = std::max(radius, std::abs(tap.dx));
            }
            int simdFrom = w;
            int simdTo = w;
#ifdef HD_UPSCALE_SIMD
            if (w > 2 * radius + 4) {
                constexpr int kMaxTaps = 25;
                const float* rowR[kMaxTaps];
                const float* rowG[kMaxTaps];
                const float* rowB[kMaxTaps];
                const float* rowV[kMaxTaps];
                int dxs[kMaxTaps];
                float weights[kMaxTaps];
                int count = std::min(static_cast<int>(taps.size()), kMaxTaps);
                for (int t = 0; t < count; t++) {
                    size_t nrow = static_cast<size_t>(clampi(y + taps[t].dy, 0, h - 1)) * w;
                    rowR[t] = in.r.data() + nrow;
                    rowG[t] = in.g.data() + nrow;
                    rowB[t] = in.b.data() + nrow;
                    rowV[t] = valid + nrow;
                    dxs[t] = taps[t].dx;
                    weights[t] = taps[t].weight;
                }
                simdFrom = radius;
                simdTo = bilateralInteriorSimd(radius, w - radius, cr, cg, cb, inv, rowR, rowG, rowB, rowV, dxs, weights, count,
                    ar, ag, ab, ws);
            }
#endif

            // Elsewhere (edges, the remainder, or without SIMD) tap by tap
            // over the row (the inner loop vectorises).
            for (const Tap& tap : taps) {
                size_t nrow = static_cast<size_t>(clampi(y + tap.dy, 0, h - 1)) * w;
                const float* nr = in.r.data() + nrow;
                const float* ng = in.g.data() + nrow;
                const float* nb = in.b.data() + nrow;
                const float* nv = valid + nrow;
                float weight = tap.weight;
                int dx = tap.dx;

                auto edge = [&](int x) {
                    int xi = clampi(x + dx, 0, w - 1);
                    float dr = nr[xi] - cr[x];
                    float dg = ng[xi] - cg[x];
                    float db = nb[xi] - cb[x];
                    float d2 = dr * dr + dg * dg + db * db;
                    float wt = weight * fastExp(-d2 * inv[x]) * nv[xi];
                    ar[x] += wt * nr[xi];
                    ag[x] += wt * ng[xi];
                    ab[x] += wt * nb[xi];
                    ws[x] += wt;
                };

                int lo = std::min(std::max(0, -dx), w);
                int hi = std::max(std::min(w, w - dx), lo);
                auto inner = [&](int from, int to) {
                    for (int x = from; x < to; x++) {
                        float dr = nr[x + dx] - cr[x];
                        float dg = ng[x + dx] - cg[x];
                        float db = nb[x + dx] - cb[x];
                        float d2 = dr * dr + dg * dg + db * db;
                        float wt = weight * fastExp(-d2 * inv[x]) * nv[x + dx];
                        ar[x] += wt * nr[x + dx];
                        ag[x] += wt * ng[x + dx];
                        ab[x] += wt * nb[x + dx];
                        ws[x] += wt;
                    }
                };
                for (int x = 0; x < lo; x++) {
                    if (x < simdFrom || x >= simdTo) {
                        edge(x);
                    }
                }
                inner(lo, std::max(lo, std::min(hi, simdFrom)));
                inner(std::max(lo, simdTo), hi);
                for (int x = hi; x < w; x++) {
                    if (x < simdFrom || x >= simdTo) {
                        edge(x);
                    }
                }
            }

            const float* cv = valid + row;
            float* orr = out.r.data() + row;
            float* og = out.g.data() + row;
            float* ob = out.b.data() + row;
            for (int x = 0; x < w; x++) {
                bool use = keepInvalid ? cv[x] != 0.0f : ws[x] > 1e-6f;
                if (use) {
                    float inverse = 1.0f / std::max(ws[x], 1e-6f);
                    orr[x] = ar[x] * inverse;
                    og[x] = ag[x] * inverse;
                    ob[x] = ab[x] * inverse;
                } else {
                    orr[x] = cr[x];
                    og[x] = cg[x];
                    ob[x] = cb[x];
                }
            }
        }
    }

    const std::vector<Tap>& deditherTaps()
    {
        static const std::vector<Tap> taps = [] {
            std::vector<Tap> list;
            list.push_back({ 0, 0, 1.0f });
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dy != 0 || dx != 0) {
                        list.push_back({ dy, dx, (dy == 0 || dx == 0) ? 0.6f : 0.35f });
                    }
                }
            }
            return list;
        }();
        return taps;
    }

    const std::vector<Tap>& restoreTaps()
    {
        static const std::vector<Tap> taps = [] {
            std::vector<Tap> list;
            for (int dy = -2; dy <= 2; dy++) {
                for (int dx = -2; dx <= 2; dx++) {
                    list.push_back({ dy, dx, static_cast<float>(std::exp(-(dy * dy + dx * dx) / (2.0 * 1.6 * 1.6))) });
                }
            }
            return list;
        }();
        return taps;
    }

    inline float cubic(float a, float b, float c, float d)
    {
        return (-a + 9.0f * b + 9.0f * c - d) / 16.0f;
    }

    // _blend: p1 along direction 1 (gradient sum g1), p2 along 2: the one
    // along the smaller gradient, or a mix when they are close; clamped.
    inline float blendWeight(float g1, float g2, int* pick)
    {
        g1 += kNoiseFloor;
        g2 += kNoiseFloor;
        float r = g1 / g2;
        if (r < 1.0f / 1.15f) {
            *pick = 1;
            return 1.0f;
        }
        if (r > 1.15f) {
            *pick = 2;
            return 0.0f;
        }
        *pick = 0;
        float r5 = r * r * r * r * r;
        return 1.0f / (1.0f + r5);
    }

    inline float blendValue(float p1, float p2, float a, float lo, float hi)
    {
        float v = a * p1 + (1.0f - a) * p2;
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // dcci2x: 2x of `base`, edge directions measured on `guide`.
    void dcci2x(const Planes& base, const Planes& guide, Planes& out)
    {
        int w = base.width;
        int h = base.height;
        size_t n = static_cast<size_t>(w) * h;
        auto lumaScale = [](float r, float g, float b) {
            return (r * kLumaR + g * kLumaG + b * kLumaB) / 255.0f * 32.0f;
        };

        std::vector<float> lowY(n);
        for (size_t i = 0; i < n; i++) {
            lowY[i] = lumaScale(guide.r[i], guide.g[i], guide.b[i]);
        }

        auto at = [&](int y, int x) {
            return static_cast<size_t>(clampi(y, 0, h - 1)) * w + clampi(x, 0, w - 1);
        };

        // Pass 1: centre of each 2x2 cell -> HR (2y+1, 2x+1); rgb and the
        // guide's luma.
        Planes centre;
        centre.init(w, h);
        std::vector<float> centreY(n);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                float g45 = 0.0f;
                float g135 = 0.0f;
                for (int r = -1; r <= 2; r++) {
                    for (int c = -1; c <= 2; c++) {
                        if (r - 1 >= -1 && c + 1 <= 2) {
                            g45 += std::fabs(lowY[at(y + r, x + c)] - lowY[at(y + r - 1, x + c + 1)]);
                        }
                        if (r + 1 <= 2 && c + 1 <= 2) {
                            g135 += std::fabs(lowY[at(y + r, x + c)] - lowY[at(y + r + 1, x + c + 1)]);
                        }
                    }
                }
                int pick = 0;
                float a = blendWeight(g45, g135, &pick);

                size_t d0 = at(y - 1, x - 1), d1 = at(y, x), d2 = at(y + 1, x + 1), d3 = at(y + 2, x + 2);
                size_t e0 = at(y + 2, x - 1), e1 = at(y + 1, x), e2 = at(y, x + 1), e3 = at(y - 1, x + 2);
                size_t c00 = d1, c01 = at(y, x + 1), c10 = at(y + 1, x), c11 = d2;

                auto channel = [&](const std::vector<float>& p) {
                    float p135 = cubic(p[d0], p[d1], p[d2], p[d3]);
                    float p45 = cubic(p[e0], p[e1], p[e2], p[e3]);
                    float lo = std::min(std::min(p[c00], p[c01]), std::min(p[c10], p[c11]));
                    float hi = std::max(std::max(p[c00], p[c01]), std::max(p[c10], p[c11]));
                    return blendValue(p45, p135, a, lo, hi);
                };

                size_t i = static_cast<size_t>(y) * w + x;
                centre.r[i] = channel(base.r);
                centre.g[i] = channel(base.g);
                centre.b[i] = channel(base.b);
                centreY[i] = lumaScale(channel(guide.r), channel(guide.g), channel(guide.b));
            }
        }

        // The HR grid: known at (even, even) (the source) and (odd, odd)
        // (centres). Pass 2 reads it padded by 4 with edge replication; as
        // in the Python, padding a grid with holes breaks the parity on the
        // border, and holes are then filled from the right, left, lower and
        // upper neighbour in turn. `source` holds, per padded pixel, the HR
        // pixel its value comes from (-1: none, 0).
        int hh = 2 * h;
        int ww = 2 * w;
        constexpr int Q = 4;
        int ph = hh + 2 * Q;
        int pw = ww + 2 * Q;
        size_t pn = static_cast<size_t>(ph) * pw;
        std::vector<int> source(pn);
        for (int py = 0; py < ph; py++) {
            int qy = clampi(py - Q, 0, hh - 1);
            for (int px = 0; px < pw; px++) {
                int qx = clampi(px - Q, 0, ww - 1);
                source[static_cast<size_t>(py) * pw + px] = ((qy + qx) % 2 == 0) ? qy * ww + qx : -1;
            }
        }
        {
            // Each pass reads the state before it: scanning away from the
            // neighbour read keeps that true in place.
            auto fill = [&](int py, int px, int sy, int sx) {
                int& at = source[static_cast<size_t>(py) * pw + px];
                if (at == -1) {
                    at = source[static_cast<size_t>(sy) * pw + sx];
                }
            };
            for (int py = 0; py < ph; py++) {
                for (int px = 0; px < pw; px++) {
                    fill(py, px, py, std::min(px + 1, pw - 1));
                }
                for (int px = pw - 1; px >= 0; px--) {
                    fill(py, px, py, std::max(px - 1, 0));
                }
            }
            for (int py = 0; py < ph; py++) {
                int sy = std::min(py + 1, ph - 1);
                for (int px = 0; px < pw; px++) {
                    fill(py, px, sy, px);
                }
            }
            for (int py = ph - 1; py >= 0; py--) {
                int sy = std::max(py - 1, 0);
                for (int px = 0; px < pw; px++) {
                    fill(py, px, sy, px);
                }
            }
        }

        // The known HR pixels, then the padded planes (r, g, b, guide luma).
        size_t hn = static_cast<size_t>(hh) * ww;
        std::vector<float> hr(hn * 4, 0.0f);
        for (int y = 0; y < hh; y++) {
            const Planes& from = (y & 1) == 0 ? base : centre;
            const float* fromY = (y & 1) == 0 ? lowY.data() : centreY.data();
            size_t lrow = static_cast<size_t>(y / 2) * w;
            for (int x = y & 1; x < ww; x += 2) {
                size_t li = lrow + x / 2;
                float* to = hr.data() + (static_cast<size_t>(y) * ww + x) * 4;
                to[0] = from.r[li];
                to[1] = from.g[li];
                to[2] = from.b[li];
                to[3] = fromY[li];
            }
        }
        std::vector<float> pr(pn), pg(pn), pb(pn), py_(pn);
        for (size_t i = 0; i < pn; i++) {
            int q = source[i];
            if (q < 0) {
                pr[i] = pg[i] = pb[i] = py_[i] = 0.0f;
                continue;
            }
            const float* from = hr.data() + static_cast<size_t>(q) * 4;
            pr[i] = from[0];
            pg[i] = from[1];
            pb[i] = from[2];
            py_[i] = from[3];
        }
        std::vector<int>().swap(source);
        std::vector<float>().swap(hr);

        auto pidx = [&](int y, int x) {
            return static_cast<size_t>(y + Q) * pw + (x + Q);
        };

        std::vector<float> dh(hn), dv(hn);
        for (int y = 0; y < hh; y++) {
            const float* row = py_.data() + pidx(y, 0);
            const float* up = py_.data() + pidx(y - 1, 0);
            const float* down = py_.data() + pidx(y + 1, 0);
            float* oh = dh.data() + static_cast<size_t>(y) * ww;
            float* ov = dv.data() + static_cast<size_t>(y) * ww;
            for (int x = 0; x < ww; x++) {
                oh[x] = std::fabs(row[x - 1] - row[x + 1]);
                ov[x] = std::fabs(up[x] - down[x]);
            }
        }

        // Gradient sums over the 13 offsets of [-2, 2]^2 with an even
        // dy + dx (edge replicated): rows dy = 0, +-2 take dx = 0, +-2, rows
        // dy = +-1 take dx = +-1; row sums first.
        std::vector<float> evenH(hn), oddH(hn), evenV(hn), oddV(hn);
        for (int y = 0; y < hh; y++) {
            size_t row = static_cast<size_t>(y) * ww;
            for (int x = 0; x < ww; x++) {
                int l2 = clampi(x - 2, 0, ww - 1);
                int l1 = clampi(x - 1, 0, ww - 1);
                int r1 = clampi(x + 1, 0, ww - 1);
                int r2 = clampi(x + 2, 0, ww - 1);
                evenH[row + x] = dh[row + l2] + dh[row + x] + dh[row + r2];
                oddH[row + x] = dh[row + l1] + dh[row + r1];
                evenV[row + x] = dv[row + l2] + dv[row + x] + dv[row + r2];
                oddV[row + x] = dv[row + l1] + dv[row + r1];
            }
        }
        std::vector<float>().swap(dh);
        std::vector<float>().swap(dv);

        out.init(ww, hh);
        for (int y = 0; y < hh; y++) {
            size_t rm2 = static_cast<size_t>(clampi(y - 2, 0, hh - 1)) * ww;
            size_t rm1 = static_cast<size_t>(clampi(y - 1, 0, hh - 1)) * ww;
            size_t r0 = static_cast<size_t>(y) * ww;
            size_t rp1 = static_cast<size_t>(clampi(y + 1, 0, hh - 1)) * ww;
            size_t rp2 = static_cast<size_t>(clampi(y + 2, 0, hh - 1)) * ww;
            for (int x = 0; x < ww; x++) {
                size_t o = r0 + x;
                if (((y + x) & 1) == 0) {
                    size_t i = pidx(y, x);
                    out.r[o] = pr[i];
                    out.g[o] = pg[i];
                    out.b[o] = pb[i];
                    continue;
                }

                float gh = evenH[rm2 + x] + oddH[rm1 + x] + evenH[r0 + x] + oddH[rp1 + x] + evenH[rp2 + x];
                float gv = evenV[rm2 + x] + oddV[rm1 + x] + evenV[r0 + x] + oddV[rp1 + x] + evenV[rp2 + x];

                int pick = 0;
                float a = blendWeight(gh, gv, &pick);
                size_t h0 = pidx(y, x - 3), h1 = pidx(y, x - 1), h2 = pidx(y, x + 1), h3 = pidx(y, x + 3);
                size_t v0 = pidx(y - 3, x), v1 = pidx(y - 1, x), v2 = pidx(y + 1, x), v3 = pidx(y + 3, x);
                auto channel = [&](const std::vector<float>& p) {
                    float phv = cubic(p[h0], p[h1], p[h2], p[h3]);
                    float pvv = cubic(p[v0], p[v1], p[v2], p[v3]);
                    float lo = std::min(std::min(p[h1], p[h2]), std::min(p[v1], p[v2]));
                    float hi = std::max(std::max(p[h1], p[h2]), std::max(p[v1], p[v2]));
                    return blendValue(phv, pvv, a, lo, hi);
                };
                out.r[o] = channel(pr);
                out.g[o] = channel(pg);
                out.b[o] = channel(pb);
            }
        }
    }

    // restore_gradients: 5x5 range-limited smoothing (range ~ one palette
    // step), then an unsharp mask where the local contrast is large.
    void restoreGradients(Planes& big, const float* invS2, const float* valid)
    {
        int w = big.width;
        int h = big.height;
        Planes smooth;
        bilateral(big, invS2, valid, restoreTaps(), false, smooth);

        // 3x3 box mean, edge replicated (separable).
        size_t n = static_cast<size_t>(w) * h;
        auto lumaAt = [&](size_t i) {
            return smooth.r[i] * kLumaR + smooth.g[i] * kLumaG + smooth.b[i] * kLumaB;
        };

        std::vector<float> rowSum(static_cast<size_t>(w) * 3);
        for (int y = 0; y < h; y++) {
            size_t up = static_cast<size_t>(clampi(y - 1, 0, h - 1)) * w;
            size_t mid = static_cast<size_t>(y) * w;
            size_t down = static_cast<size_t>(clampi(y + 1, 0, h - 1)) * w;
            for (int c = 0; c < 3; c++) {
                const std::vector<float>& p = c == 0 ? smooth.r : (c == 1 ? smooth.g : smooth.b);
                float* sum = rowSum.data() + static_cast<size_t>(c) * w;
                for (int x = 0; x < w; x++) {
                    sum[x] = p[up + x] + p[mid + x] + p[down + x];
                }
            }
            for (int x = 0; x < w; x++) {
                int xl = clampi(x - 1, 0, w - 1);
                int xr = clampi(x + 1, 0, w - 1);
                size_t i = mid + x;
                float edge = std::fabs(lumaAt(mid + xr) - lumaAt(mid + xl)) + std::fabs(lumaAt(down + x) - lumaAt(up + x));
                float amount = 0.45f * std::min(std::max((edge - 12.0f) / 40.0f, 0.0f), 1.0f);
                for (int c = 0; c < 3; c++) {
                    const float* sum = rowSum.data() + static_cast<size_t>(c) * w;
                    const std::vector<float>& s = c == 0 ? smooth.r : (c == 1 ? smooth.g : smooth.b);
                    std::vector<float>& dst = c == 0 ? big.r : (c == 1 ? big.g : big.b);
                    float blur = (sum[xl] + sum[x] + sum[xr]) / 9.0f;
                    dst[i] = s[i] + amount * (s[i] - blur);
                }
            }
        }
        (void)n;
    }

    // Ordered dither thresholds (4x4 Bayer), in levels, mean 0.
    constexpr float kBayer4[16] = {
        -0.46875f, 0.03125f, -0.34375f, 0.15625f,
        0.28125f, -0.21875f, 0.40625f, -0.09375f,
        -0.28125f, 0.21875f, -0.40625f, 0.09375f,
        0.46875f, -0.03125f, 0.34375f, -0.15625f,
    };

    // De-banding (precise mode): the 256-colour quantisation leaves smooth
    // areas as bands one palette step apart. A separable, range-limited
    // tent filter (radius 8 at 2x, horizontal then vertical, twice) averages
    // only neighbours within about one palette step of the pixel, so each
    // band boundary becomes a ramp while edges (jumps of several steps) and
    // texture stay. `steps`: the palette step per pixel; `valid`: 0 on holes.
    void deband(Planes& big, const float* steps, const float* valid)
    {
        // At source resolution (bands are features of the 1x picture):
        // radius 4 here is 8 at 2x.
        constexpr int R = 4;
        constexpr int A = 2; // activity window radius
        int w = big.width;
        int h = big.height;
        size_t n = static_cast<size_t>(w) * h;
        Planes tmp;
        tmp.init(w, h);
        std::vector<float> invT(n);
        for (size_t i = 0; i < n; i++) {
            float t = 1.6f * steps[i];
            invT[i] = 1.0f / (t * t);
        }
        // Where it applies: banded areas are flat between isolated one-step
        // edges, textured ones change at nearly every pixel. Activity = mean
        // luma gradient over 5x5 pixels, in palette steps; full effect below
        // kFlat, none above kTextured.
        constexpr float kFlat = 0.10f;
        constexpr float kTextured = 0.22f;
        std::vector<float> amount(n);
        {
            std::vector<float> luma(n);
            for (size_t i = 0; i < n; i++) {
                luma[i] = big.r[i] * kLumaR + big.g[i] * kLumaG + big.b[i] * kLumaB;
            }
            std::vector<float> grad(n);
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    size_t i = static_cast<size_t>(y) * w + x;
                    float gx = luma[static_cast<size_t>(y) * w + clampi(x + 1, 0, w - 1)] - luma[static_cast<size_t>(y) * w + clampi(x - 1, 0, w - 1)];
                    float gy = luma[static_cast<size_t>(clampi(y + 1, 0, h - 1)) * w + x] - luma[static_cast<size_t>(clampi(y - 1, 0, h - 1)) * w + x];
                    grad[i] = 0.5f * (std::fabs(gx) + std::fabs(gy));
                }
            }
            // 5x5 box mean, separable.
            std::vector<float> rows(n);
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    float sum = 0.0f;
                    for (int d = -A; d <= A; d++) {
                        sum += grad[static_cast<size_t>(y) * w + clampi(x + d, 0, w - 1)];
                    }
                    rows[static_cast<size_t>(y) * w + x] = sum;
                }
            }
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    float sum = 0.0f;
                    for (int d = -A; d <= A; d++) {
                        sum += rows[static_cast<size_t>(clampi(y + d, 0, h - 1)) * w + x];
                    }
                    size_t i = static_cast<size_t>(y) * w + x;
                    float activity = sum / static_cast<float>((2 * A + 1) * (2 * A + 1)) / std::max(steps[i], 1.0f);
                    amount[i] = std::min(std::max((kTextured - activity) / (kTextured - kFlat), 0.0f), 1.0f);
                }
            }
        }
        Planes original = big;

        std::vector<float> accR(w);
        std::vector<float> accG(w);
        std::vector<float> accB(w);
        std::vector<float> wsum(w);
        for (int iteration = 0; iteration < 2; iteration++) {
            for (int axis = 0; axis < 2; axis++) {
                const Planes& in = big;
                for (int y = 0; y < h; y++) {
                    size_t row = static_cast<size_t>(y) * w;
                    const float* cr = in.r.data() + row;
                    const float* cg = in.g.data() + row;
                    const float* cb = in.b.data() + row;
                    const float* k = invT.data() + row;
                    float* ar = accR.data();
                    float* ag = accG.data();
                    float* ab = accB.data();
                    float* ws = wsum.data();
                    std::fill(accR.begin(), accR.end(), 0.0f);
                    std::fill(accG.begin(), accG.end(), 0.0f);
                    std::fill(accB.begin(), accB.end(), 0.0f);
                    std::fill(wsum.begin(), wsum.end(), 0.0f);
                    // Tap by tap over the row (vectorises); horizontal taps
                    // clamp at the row ends, vertical ones at the image's.
                    for (int d = -R; d <= R; d++) {
                        float tent = static_cast<float>(R + 1 - (d < 0 ? -d : d));
                        int dx = axis == 0 ? d : 0;
                        size_t nrow = axis == 0 ? row : static_cast<size_t>(clampi(y + d, 0, h - 1)) * w;
                        const float* nr = in.r.data() + nrow;
                        const float* ng = in.g.data() + nrow;
                        const float* nb = in.b.data() + nrow;
                        const float* nv = valid + nrow;
                        auto tap = [&](int x, int xi) {
                            float dr = nr[xi] - cr[x];
                            float dg = ng[xi] - cg[x];
                            float db = nb[xi] - cb[x];
                            float range = 1.0f - (dr * dr + dg * dg + db * db) * k[x];
                            range = range > 0.0f ? range * range : 0.0f;
                            float wt = tent * range * nv[xi];
                            ar[x] += wt * nr[xi];
                            ag[x] += wt * ng[xi];
                            ab[x] += wt * nb[xi];
                            ws[x] += wt;
                        };
                        int lo = std::min(std::max(0, -dx), w);
                        int hi = std::max(std::min(w, w - dx), lo);
                        for (int x = 0; x < lo; x++) {
                            tap(x, clampi(x + dx, 0, w - 1));
                        }
                        for (int x = lo; x < hi; x++) {
                            float dr = nr[x + dx] - cr[x];
                            float dg = ng[x + dx] - cg[x];
                            float db = nb[x + dx] - cb[x];
                            float range = 1.0f - (dr * dr + dg * dg + db * db) * k[x];
                            range = range > 0.0f ? range * range : 0.0f;
                            float wt = tent * range * nv[x + dx];
                            ar[x] += wt * nr[x + dx];
                            ag[x] += wt * ng[x + dx];
                            ab[x] += wt * nb[x + dx];
                            ws[x] += wt;
                        }
                        for (int x = hi; x < w; x++) {
                            tap(x, clampi(x + dx, 0, w - 1));
                        }
                    }
                    const float* cv = valid + row;
                    for (int x = 0; x < w; x++) {
                        size_t c = row + x;
                        if (ws[x] > 1e-6f && cv[x] != 0.0f) {
                            float inverse = 1.0f / ws[x];
                            tmp.r[c] = ar[x] * inverse;
                            tmp.g[c] = ag[x] * inverse;
                            tmp.b[c] = ab[x] * inverse;
                        } else {
                            tmp.r[c] = cr[x];
                            tmp.g[c] = cg[x];
                            tmp.b[c] = cb[x];
                        }
                    }
                }
                std::swap(big.r, tmp.r);
                std::swap(big.g, tmp.g);
                std::swap(big.b, tmp.b);
            }
        }
        for (size_t i = 0; i < n; i++) {
            float a = amount[i];
            big.r[i] = original.r[i] + a * (big.r[i] - original.r[i]);
            big.g[i] = original.g[i] + a * (big.g[i] - original.g[i]);
            big.b[i] = original.b[i] + a * (big.b[i] - original.b[i]);
        }
    }

} // namespace

namespace {

    // Per-stage time (ns), summed over all threads (hdUpscaleStageTimes).
    std::atomic<unsigned long long> gStageNs[kHdUpscaleStages];

    struct StageClock {
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        void mark(int stage)
        {
            auto now = std::chrono::steady_clock::now();
            gStageNs[stage].fetch_add(static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count()),
                std::memory_order_relaxed);
            last = now;
        }
    };

    void upscaleWhole(const unsigned char* indices, int width, int height, int pitch, const unsigned char* palette8,
        bool tile, bool precise, HdImage* dest, const HdTileContext* context = nullptr);

    // Rows of source context each strip gets above and below: more than
    // every stage reaches (de-dither 3, DCCI 2 + 3/2, restoration and
    // unsharp 3/2, grain 1, de-banding 2 x 8/2; hole filling near opaque
    // pixels likewise), so the strips' own rows come out exactly as in one
    // piece. Even, so the ordered dither's phase is kept.
    constexpr int kStripMargin = 32;

} // namespace

void hdUpscaleScripted(const unsigned char* indices, int width, int height, int pitch, const unsigned char* palette8,
    bool tile, HdImage* dest, int threads, bool precise, const HdTileContext* context)
{
    if (threads <= 1 || tile || static_cast<long long>(width) * height < 40000 || height < 4 * kStripMargin) {
        upscaleWhole(indices, width, height, pitch, palette8, tile, precise, dest, context);
        return;
    }

    // Horizontal strips with margins, one thread each.
    int strips = std::min(threads, std::max(1, height / (2 * kStripMargin)));
    dest->width = width * kHdScale;
    dest->height = height * kHdScale;
    dest->pixels.assign(static_cast<size_t>(dest->width) * dest->height, 0);
    std::vector<std::thread> workers;
    for (int strip = 0; strip < strips; strip++) {
        int y0 = (height * strip / strips) & ~1;
        int y1 = strip == strips - 1 ? height : (height * (strip + 1) / strips) & ~1;
        workers.emplace_back([=]() {
            int top = std::max(0, y0 - kStripMargin);
            int bottom = std::min(height, y1 + kStripMargin);
            HdImage part;
            upscaleWhole(indices + static_cast<size_t>(top) * pitch, width, bottom - top, pitch, palette8, false, precise, &part);
            size_t rowPixels = static_cast<size_t>(dest->width);
            memcpy(dest->pixels.data() + static_cast<size_t>(y0) * kHdScale * rowPixels,
                part.pixels.data() + static_cast<size_t>(y0 - top) * kHdScale * rowPixels,
                static_cast<size_t>(y1 - y0) * kHdScale * rowPixels * sizeof(uint32_t));
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
}

namespace {

void upscaleWhole(const unsigned char* indices, int width, int height, int pitch, const unsigned char* palette8,
    bool tile, bool precise, HdImage* dest, const HdTileContext* context)
{
    dest->width = width * kHdScale;
    dest->height = height * kHdScale;
    dest->pixels.assign(static_cast<size_t>(dest->width) * dest->height, 0);
    if (width <= 0 || height <= 0) {
        return;
    }

    // Work image (tiles: padded with lattice copies of themselves).
    std::vector<unsigned char> work;
    int w = width;
    int h = height;
    int pad = 0;
    if (tile) {
        pad = kHdUpscaleTilePad;
        wrapPadTile(indices, width, height, pitch, pad, &work, context);
        w = width + 2 * pad;
        h = height + 2 * pad;
    } else {
        work.resize(static_cast<size_t>(w) * h);
        for (int y = 0; y < h; y++) {
            memcpy(work.data() + static_cast<size_t>(y) * w, indices + static_cast<size_t>(y) * pitch, w);
        }
    }

    // Palette: 8-bit entries as the Python uses them, or (precise) the
    // game's 6-bit values expanded at full precision, v * 255 / 63, kept as
    // floats through every step (finer than 16 bits per channel).
    float palette[256 * 3];
    for (int i = 0; i < 256 * 3; i++) {
        palette[i] = precise ? (palette8[i] >> 2) * (255.0f / 63.0f) : static_cast<float>(palette8[i]);
    }

    size_t n = static_cast<size_t>(w) * h;
    std::vector<unsigned char> holes(n);
    std::vector<float> valid(n);
    Planes rgb;
    rgb.init(w, h);
    for (size_t i = 0; i < n; i++) {
        unsigned char index = work[i];
        holes[i] = isHole(index) ? 1 : 0;
        valid[i] = holes[i] ? 0.0f : 1.0f;
        rgb.r[i] = palette[index * 3];
        rgb.g[i] = palette[index * 3 + 1];
        rgb.b[i] = palette[index * 3 + 2];
    }
    StageClock clock;
    fillHoles(rgb, holes);

    float steps[256];
    paletteSteps(palette, steps);
    std::vector<float> invS2(n);
    for (size_t i = 0; i < n; i++) {
        float sigma = 1.1f * steps[work[i]];
        invS2[i] = 1.0f / (2.0f * sigma * sigma);
    }

    // Base: fully de-dithered (two passes); guide: one more pass.
    Planes pass1;
    Planes base;
    Planes guide;
    clock.mark(0);
    bilateral(rgb, invS2.data(), valid.data(), deditherTaps(), true, pass1);
    bilateral(pass1, invS2.data(), valid.data(), deditherTaps(), true, base);
    pass1.release();
    // Precise: de-banding of the de-dithered base; the grain is still what
    // the de-dither removed from the source (`grainBase`).
    Planes debandedFrom;
    if (precise) {
        debandedFrom = base;
        std::vector<float> stepsLow(n);
        for (size_t i = 0; i < n; i++) {
            stepsLow[i] = steps[work[i]];
        }
        clock.mark(1);
        deband(base, stepsLow.data(), valid.data());
        clock.mark(4);
    }
    const Planes& grainBase = precise ? debandedFrom : base;
    bilateral(base, invS2.data(), valid.data(), deditherTaps(), true, guide);

    Planes big;
    clock.mark(1);
    dcci2x(base, guide, big);
    clock.mark(2);
    guide.release();

    int ww = 2 * w;
    int hh = 2 * h;
    {
        size_t hn = static_cast<size_t>(ww) * hh;
        std::vector<float> invS2Big(hn);
        std::vector<float> validBig(hn);
        for (int y = 0; y < hh; y++) {
            for (int x = 0; x < ww; x++) {
                size_t li = static_cast<size_t>(y / 2) * w + x / 2;
                float sigma = 0.6f * steps[work[li]];
                invS2Big[static_cast<size_t>(y) * ww + x] = 1.0f / (2.0f * sigma * sigma);
                validBig[static_cast<size_t>(y) * ww + x] = valid[li];
            }
        }
        restoreGradients(big, invS2Big.data(), validBig.data());
        clock.mark(3);
    }

    // Grain: what the de-dither removed, bilinear 2x (weights 3/4, 1/4).
    const float grain = 1.0f - kDeditherStrength;
    {
        std::vector<float> diff(n);
        std::vector<float> column(static_cast<size_t>(w) * hh);
        for (int c = 0; c < 3; c++) {
            const std::vector<float>& src = c == 0 ? rgb.r : (c == 1 ? rgb.g : rgb.b);
            const std::vector<float>& sub = c == 0 ? grainBase.r : (c == 1 ? grainBase.g : grainBase.b);
            std::vector<float>& dst = c == 0 ? big.r : (c == 1 ? big.g : big.b);
            for (size_t i = 0; i < n; i++) {
                diff[i] = src[i] - sub[i];
            }
            for (int y = 0; y < h; y++) {
                const float* cur = diff.data() + static_cast<size_t>(y) * w;
                const float* prev = diff.data() + static_cast<size_t>(clampi(y - 1, 0, h - 1)) * w;
                const float* next = diff.data() + static_cast<size_t>(clampi(y + 1, 0, h - 1)) * w;
                float* lo = column.data() + static_cast<size_t>(2 * y) * w;
                float* hi = lo + w;
                for (int x = 0; x < w; x++) {
                    lo[x] = 0.75f * cur[x] + 0.25f * prev[x];
                    hi[x] = 0.75f * cur[x] + 0.25f * next[x];
                }
            }
            for (int y = 0; y < hh; y++) {
                const float* row = column.data() + static_cast<size_t>(y) * w;
                float* out = dst.data() + static_cast<size_t>(y) * ww;
                for (int x = 0; x < w; x++) {
                    float cur = row[x];
                    float prev = row[clampi(x - 1, 0, w - 1)];
                    float next = row[clampi(x + 1, 0, w - 1)];
                    out[2 * x] += grain * (0.75f * cur + 0.25f * prev);
                    out[2 * x + 1] += grain * (0.75f * cur + 0.25f * next);
                }
            }
        }
    }

    clock.mark(5);
    // Crop (tiles), clip, alpha from the source silhouette.
    int offset = 2 * pad;
    for (int y = 0; y < dest->height; y++) {
        const unsigned char* srcRow = indices + static_cast<size_t>(y / 2) * pitch;
        size_t bigRow = static_cast<size_t>(y + offset) * ww + offset;
        uint32_t* out = dest->pixels.data() + static_cast<size_t>(y) * dest->width;
        for (int x = 0; x < dest->width; x++) {
            if (isHole(srcRow[x / 2])) {
                out[x] = 0;
                continue;
            }
            size_t i = bigRow + x;
            // Precise: a 4x4 ordered dither (+-0.5 level) before rounding,
            // so the smooth ramps do not band again at 8 bits.
            float bias = precise ? kBayer4[(y & 3) * 4 + (x & 3)] : 0.0f;
            auto channel = [bias](float v) {
                v += bias;
                v = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
                return static_cast<uint32_t>(std::nearbyint(v));
            };
            out[x] = 0xFF000000u | (channel(big.r[i]) << 16) | (channel(big.g[i]) << 8) | channel(big.b[i]);
        }
    }
    clock.mark(6);
}

} // namespace

void hdUpscaleStageTimes(unsigned long long* ns, bool reset)
{
    for (int stage = 0; stage < kHdUpscaleStages; stage++) {
        ns[stage] = reset ? gStageNs[stage].exchange(0) : gStageNs[stage].load();
    }
}

uint64_t hdUpscaleKey(const unsigned char* indices, int width, int height, int pitch, const unsigned char* palette8, bool tile,
    bool precise, const HdTileContext* context)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    auto feed = [&](const void* data, size_t size) {
        const unsigned char* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; i++) {
            hash ^= bytes[i];
            hash *= 0x100000001b3ull;
        }
    };
    int32_t header[4] = { kHdUpscaleVersion, width, height, (tile ? 1 : 0) | (precise ? 2 : 0) };
    feed(header, sizeof(header));
    feed(palette8, 256 * 3);
    for (int y = 0; y < height; y++) {
        feed(indices + static_cast<size_t>(y) * pitch, width);
    }
    if (tile && context != nullptr) {
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                unsigned char marker = context->neighbour[r][c] != nullptr ? 1 : 0;
                feed(&marker, 1);
                if (marker) {
                    feed(context->neighbour[r][c], static_cast<size_t>(width) * height);
                }
            }
        }
    }
    return hash;
}

namespace {

    std::string cachePath(const std::string& dir, uint64_t key, std::string* subdir)
    {
        char name[64];
        snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(key));
        std::string sub = dir + "/" + std::string(name, 2);
        if (subdir != nullptr) {
            *subdir = sub;
        }
        return sub + "/" + name + ".rgba";
    }

    // Creates `path` and any missing parents.
    void makeDir(const std::string& path)
    {
        for (size_t at = 1; at <= path.size(); at++) {
            if (at < path.size() && path[at] != '/' && path[at] != '\\') {
                continue;
            }
            std::string part = path.substr(0, at);
#ifdef _WIN32
            _mkdir(part.c_str());
#else
            mkdir(part.c_str(), 0755);
#endif
        }
    }

    constexpr uint32_t kCacheMagic = 0x31554448; // "HDU1"

} // namespace

bool hdUpscaleCacheRead(const std::string& dir, uint64_t key, HdImage* image)
{
    std::string path = cachePath(dir, key, nullptr);
    FILE* stream = fopen(path.c_str(), "rb");
    if (stream == nullptr) {
        return false;
    }
    // Last use = modification time (the cache trims the oldest).
#ifdef _WIN32
    _utime(path.c_str(), nullptr);
#else
    utime(path.c_str(), nullptr);
#endif
    uint32_t header[3];
    bool ok = fread(header, sizeof(header), 1, stream) == 1 && header[0] == kCacheMagic
        && header[1] > 0 && header[2] > 0 && header[1] <= 16384 && header[2] <= 16384;
    if (ok) {
        image->width = static_cast<int>(header[1]);
        image->height = static_cast<int>(header[2]);
        image->pixels.resize(static_cast<size_t>(image->width) * image->height);
        ok = fread(image->pixels.data(), sizeof(uint32_t), image->pixels.size(), stream) == image->pixels.size();
    }
    fclose(stream);
    return ok;
}

bool hdUpscaleCacheWrite(const std::string& dir, uint64_t key, const HdImage& image)
{
    std::string subdir;
    std::string path = cachePath(dir, key, &subdir);
    makeDir(subdir);

    static std::atomic<unsigned int> counter { 0 };
    char suffix[32];
    snprintf(suffix, sizeof(suffix), ".tmp%u", counter.fetch_add(1));
    std::string temp = path + suffix;
    FILE* stream = fopen(temp.c_str(), "wb");
    if (stream == nullptr) {
        return false;
    }
    uint32_t header[3] = { kCacheMagic, static_cast<uint32_t>(image.width), static_cast<uint32_t>(image.height) };
    bool ok = fwrite(header, sizeof(header), 1, stream) == 1
        && fwrite(image.pixels.data(), sizeof(uint32_t), image.pixels.size(), stream) == image.pixels.size();
    ok = fclose(stream) == 0 && ok;
    if (!ok || rename(temp.c_str(), path.c_str()) != 0) {
        remove(temp.c_str());
        return false;
    }
    return true;
}

namespace {

    struct CacheFile {
        std::string path;
        time_t used = 0;
        unsigned long long size = 0;
    };

    void listCacheDir(const std::string& dir, std::vector<CacheFile>* files)
    {
#ifdef _WIN32
        struct _finddata_t found;
        intptr_t handle = _findfirst((dir + "\\*.rgba").c_str(), &found);
        if (handle == -1) {
            return;
        }
        do {
            files->push_back({ dir + "/" + found.name, found.time_write, static_cast<unsigned long long>(found.size) });
        } while (_findnext(handle, &found) == 0);
        _findclose(handle);
#else
        DIR* stream = opendir(dir.c_str());
        if (stream == nullptr) {
            return;
        }
        while (struct dirent* entry = readdir(stream)) {
            size_t length = strlen(entry->d_name);
            if (length < 6 || strcmp(entry->d_name + length - 5, ".rgba") != 0) {
                continue;
            }
            std::string path = dir + "/" + entry->d_name;
            struct stat info;
            if (stat(path.c_str(), &info) == 0) {
                files->push_back({ path, info.st_mtime, static_cast<unsigned long long>(info.st_size) });
            }
        }
        closedir(stream);
#endif
    }

} // namespace

unsigned long long hdUpscaleCacheTrim(const std::string& dir, unsigned long long maxBytes, unsigned long long* removedBytes)
{
    std::vector<CacheFile> files;
    for (int sub = 0; sub < 256; sub++) {
        char name[8];
        snprintf(name, sizeof(name), "%02x", sub);
        listCacheDir(dir + "/" + name, &files);
    }
    unsigned long long total = 0;
    for (const CacheFile& file : files) {
        total += file.size;
    }
    unsigned long long removed = 0;
    if (total > maxBytes) {
        // Oldest first, down to 90% of the cap.
        std::sort(files.begin(), files.end(), [](const CacheFile& a, const CacheFile& b) { return a.used < b.used; });
        unsigned long long target = maxBytes / 10 * 9;
        for (const CacheFile& file : files) {
            if (total <= target) {
                break;
            }
            if (remove(file.path.c_str()) == 0) {
                total -= file.size;
                removed += file.size;
            }
        }
    }
    if (removedBytes != nullptr) {
        *removedBytes = removed;
    }
    return total;
}

// --- pool ---------------------------------------------------------------------

HdUpscalePool::~HdUpscalePool()
{
    stop();
}

void HdUpscalePool::start(int threads, const std::string& cacheDir, unsigned long long cacheMaxBytes)
{
    stop();
    cacheMaxBytes_ = cacheMaxBytes;
    cacheWrittenSinceTrim_ = 0;
    if (threads <= 0) {
        unsigned int hardware = std::thread::hardware_concurrency();
        threads = std::min(std::max(static_cast<int>(hardware) - 1, 1), 8);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = false;
        cacheDir_ = cacheDir;
    }
    for (int index = 0; index < threads; index++) {
        workers_.emplace_back([this]() { workerMain(); });
    }
    if (!cacheDir.empty() && cacheMaxBytes > 0) {
        // The size cap, checked at start (in the background) and again
        // after every 1/8 of it written.
        trimmer_ = std::thread([this]() { trimCache(); });
    }
}

void HdUpscalePool::trimCache()
{
    unsigned long long removed = 0;
    unsigned long long total = hdUpscaleCacheTrim(cacheDir_, cacheMaxBytes_, &removed);
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.cacheBytes = total;
    stats_.cacheTrimmedBytes += removed;
}

void HdUpscalePool::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cancelQueued();
    workAvailable_.notify_all();
    for (std::thread& worker : workers_) {
        worker.join();
    }
    workers_.clear();
    if (trimmer_.joinable()) {
        trimmer_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    done_.clear();
    running_ = 0;
    for (int group = 0; group < kGroups; group++) {
        pendingByGroup_[group] = 0;
    }
}

void HdUpscalePool::submit(HdUpscaleJob job)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || workers_.empty()) {
            return;
        }
        int group = std::min(std::max(job.group, 0), kGroups - 1);
        auto found = entries_.find(job.key);
        if (found != entries_.end()) {
            // Already known; a queued one moves up if wanted sooner.
            Entry& entry = found->second;
            if (entry.state == State::Queued && job.priority < entry.order.first) {
                queue_.erase(entry.order);
                entry.order = { job.priority, sequence_++ };
                queue_.emplace(entry.order, job.key);
            }
            return;
        }
        std::string key = job.key;
        Entry& entry = entries_[key];
        entry.state = State::Queued;
        entry.group = group;
        entry.order = { job.priority, sequence_++ };
        entry.job = std::make_unique<HdUpscaleJob>(std::move(job));
        queue_.emplace(entry.order, key);
        pendingByGroup_[group]++;
        totalByGroup_[group]++;
        stats_.submitted++;
    }
    workAvailable_.notify_one();
}

void HdUpscalePool::cancelQueued(int group)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = queue_.begin(); it != queue_.end();) {
        auto found = entries_.find(it->second);
        if (found == entries_.end() || (group >= 0 && found->second.group != group)) {
            ++it;
            continue;
        }
        pendingByGroup_[found->second.group]--;
        totalByGroup_[found->second.group]--;
        entries_.erase(found);
        it = queue_.erase(it);
    }
}

std::unique_ptr<HdUpscaleJob> HdUpscalePool::popLocked(std::string* key)
{
    while (!queue_.empty()) {
        auto first = queue_.begin();
        std::string name = std::move(first->second);
        queue_.erase(first);
        auto found = entries_.find(name);
        if (found == entries_.end() || found->second.state != State::Queued) {
            continue;
        }
        found->second.state = State::Running;
        running_++;
        *key = std::move(name);
        return std::move(found->second.job);
    }
    return nullptr;
}

void HdUpscalePool::finishLocked(const std::string& key, int group, std::shared_ptr<HdImage> image)
{
    running_--;
    pendingByGroup_[group]--;
    doneByGroup_[group]++;
    auto entry = entries_.find(key);
    if (entry != entries_.end()) {
        entry->second.state = State::Done;
        entry->second.image = std::move(image);
        done_.push_back(key);
    }
    if (running_ == 0 && queue_.empty()) {
        stats_.idleSince = std::chrono::steady_clock::now();
    }
    jobDone_.notify_all();
}

std::shared_ptr<HdImage> HdUpscalePool::take(const std::string& key, bool* ready)
{
    std::unique_ptr<HdUpscaleJob> job;
    int group = 0;
    if (ready != nullptr) {
        *ready = false;
    }
    {
        std::unique_lock<std::mutex> lock(mutex_);
        auto found = entries_.find(key);
        if (found == entries_.end()) {
            return nullptr;
        }
        bool waitedHere = false;
        if (found->second.state == State::Running) {
            waitedHere = true;
            stats_.waited++;
            jobDone_.wait(lock, [&]() {
                auto again = entries_.find(key);
                return again == entries_.end() || again->second.state == State::Done;
            });
            found = entries_.find(key);
            if (found == entries_.end()) {
                return nullptr;
            }
        }
        if (found->second.state == State::Done) {
            if (ready != nullptr) {
                *ready = !waitedHere;
            }
            std::shared_ptr<HdImage> image = std::move(found->second.image);
            entries_.erase(found);
            return image;
        }
        // Queued: run it here.
        group = found->second.group;
        job = std::move(found->second.job);
        queue_.erase(found->second.order);
        entries_.erase(found);
        pendingByGroup_[group]--;
        doneByGroup_[group]++;
    }
    return run(*job, true);
}

bool HdUpscalePool::help()
{
    std::string key;
    std::unique_ptr<HdUpscaleJob> job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job = popLocked(&key);
    }
    if (job == nullptr) {
        return false;
    }
    std::shared_ptr<HdImage> image = run(*job, true);
    std::lock_guard<std::mutex> lock(mutex_);
    finishLocked(key, job->group, std::move(image));
    return true;
}

void HdUpscalePool::drain(const std::function<void(const std::string&, std::shared_ptr<HdImage>)>& sink)
{
    std::vector<std::pair<std::string, std::shared_ptr<HdImage>>> done;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (done_.empty()) {
            return;
        }
        for (const std::string& key : done_) {
            // Taken meanwhile (take()) when no longer there.
            auto found = entries_.find(key);
            if (found != entries_.end() && found->second.state == State::Done) {
                done.emplace_back(key, std::move(found->second.image));
                entries_.erase(found);
            }
        }
        done_.clear();
    }
    for (auto& entry : done) {
        if (entry.second != nullptr) {
            sink(entry.first, std::move(entry.second));
        }
    }
}

int HdUpscalePool::pending()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(queue_.size()) + running_;
}

int HdUpscalePool::pending(int group)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return group >= 0 && group < kGroups ? pendingByGroup_[group] : 0;
}

void HdUpscalePool::groupProgress(int group, int* total, int* done)
{
    std::lock_guard<std::mutex> lock(mutex_);
    *total = group >= 0 && group < kGroups ? totalByGroup_[group] : 0;
    *done = group >= 0 && group < kGroups ? doneByGroup_[group] : 0;
}

void HdUpscalePool::resetGroup(int group)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (group >= 0 && group < kGroups) {
        totalByGroup_[group] = pendingByGroup_[group];
        doneByGroup_[group] = 0;
    }
}

void HdUpscalePool::waitIdle()
{
    std::unique_lock<std::mutex> lock(mutex_);
    jobDone_.wait(lock, [&]() { return queue_.empty() && running_ == 0; });
}

HdUpscaleStats HdUpscalePool::stats()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

std::shared_ptr<HdImage> HdUpscalePool::run(const HdUpscaleJob& job, bool inlineRun)
{
    auto image = std::make_shared<HdImage>();
    HdTileContext context;
    bool hasContext = false;
    size_t tileSize = static_cast<size_t>(job.width) * job.height;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            if (job.context[r][c].size() == tileSize && tileSize > 0) {
                context.neighbour[r][c] = job.context[r][c].data();
                hasContext = true;
            }
        }
    }
    uint64_t key = 0;
    if (!cacheDir_.empty()) {
        key = hdUpscaleKey(job.indices.data(), job.width, job.height, job.width, job.palette8, job.tile, job.precise, &context);
        if (hdUpscaleCacheRead(cacheDir_, key, image.get())
            && image->width == job.width * kHdScale && image->height == job.height * kHdScale) {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.diskHits++;
            return image;
        }
    }

    // A caller waiting for the frame splits a large one over the cores.
    int threads = 1;
    if (inlineRun) {
        threads = std::min(std::max(static_cast<int>(std::thread::hardware_concurrency()), 1), 8);
    }
    auto started = std::chrono::steady_clock::now();
    hdUpscaleScripted(job.indices.data(), job.width, job.height, job.width, job.palette8, job.tile, image.get(), threads,
        job.precise, hasContext ? &context : nullptr);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

    bool wrote = !cacheDir_.empty() && hdUpscaleCacheWrite(cacheDir_, key, *image);
    bool trim = false;
    if (wrote && cacheMaxBytes_ > 0) {
        unsigned long long written = cacheWrittenSinceTrim_.fetch_add(image->pixels.size() * sizeof(uint32_t) + 12)
            + image->pixels.size() * sizeof(uint32_t);
        if (written > cacheMaxBytes_ / 8) {
            cacheWrittenSinceTrim_ = 0;
            trim = true;
        }
    }
    if (trim) {
        trimCache();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (inlineRun) {
        stats_.computedInline++;
    } else {
        stats_.computed++;
    }
    stats_.computeMs += ms;
    stats_.outputPixels += image->pixels.size();
    if (wrote) {
        stats_.diskWrites++;
    }
    return image;
}

void HdUpscalePool::workerMain()
{
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        workAvailable_.wait(lock, [&]() { return stopping_ || !queue_.empty(); });
        if (stopping_) {
            return;
        }
        std::string key;
        std::unique_ptr<HdUpscaleJob> job = popLocked(&key);
        if (job == nullptr) {
            continue;
        }
        lock.unlock();
        std::shared_ptr<HdImage> image = run(*job, false);
        lock.lock();
        finishLocked(key, job->group, std::move(image));
    }
}

} // namespace fallout
