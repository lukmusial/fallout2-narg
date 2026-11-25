#include "hd_text.h"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "hd_layer.h"
#include "settings.h"
#include "text_font.h"
#include "window_manager.h"

namespace fallout {

bool gHdTextActive = false;

namespace {

    // A string laid out and drawn at 2x, relative to the pointer the legacy
    // draw got (logical pixel (0, 0)).
    struct TextRun {
        // Rectangle (logical) covered by the run, relative to the draw origin.
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        // 1x: non-zero where the legacy draw writes a pixel.
        std::vector<unsigned char> mask;
        // 2x premultiplied 0xAARRGGBB (base palette colours).
        std::vector<uint32_t> pixels;
    };

    // A glyph placement in logical pixels: glyph `index` with its box at
    // (x, y), drawn `width` x `height` (scaled draws stretch the glyph).
    struct Placement {
        int index;
        int x;
        int y;
        int width;
        int height;
    };

    struct Layout {
        std::vector<Placement> glyphs;
        // Underline row (logical) from x = 0 to underlineEnd, when > 0.
        int underlineY = 0;
        int underlineEnd = 0;
    };

    struct CachedGlyph {
        int width = 0;
        int height = 0;
        std::vector<unsigned char> levels;
        // 2x coverage with kHdGlyphMargin around the glyph box (hd_glyph.h).
        std::vector<unsigned char> alpha;
    };

    struct TextRecord {
        // The pointer and pitch the legacy draw got.
        const unsigned char* origin;
        int pitch;
        std::shared_ptr<const TextRun> run;
        // The indices the legacy draw left at the run's mask pixels (in mask
        // order); the text is still in the buffer while they are.
        std::vector<unsigned char> indices;
    };

    constexpr size_t kMaxRecords = 512;

    bool gEnabled = true;
    HdGlyphStyle gStyle = HdGlyphStyle::Smooth;
    int gDepth = 0;
    std::unordered_map<uint32_t, CachedGlyph> gGlyphs;
    std::deque<TextRecord> gRecords;
    HdTextStats gStats = {};

    struct Timer {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~Timer()
        {
            gStats.totalMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }
    };

    const CachedGlyph* glyphFor(int font, int index)
    {
        uint32_t key = (static_cast<uint32_t>(font & 0xFFFF) << 8) | static_cast<uint32_t>(index & 0xFF);
        auto it = gGlyphs.find(key);
        if (it != gGlyphs.end()) {
            return &(it->second);
        }

        CachedGlyph glyph;
        if (!fontGetHdGlyph(font, index, &glyph.width, &glyph.height, &glyph.levels)) {
            return nullptr;
        }
        hdGlyphUpscale(glyph.levels.data(), glyph.width, glyph.height, gStyle, &glyph.alpha);
        gStats.glyphsCached++;
        return &(gGlyphs.emplace(key, std::move(glyph)).first->second);
    }

    // Mirrors textFontDrawImpl / interfaceFontDrawImpl (without the shadow
    // pass): where every glyph of `string` lands. False when the legacy draw
    // would read past the font's glyph table (left to the legacy renderer).
    bool layoutString(const FontHdInfo& info, int font, const char* string, int length, ColorWithFlags color, Layout* layout)
    {
        bool monospaced = (color & DRAW_TEXT_FLAG_MONOSPACED) != 0;
        int ptr = 0;

        if (!info.interfaceFont) {
            for (const char* p = string; *p != '\0'; p++) {
                char ch = *p;
                if (ch >= info.glyphCount) {
                    continue;
                }
                int index = ch & 0xFF;
                if (index >= info.glyphCount) {
                    return false;
                }

                int width;
                int height;
                const CachedGlyph* glyph = glyphFor(font, index);
                if (glyph == nullptr) {
                    return false;
                }
                width = glyph->width;
                height = glyph->height;

                int end;
                if (monospaced) {
                    end = ptr + info.monospacedWidth;
                    ptr += (info.monospacedWidth - info.letterSpacing - width) / 2;
                } else {
                    end = ptr + width + info.letterSpacing;
                }

                if (end > length) {
                    break;
                }

                layout->glyphs.push_back({ index, ptr, 0, width, height });
                ptr = end;
            }
        } else {
            for (const char* p = string; *p != '\0'; p++) {
                int index = static_cast<unsigned char>(*p);
                const CachedGlyph* glyph = glyphFor(font, index);
                if (glyph == nullptr) {
                    return false;
                }

                int characterWidth = index == ' ' ? info.wordSpacing : glyph->width;
                int end;
                if (monospaced) {
                    end = ptr + info.monospacedWidth;
                    ptr += (info.monospacedWidth - characterWidth - info.letterSpacing) / 2;
                } else {
                    end = ptr + characterWidth + info.letterSpacing;
                }

                if (end > length) {
                    break;
                }

                layout->glyphs.push_back({ index, ptr, info.rows - glyph->height, glyph->width, glyph->height });
                ptr = end;
            }
        }

        if ((color & DRAW_TEXT_FLAG_UNDERLINED) != 0 && ptr > 0) {
            layout->underlineY = info.rows - 1;
            layout->underlineEnd = ptr;
        }

        return true;
    }

    // Mirrors interfaceFontDrawScaledImpl.
    bool layoutScaled(const FontHdInfo& info, int font, const char* string, int x, int y, int destWidth, float scale, Layout* layout)
    {
        float cursorX = static_cast<float>(x);
        for (const char* p = string; *p != '\0'; p++) {
            int index = static_cast<unsigned char>(*p);
            const CachedGlyph* glyph = glyphFor(font, index);
            if (glyph == nullptr) {
                return false;
            }

            int characterWidth = index == ' ' ? info.wordSpacing : glyph->width;
            int advance = characterWidth + info.letterSpacing;
            int glyphX = static_cast<int>(lround(cursorX));
            if (glyph->width > 0 && glyph->height > 0) {
                int scaledWidth = std::max(1, static_cast<int>(lround(glyph->width * scale)));
                int scaledHeight = std::max(1, static_cast<int>(lround(glyph->height * scale)));
                int glyphY = y + std::max(0, static_cast<int>(lround((info.rows - glyph->height) * scale)));
                layout->glyphs.push_back({ index, glyphX, glyphY, scaledWidth, scaledHeight });
            }

            cursorX += advance * scale;
            if (static_cast<int>(lround(cursorX)) >= destWidth) {
                break;
            }
        }
        return true;
    }

    // Coverage planes of a run being built (2x, one byte per sub-pixel).
    struct Planes {
        int x = 0; // logical origin of the planes relative to the draw origin
        int y = 0;
        int width = 0; // logical size
        int height = 0;
        std::vector<unsigned char> text;
        std::vector<unsigned char> shadow;
        std::vector<unsigned char> mask;
    };

    void bounds(const Layout& layout, int dx, int dy, int* left, int* top, int* right, int* bottom)
    {
        for (const Placement& glyph : layout.glyphs) {
            *left = std::min(*left, glyph.x + dx);
            *top = std::min(*top, glyph.y + dy);
            *right = std::max(*right, glyph.x + glyph.width + dx);
            *bottom = std::max(*bottom, glyph.y + glyph.height + dy);
        }
        if (layout.underlineEnd > 0) {
            *left = std::min(*left, dx);
            *top = std::min(*top, layout.underlineY + dy);
            *right = std::max(*right, layout.underlineEnd + dx);
            *bottom = std::max(*bottom, layout.underlineY + 1 + dy);
        }
    }

    // Draws a layout's glyphs into one coverage plane (max of coverages) and
    // the mask, offset by (dx, dy) logical pixels.
    void rasterize(Planes* planes, std::vector<unsigned char>* plane, int font, const Layout& layout, int dx, int dy, bool scaled)
    {
        const int planePitch = planes->width * 2;
        const int planeRows = planes->height * 2;

        for (const Placement& placement : layout.glyphs) {
            const CachedGlyph* glyph = glyphFor(font, placement.index);
            if (glyph == nullptr || glyph->width <= 0 || glyph->height <= 0) {
                continue;
            }

            const int gx = placement.x + dx - planes->x;
            const int gy = placement.y + dy - planes->y;
            const int alphaPitch = (glyph->width + 2 * kHdGlyphMargin) * 2;
            const int alphaRows = (glyph->height + 2 * kHdGlyphMargin) * 2;

            if (!scaled) {
                for (int y = 0; y < glyph->height; y++) {
                    for (int x = 0; x < glyph->width; x++) {
                        if (glyph->levels[static_cast<size_t>(y) * glyph->width + x] != 0) {
                            planes->mask[static_cast<size_t>(gy + y) * planes->width + gx + x] = 1;
                        }
                    }
                }

                int ox = (gx - kHdGlyphMargin) * 2;
                int oy = (gy - kHdGlyphMargin) * 2;
                for (int y = 0; y < alphaRows; y++) {
                    int py = oy + y;
                    if (py < 0 || py >= planeRows) {
                        continue;
                    }
                    for (int x = 0; x < alphaPitch; x++) {
                        int px = ox + x;
                        if (px < 0 || px >= planePitch) {
                            continue;
                        }
                        unsigned char value = glyph->alpha[static_cast<size_t>(y) * alphaPitch + x];
                        unsigned char& dest = (*plane)[static_cast<size_t>(py) * planePitch + px];
                        dest = std::max(dest, value);
                    }
                }
                continue;
            }

            // Scaled: the mask samples the glyph like the legacy draw (nearest),
            // the 2x coverage is resampled (bilinear).
            float scaleX = static_cast<float>(placement.width) / glyph->width;
            float scaleY = static_cast<float>(placement.height) / glyph->height;
            for (int y = 0; y < placement.height; y++) {
                int srcY = std::clamp(static_cast<int>(y / scaleY), 0, glyph->height - 1);
                for (int x = 0; x < placement.width; x++) {
                    int srcX = std::clamp(static_cast<int>(x / scaleX), 0, glyph->width - 1);
                    if (glyph->levels[static_cast<size_t>(srcY) * glyph->width + srcX] != 0) {
                        planes->mask[static_cast<size_t>(gy + y) * planes->width + gx + x] = 1;
                    }
                }
            }

            auto sample = [&](float u, float v) {
                int u0 = static_cast<int>(floorf(u));
                int v0 = static_cast<int>(floorf(v));
                float fu = u - u0;
                float fv = v - v0;
                auto at = [&](int ax, int ay) -> float {
                    if (ax < 0 || ay < 0 || ax >= alphaPitch || ay >= alphaRows) {
                        return 0.0f;
                    }
                    return glyph->alpha[static_cast<size_t>(ay) * alphaPitch + ax];
                };
                return (at(u0, v0) * (1 - fu) + at(u0 + 1, v0) * fu) * (1 - fv)
                    + (at(u0, v0 + 1) * (1 - fu) + at(u0 + 1, v0 + 1) * fu) * fv;
            };

            int margin = static_cast<int>(ceilf(2 * kHdGlyphMargin * std::max(scaleX, scaleY)));
            for (int y = -margin; y < placement.height * 2 + margin; y++) {
                int py = gy * 2 + y;
                if (py < 0 || py >= planeRows) {
                    continue;
                }
                float v = (y + 0.5f) / scaleY - 0.5f + 2 * kHdGlyphMargin;
                for (int x = -margin; x < placement.width * 2 + margin; x++) {
                    int px = gx * 2 + x;
                    if (px < 0 || px >= planePitch) {
                        continue;
                    }
                    float u = (x + 0.5f) / scaleX - 0.5f + 2 * kHdGlyphMargin;
                    unsigned char value = static_cast<unsigned char>(std::clamp(sample(u, v) + 0.5f, 0.0f, 255.0f));
                    unsigned char& dest = (*plane)[static_cast<size_t>(py) * planePitch + px];
                    dest = std::max(dest, value);
                }
            }
        }

        if (layout.underlineEnd > 0) {
            int uy = layout.underlineY + dy - planes->y;
            for (int x = 0; x < layout.underlineEnd; x++) {
                int ux = x + dx - planes->x;
                planes->mask[static_cast<size_t>(uy) * planes->width + ux] = 1;
                for (int sy = 0; sy < 2; sy++) {
                    for (int sx = 0; sx < 2; sx++) {
                        (*plane)[static_cast<size_t>(uy * 2 + sy) * planePitch + ux * 2 + sx] = 255;
                    }
                }
            }
        }
    }

    uint32_t paletteColor(const unsigned char* palette8, int index)
    {
        return (static_cast<uint32_t>(palette8[index * 3]) << 16)
            | (static_cast<uint32_t>(palette8[index * 3 + 1]) << 8)
            | palette8[index * 3 + 2];
    }

    // Premultiplied text colour over premultiplied shadow colour.
    uint32_t composite(uint32_t textColor, unsigned textAlpha, uint32_t shadowColor, unsigned shadowAlpha)
    {
        unsigned shadowWeight = shadowAlpha * (255 - textAlpha) / 255;
        unsigned alpha = textAlpha + shadowWeight;
        if (alpha == 0) {
            return 0;
        }
        uint32_t result = std::min(alpha, 255u) << 24;
        for (int shift = 0; shift < 24; shift += 8) {
            unsigned value = (((textColor >> shift) & 0xFF) * textAlpha + ((shadowColor >> shift) & 0xFF) * shadowWeight + 127) / 255;
            result |= std::min(value, 255u) << shift;
        }
        return result;
    }

    void finishRun(const Planes& planes, uint32_t textColor, uint32_t shadowColor, TextRun* run)
    {
        run->x = planes.x;
        run->y = planes.y;
        run->width = planes.width;
        run->height = planes.height;
        run->mask = planes.mask;
        run->pixels.assign(planes.text.size(), 0);
        for (size_t index = 0; index < planes.text.size(); index++) {
            unsigned shadowAlpha = planes.shadow.empty() ? 0 : planes.shadow[index];
            run->pixels[index] = composite(textColor, planes.text[index], shadowColor, shadowAlpha);
        }
    }

    void allocatePlanes(Planes* planes, int left, int top, int right, int bottom, bool shadow)
    {
        // One logical pixel of margin for the 2x glyphs' reach.
        planes->x = left - kHdGlyphMargin;
        planes->y = top - kHdGlyphMargin;
        planes->width = right - left + 2 * kHdGlyphMargin;
        planes->height = bottom - top + 2 * kHdGlyphMargin;
        size_t logical = static_cast<size_t>(planes->width) * planes->height;
        planes->mask.assign(logical, 0);
        planes->text.assign(logical * 4, 0);
        if (shadow) {
            planes->shadow.assign(logical * 4, 0);
        }
    }

    bool renderString(const char* string, int length, ColorWithFlags color, TextRun* run)
    {
        int font = fontGetCurrent();
        FontHdInfo info;
        if (!fontGetHdInfo(font, &info)) {
            return false;
        }

        bool shadowed = (color & DRAW_TEXT_FLAG_SHADOWED) != 0;
        Layout layout;
        if (!layoutString(info, font, string, length, color, &layout)) {
            return false;
        }

        // The shadow pass: FON drops every flag, AAF keeps them.
        Layout shadowLayout;
        if (shadowed) {
            ColorWithFlags shadowFlags = info.interfaceFont
                ? static_cast<ColorWithFlags>(color & ~(DRAW_TEXT_FLAG_SHADOWED | 0xFF))
                : static_cast<ColorWithFlags>(0);
            if (!layoutString(info, font, string, length, shadowFlags, &shadowLayout)) {
                return false;
            }
        }

        int left = 1 << 30;
        int top = 1 << 30;
        int right = -(1 << 30);
        int bottom = -(1 << 30);
        bounds(layout, 0, 0, &left, &top, &right, &bottom);
        if (shadowed) {
            bounds(shadowLayout, 1, 1, &left, &top, &right, &bottom);
        }
        if (left >= right || top >= bottom) {
            return false;
        }

        Planes planes;
        allocatePlanes(&planes, left, top, right, bottom, shadowed);
        if (shadowed) {
            rasterize(&planes, &planes.shadow, font, shadowLayout, 1, 1, false);
        }
        rasterize(&planes, &planes.text, font, layout, 0, 0, false);

        unsigned char palette8[256 * 3];
        windowHdGetBasePalette(palette8);
        finishRun(planes, paletteColor(palette8, color & 0xFF), paletteColor(palette8, COLOR_BLACK), run);
        return true;
    }

    // Copies the window buffer's indices in the run's rectangle at (x, y)
    // (window coordinates); pixels outside the window are 0 (clipped later).
    void snapshot(const unsigned char* buffer, int pitch, int bufferHeight, int x, int y, int width, int height, std::vector<unsigned char>* out)
    {
        out->assign(static_cast<size_t>(width) * height, 0);
        for (int row = 0; row < height; row++) {
            int wy = y + row;
            if (wy < 0 || wy >= bufferHeight) {
                continue;
            }
            for (int column = 0; column < width; column++) {
                int wx = x + column;
                if (wx < 0 || wx >= pitch) {
                    continue;
                }
                (*out)[static_cast<size_t>(row) * width + column] = buffer[static_cast<size_t>(wy) * pitch + wx];
            }
        }
    }

    int windowHeightOf(int win)
    {
        Window* window = windowGetWindow(win);
        return window != nullptr ? window->height : 0;
    }

    void drawRunInWindow(int win, int x, int y, const TextRun& run, const std::vector<unsigned char>& before)
    {
        HdTextDraw draw;
        draw.x = x + run.x;
        draw.y = y + run.y;
        draw.width = run.width;
        draw.height = run.height;
        draw.mask = run.mask.data();
        draw.pixels = run.pixels.data();
        draw.before = before.data();
        windowHdDrawText(win, draw);
    }

    void remember(const unsigned char* origin, int pitch, std::shared_ptr<const TextRun> run)
    {
        TextRecord record;
        record.origin = origin;
        record.pitch = pitch;
        for (int y = 0; y < run->height; y++) {
            for (int x = 0; x < run->width; x++) {
                if (run->mask[static_cast<size_t>(y) * run->width + x] != 0) {
                    record.indices.push_back(origin[static_cast<ptrdiff_t>(run->y + y) * pitch + run->x + x]);
                }
            }
        }
        record.run = std::move(run);

        // Text drawn again at the same place replaces what was remembered there.
        gRecords.erase(std::remove_if(gRecords.begin(), gRecords.end(), [&](const TextRecord& other) {
            return other.origin == origin && other.pitch == pitch;
        }),
            gRecords.end());
        gRecords.push_back(std::move(record));
        while (gRecords.size() > kMaxRecords) {
            gRecords.pop_front();
        }
    }

} // namespace

void hdTextUpdateActive()
{
    HdGlyphStyle style;
    if (hdGlyphStyleFromName(settings.graphics.hd_text_style.c_str(), &style) && style != gStyle) {
        gStyle = style;
        gGlyphs.clear();
    }
    gEnabled = settings.graphics.hd_text;
    gHdTextActive = gEnabled && windowHdIsActive();
    if (!gHdTextActive) {
        gRecords.clear();
    }
}

void hdTextSetMode(bool enabled, HdGlyphStyle style)
{
    settings.graphics.hd_text = enabled;
    settings.graphics.hd_text_style = hdGlyphStyleName(style);
    hdTextUpdateActive();
}

bool hdTextIsActive()
{
    return gHdTextActive;
}

HdGlyphStyle hdTextGetStyle()
{
    return gStyle;
}

void hdTextDraw(unsigned char* buffer, const char* string, int length, int pitch, ColorWithFlags color)
{
    // Nested draws (the bitmap fonts draw their shadow through fontDrawText)
    // are part of the outer draw.
    if (gDepth > 0 || string == nullptr || buffer == nullptr) {
        fontDrawTextPtr(buffer, string, length, pitch, color);
        return;
    }

    gDepth++;
    Timer timer;

    int win;
    int x;
    int y;
    bool inWindow = windowFindBufferPixel(buffer, pitch, &win, &x, &y);

    auto run = std::make_shared<TextRun>();
    if (!renderString(string, length, color, run.get())) {
        fontDrawTextPtr(buffer, string, length, pitch, color);
        gDepth--;
        return;
    }

    if (inWindow) {
        Window* window = windowGetWindow(win);
        std::vector<unsigned char> before;
        snapshot(window->buffer, window->width, window->height, x + run->x, y + run->y, run->width, run->height, &before);
        fontDrawTextPtr(buffer, string, length, pitch, color);
        drawRunInWindow(win, x, y, *run, before);
        gStats.draws++;
    } else {
        fontDrawTextPtr(buffer, string, length, pitch, color);
        remember(buffer, pitch, std::move(run));
    }

    gDepth--;
}

void hdTextDrawScaled(const Buffer2D& dest, int x, int y, const char* string, ColorWithFlags color, float scale,
    void (*legacy)(const Buffer2D& dest, int x, int y, const char* string, ColorWithFlags color, float scale))
{
    int win;
    int wx;
    int wy;
    if (!gHdTextActive || gDepth > 0 || !windowFindBufferPixel(dest.data, dest.width, &win, &wx, &wy) || wx != 0 || wy != 0) {
        legacy(dest, x, y, string, color, scale);
        return;
    }

    gDepth++;
    Timer timer;

    int font = fontGetCurrent();
    FontHdInfo info;
    Layout layout;
    Layout shadowLayout;
    bool shadowed = (color & DRAW_TEXT_FLAG_SHADOWED) != 0;
    bool ok = fontGetHdInfo(font, &info) && info.interfaceFont
        && layoutScaled(info, font, string, x, y, dest.width, scale, &layout)
        && (!shadowed || layoutScaled(info, font, string, x + 1, y + 1, dest.width, scale, &shadowLayout));

    int left = 1 << 30;
    int top = 1 << 30;
    int right = -(1 << 30);
    int bottom = -(1 << 30);
    if (ok) {
        bounds(layout, 0, 0, &left, &top, &right, &bottom);
        bounds(shadowLayout, 0, 0, &left, &top, &right, &bottom);
        ok = left < right && top < bottom;
    }
    if (!ok) {
        legacy(dest, x, y, string, color, scale);
        gDepth--;
        return;
    }

    Planes planes;
    allocatePlanes(&planes, left, top, right, bottom, shadowed);
    if (shadowed) {
        rasterize(&planes, &planes.shadow, font, shadowLayout, 0, 0, true);
    }
    rasterize(&planes, &planes.text, font, layout, 0, 0, true);

    unsigned char palette8[256 * 3];
    windowHdGetBasePalette(palette8);
    TextRun run;
    finishRun(planes, paletteColor(palette8, color & 0xFF), paletteColor(palette8, COLOR_BLACK), &run);

    // The legacy scaled draw clips to the buffer; the mask must too.
    for (int row = 0; row < run.height; row++) {
        for (int column = 0; column < run.width; column++) {
            int px = run.x + column;
            int py = run.y + row;
            if (px < 0 || py < 0 || px >= dest.width || py >= dest.height) {
                run.mask[static_cast<size_t>(row) * run.width + column] = 0;
            }
        }
    }

    std::vector<unsigned char> before;
    snapshot(dest.data, dest.width, dest.height, run.x, run.y, run.width, run.height, &before);
    legacy(dest, x, y, string, color, scale);
    drawRunInWindow(win, 0, 0, run, before);
    gStats.draws++;
    gDepth--;
}

bool hdTextHasRecords()
{
    return gHdTextActive && !gRecords.empty();
}

void hdTextBlit(const unsigned char* src, int width, int height, int srcPitch, unsigned char* dest, int destPitch, bool transparent)
{
    struct Match {
        size_t record;
        int column; // run origin relative to src (logical)
        int row;
    };

    std::vector<Match> matches;
    std::vector<size_t> stale;
    Timer timer;

    for (size_t index = 0; index < gRecords.size(); index++) {
        const TextRecord& record = gRecords[index];
        if (record.pitch != srcPitch) {
            continue;
        }

        // The origin's position relative to src: the same row offset can be
        // a column to the right on one row or to the left on the next.
        ptrdiff_t offset = record.origin - src;
        ptrdiff_t row = offset >= 0 ? offset / srcPitch : -((-offset + srcPitch - 1) / srcPitch);
        ptrdiff_t column = offset - row * srcPitch;
        const TextRun& run = *record.run;

        bool found = false;
        bool intersects = false;
        for (int candidate = 0; candidate < 2 && !found; candidate++) {
            int r = static_cast<int>(row + candidate);
            int c = static_cast<int>(column - candidate * srcPitch);
            if (c + run.x >= width || c + run.x + run.width <= 0 || r + run.y >= height || r + run.y + run.height <= 0) {
                continue;
            }
            intersects = true;

            // Every text pixel inside the copied rectangle must still be
            // what the legacy draw left there.
            bool valid = true;
            bool any = false;
            size_t k = 0;
            for (int y = 0; y < run.height && valid; y++) {
                for (int x = 0; x < run.width; x++) {
                    if (run.mask[static_cast<size_t>(y) * run.width + x] == 0) {
                        continue;
                    }
                    int sx = c + run.x + x;
                    int sy = r + run.y + y;
                    if (sx >= 0 && sy >= 0 && sx < width && sy < height) {
                        if (src[static_cast<size_t>(sy) * srcPitch + sx] != record.indices[k]) {
                            valid = false;
                            break;
                        }
                        any = true;
                    }
                    k++;
                }
            }
            if (valid && any) {
                matches.push_back({ index, c + run.x, r + run.y });
                found = true;
            }
        }
        if (intersects && !found) {
            stale.push_back(index);
        }
    }

    int win = -1;
    int wx = 0;
    int wy = 0;
    bool toWindow = !matches.empty() && windowFindBufferPixel(dest, destPitch, &win, &wx, &wy);
    Window* window = toWindow ? windowGetWindow(win) : nullptr;

    std::vector<std::vector<unsigned char>> befores(matches.size());
    if (window != nullptr) {
        for (size_t index = 0; index < matches.size(); index++) {
            const TextRun& run = *gRecords[matches[index].record].run;
            snapshot(window->buffer, window->width, window->height, wx + matches[index].column, wy + matches[index].row, run.width, run.height, &befores[index]);
        }
    }

    if (transparent) {
        transSrcCopy(dest, destPitch, src, srcPitch, width, height);
    } else {
        srcCopy(dest, destPitch, src, srcPitch, width, height);
    }

    std::vector<TextRecord> copies;
    for (size_t index = 0; index < matches.size(); index++) {
        const Match& match = matches[index];
        const TextRecord& record = gRecords[match.record];
        const TextRun& run = *record.run;

        // The part of the run inside the copied rectangle, and only pixels
        // the copy wrote (a transparent copy skips index 0).
        TextRun clipped = run;
        bool whole = true;
        for (int y = 0; y < run.height; y++) {
            for (int x = 0; x < run.width; x++) {
                int sx = match.column + x;
                int sy = match.row + y;
                bool inside = sx >= 0 && sy >= 0 && sx < width && sy < height;
                size_t offset = static_cast<size_t>(y) * run.width + x;
                if (!inside || (transparent && src[static_cast<size_t>(sy) * srcPitch + sx] == 0)) {
                    if (run.mask[offset] != 0) {
                        whole = false;
                    }
                    clipped.mask[offset] = 0;
                    if (!inside) {
                        for (int sub = 0; sub < 4; sub++) {
                            clipped.pixels[static_cast<size_t>(y * 2 + sub / 2) * run.width * 2 + x * 2 + sub % 2] = 0;
                        }
                    }
                }
            }
        }

        if (window != nullptr) {
            HdTextDraw draw;
            draw.x = wx + match.column;
            draw.y = wy + match.row;
            draw.width = run.width;
            draw.height = run.height;
            draw.mask = clipped.mask.data();
            draw.pixels = clipped.pixels.data();
            draw.before = befores[index].data();
            windowHdDrawText(win, draw);
            gStats.replays++;
        } else if (whole && !transparent) {
            // Copied into another off-screen buffer: remember it there too.
            TextRecord copy;
            copy.origin = dest + static_cast<ptrdiff_t>(match.row - run.y) * destPitch + (match.column - run.x);
            copy.pitch = destPitch;
            copy.run = record.run;
            copy.indices = record.indices;
            copies.push_back(std::move(copy));
        }
    }

    for (auto it = stale.rbegin(); it != stale.rend(); ++it) {
        gRecords.erase(gRecords.begin() + static_cast<ptrdiff_t>(*it));
    }
    for (TextRecord& copy : copies) {
        gRecords.erase(std::remove_if(gRecords.begin(), gRecords.end(), [&](const TextRecord& other) {
            return other.origin == copy.origin && other.pitch == copy.pitch;
        }),
            gRecords.end());
        gRecords.push_back(std::move(copy));
    }
    while (gRecords.size() > kMaxRecords) {
        gRecords.pop_front();
    }
}

void hdTextOutline(unsigned char* buffer, int width, int height, int pitch, Color color)
{
    if (!gHdTextActive) {
        return;
    }

    unsigned char palette8[256 * 3];
    windowHdGetBasePalette(palette8);
    uint32_t outlineColor = paletteColor(palette8, color);

    for (TextRecord& record : gRecords) {
        ptrdiff_t offset = record.origin - buffer;
        if (record.pitch != pitch || offset < 0 || offset >= static_cast<ptrdiff_t>(pitch) * height) {
            continue;
        }

        // The outline goes around the text pixels (4-neighbourhood at 1x, a
        // radius of 2 sub-pixels at 2x), under the text; the mask grows by
        // the pixels bufferOutline wrote.
        const TextRun& old = *record.run;
        auto run = std::make_shared<TextRun>();
        run->x = old.x - 1;
        run->y = old.y - 1;
        run->width = old.width + 2;
        run->height = old.height + 2;
        run->mask.assign(static_cast<size_t>(run->width) * run->height, 0);
        run->pixels.assign(static_cast<size_t>(run->width) * run->height * 4, 0);

        int originX = static_cast<int>(offset % pitch);
        int originY = static_cast<int>(offset / pitch);
        for (int y = 0; y < run->height; y++) {
            for (int x = 0; x < run->width; x++) {
                int ox = x - 1;
                int oy = y - 1;
                bool wasText = ox >= 0 && oy >= 0 && ox < old.width && oy < old.height
                    && old.mask[static_cast<size_t>(oy) * old.width + ox] != 0;
                int bx = originX + run->x + x;
                int by = originY + run->y + y;
                bool outline = !wasText && bx >= 0 && by >= 0 && bx < width && by < height
                    && buffer[static_cast<size_t>(by) * pitch + bx] == color;
                if (outline) {
                    // Only pixels next to this run's text belong to it.
                    bool near = false;
                    for (int n = 0; n < 4 && !near; n++) {
                        static const int kDx[4] = { -1, 1, 0, 0 };
                        static const int kDy[4] = { 0, 0, -1, 1 };
                        int nx = ox + kDx[n];
                        int ny = oy + kDy[n];
                        near = nx >= 0 && ny >= 0 && nx < old.width && ny < old.height && old.mask[static_cast<size_t>(ny) * old.width + nx] != 0;
                    }
                    outline = near;
                }
                run->mask[static_cast<size_t>(y) * run->width + x] = wasText || outline;
            }
        }

        const int oldPitch = old.width * 2;
        const int newPitch = run->width * 2;
        const int rows = run->height * 2;
        for (int y = 0; y < rows; y++) {
            for (int x = 0; x < newPitch; x++) {
                // Outline coverage: the text's maximum alpha within a
                // diamond of radius 2 sub-pixels.
                unsigned outlineAlpha = 0;
                for (int dy = -2; dy <= 2; dy++) {
                    for (int dx = -2 + std::abs(dy); dx <= 2 - std::abs(dy); dx++) {
                        int ox = x + dx - 2;
                        int oy = y + dy - 2;
                        if (ox >= 0 && oy >= 0 && ox < oldPitch && oy < old.height * 2) {
                            outlineAlpha = std::max(outlineAlpha, old.pixels[static_cast<size_t>(oy) * oldPitch + ox] >> 24);
                        }
                    }
                }
                uint32_t text = 0;
                int tx = x - 2;
                int ty = y - 2;
                if (tx >= 0 && ty >= 0 && tx < oldPitch && ty < old.height * 2) {
                    text = old.pixels[static_cast<size_t>(ty) * oldPitch + tx];
                }
                uint32_t under = outlineAlpha == 0 ? 0 : composite(outlineColor, outlineAlpha, 0, 0);
                unsigned textAlpha = text >> 24;
                uint32_t result = text;
                if (textAlpha < 255 && under != 0) {
                    uint32_t inverse = 255 - textAlpha;
                    result = 0;
                    for (int shift = 0; shift < 32; shift += 8) {
                        unsigned value = ((text >> shift) & 0xFF) + (((under >> shift) & 0xFF) * inverse + 127) / 255;
                        result |= std::min(value, 255u) << shift;
                    }
                }
                run->pixels[static_cast<size_t>(y) * newPitch + x] = result;
            }
        }

        record.indices.clear();
        for (int y = 0; y < run->height; y++) {
            for (int x = 0; x < run->width; x++) {
                if (run->mask[static_cast<size_t>(y) * run->width + x] != 0) {
                    record.indices.push_back(buffer[static_cast<size_t>(originY + run->y + y) * pitch + originX + run->x + x]);
                }
            }
        }
        record.run = std::move(run);
    }
}

void hdTextGetStats(HdTextStats* stats)
{
    *stats = gStats;
}

} // namespace fallout
