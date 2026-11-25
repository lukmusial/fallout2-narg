#include "svga.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include <SDL.h>

#include "art.h"
#include "color.h"
#include "config.h"
#include "dinput.h"
#include "draw.h"
#include "game.h"
#include "hd_art.h"
#include "hd_dev_automation.h"
#include "hd_layer.h"
#include "hd_lights.h"
#include "hd_lights_map.h"
#include "hd_text.h"
#include "interface.h"
#include "memory.h"
#include "mouse.h"
#include "movie.h"
#include "scan_unimplemented.h"
#include "settings.h"
#include "text_font.h"
#include "tile.h"
#include "win32.h"
#include "window_manager.h"
#include "window_manager_private.h"

namespace fallout {

static bool createRenderer(int width, int height);
static void destroyRenderer();

// screen rect
Rect _scr_size;

// 0x6ACA18 scr_blit
void (*_scr_blit)(unsigned char* src, int src_pitch, int unused, int src_x, int src_y, int src_width, int src_height, int dest_x, int dest_y) = _GNW95_ShowRect;

// 0x6ACA1C zero_mem
void (*_zero_mem)() = nullptr;

SDL_Window* gSdlWindow = nullptr;
SDL_Renderer* gSdlRenderer = nullptr;
SDL_Texture* gSdlTexture = nullptr;

namespace {

    struct PresenterCounters {
        uint64_t presents;
        uint64_t presentTicks;
        uint64_t presentMaxTicks;
        uint64_t convertTicks;
        uint64_t convertedPixels;
        uint64_t paletteTicks;
        uint64_t paletteUpdates;
        uint64_t uploadedPixels;
        uint64_t lightTicks;
        uint64_t lightUpdates;
    };

} // namespace

// Logical screen size (the size every window and `_scr_size` use).
static int gScreenWidth = 0;
static int gScreenHeight = 0;

// What the game has put on screen, one palette index per logical pixel.
static std::vector<unsigned char> gScreenIndex;

// Presented colours, 0x00RRGGBB, `gFramebufferScale` output pixels per logical
// pixel in each direction.
static std::vector<uint32_t> gFramebuffer;
static int gFramebufferScale = 1;

// Current system palette at 8-bit precision (gamma and fades applied).
static unsigned char gPalette8[256 * 3];
static uint32_t gPaletteRgb[256];
static bool gPaletteValid = false;

// Entries changed since the framebuffer was last brought up to date.
static bool gPaletteChanged[256];
static int gPaletteChangedCount = 0;

// Area of the framebuffer (in logical pixels) not yet uploaded to the texture.
static Rect gDirty = { INT_MAX, INT_MAX, INT_MIN, INT_MIN };

static PresenterCounters gStats;

// HD composition (only when the framebuffer is at kHdScale).
//
// The window manager composites HD layers into the staging planes alongside
// its 8-bit `_screen_buffer` (screen coordinates), then blits rectangles of
// that buffer here with the HD source flag set; those rectangles copy the
// staging mask into `gHdMask`. Every other blit (cursor, movies, splash)
// clears `gHdMask`. Wherever `gHdMask` is set, the screen index plane holds
// the shadow index of an HD block whose colours are in `gHdStagingRgb`.
static bool gHdActive = false;
static std::vector<unsigned char> gHdMask;
static std::vector<unsigned char> gHdStagingMask;
static std::vector<uint32_t> gHdStagingRgb;
static bool gHdSourceIsStaging = false;

// Base palette (`_cmap` at 8 bits) HD colours are authored against, and the
// mapping from it to the presented palette.
static unsigned char gHdBase8[256 * 3];
static HdModulation gHdModulation;

// HD lights (hd_lights.h): the field applied to map pixels (kHdMaskLit), and
// the next one being built. `gHdLightsOn` is set while the field may hold
// anything; with hd_lights=0 it stays false and nothing below runs.
static bool gHdLightsOn = false;
static HdLightField gHdLightField;
static HdLightField gHdLightFieldNext;
static std::vector<HdLightRect> gHdLightDirty;

static void presenterAllocateFramebuffer()
{
    gFramebuffer.assign(static_cast<size_t>(gScreenWidth) * gFramebufferScale * gScreenHeight * gFramebufferScale, 0);

    gHdActive = gFramebufferScale == kHdScale;
    size_t logical = gHdActive ? static_cast<size_t>(gScreenWidth) * gScreenHeight : 0;
    gHdMask.assign(logical, 0);
    gHdStagingMask.assign(logical, 0);
    gHdStagingRgb.assign(logical * kHdScale * kHdScale, 0);
    if (gHdActive) {
        for (int index = 0; index < 256 * 3; index++) {
            gHdBase8[index] = colorExpand6(_cmap[index]);
        }
        hdModulationBuild(&gHdModulation, gHdBase8, gPalette8);
    }
}

// Keeps the modulation in step with the presented palette and `_cmap`.
// Entries whose base colour changed are flagged so their pixels are redrawn.
static void presenterUpdateHdModulation(int start, int count)
{
    if (!gHdActive) {
        return;
    }

    for (int index = 0; index < 256; index++) {
        unsigned char* base = gHdBase8 + index * 3;
        unsigned char r = colorExpand6(_cmap[index * 3]);
        unsigned char g = colorExpand6(_cmap[index * 3 + 1]);
        unsigned char b = colorExpand6(_cmap[index * 3 + 2]);
        bool inRange = index >= start && index < start + count;
        if (base[0] != r || base[1] != g || base[2] != b) {
            base[0] = r;
            base[1] = g;
            base[2] = b;
            if (!gPaletteChanged[index]) {
                gPaletteChanged[index] = true;
                gPaletteChangedCount++;
            }
            inRange = true;
        }
        if (inRange) {
            hdModulationUpdate(&gHdModulation, gHdBase8, gPalette8, index, 1);
        }
    }
}

static void presenterMarkDirty(int left, int top, int width, int height)
{
    gDirty.left = std::min(gDirty.left, left);
    gDirty.top = std::min(gDirty.top, top);
    gDirty.right = std::max(gDirty.right, left + width - 1);
    gDirty.bottom = std::max(gDirty.bottom, top + height - 1);
}

static void presenterMarkAllDirty()
{
    if (gScreenWidth > 0 && gScreenHeight > 0) {
        presenterMarkDirty(0, 0, gScreenWidth, gScreenHeight);
    }
}

// Writes the 2x2 output block of logical pixel (x, y) in HD mode.
static inline void presenterWriteBlock(int x, int y, uint32_t* dest, int fbWidth)
{
    size_t offset = static_cast<size_t>(y) * gScreenWidth + x;
    unsigned char index = gScreenIndex[offset];
    if ((gHdMask[offset] & kHdMaskShows) != 0) {
        const uint32_t* hd = gHdStagingRgb.data() + static_cast<size_t>(y) * kHdScale * fbWidth + x * kHdScale;
        dest[0] = hdModulate(&gHdModulation, hd[0], index);
        dest[1] = hdModulate(&gHdModulation, hd[1], index);
        dest[fbWidth] = hdModulate(&gHdModulation, hd[fbWidth], index);
        dest[fbWidth + 1] = hdModulate(&gHdModulation, hd[fbWidth + 1], index);
    } else {
        uint32_t color = gPaletteRgb[index];
        dest[0] = color;
        dest[1] = color;
        dest[fbWidth] = color;
        dest[fbWidth + 1] = color;
    }
}

// Converts a rectangle of the index plane into the framebuffer.
static void presenterConvertRect(int left, int top, int width, int height)
{
    if (gFramebuffer.empty() || width <= 0 || height <= 0) {
        return;
    }

    uint64_t startCounter = SDL_GetPerformanceCounter();

    const int scale = gFramebufferScale;
    const int fbWidth = gScreenWidth * scale;

    for (int y = top; y < top + height; y++) {
        const unsigned char* src = gScreenIndex.data() + static_cast<size_t>(y) * gScreenWidth + left;
        uint32_t* dest = gFramebuffer.data() + static_cast<size_t>(y) * scale * fbWidth + left * scale;
        if (gHdActive) {
            for (int x = 0; x < width; x++) {
                presenterWriteBlock(left + x, y, dest + x * kHdScale, fbWidth);
            }
            if (gHdLightsOn) {
                hdLightApplyRow(gHdLightField, y, left, width, gHdMask.data() + static_cast<size_t>(y) * gScreenWidth + left, dest, fbWidth);
            }
        } else if (scale == 1) {
            for (int x = 0; x < width; x++) {
                dest[x] = gPaletteRgb[src[x]];
            }
        } else {
            for (int x = 0; x < width; x++) {
                uint32_t color = gPaletteRgb[src[x]];
                for (int k = 0; k < scale; k++) {
                    dest[x * scale + k] = color;
                }
            }
            for (int k = 1; k < scale; k++) {
                memcpy(dest + k * fbWidth, dest, sizeof(uint32_t) * width * scale);
            }
        }
    }

    presenterMarkDirty(left, top, width, height);

    gStats.convertTicks += SDL_GetPerformanceCounter() - startCounter;
    gStats.convertedPixels += static_cast<uint64_t>(width) * height;
}

// Brings the framebuffer up to date with palette changes made since the last
// present. Only pixels whose index changed are rewritten, so colour cycling
// (a few dozen entries) does not cost a full conversion.
static void presenterApplyPaletteChanges()
{
    if (gPaletteChangedCount == 0 || gFramebuffer.empty()) {
        gPaletteChangedCount = 0;
        return;
    }

    uint64_t startCounter = SDL_GetPerformanceCounter();

    const int scale = gFramebufferScale;
    const int fbWidth = gScreenWidth * scale;
    int changedTop = INT_MAX;
    int changedBottom = INT_MIN;

    for (int y = 0; y < gScreenHeight; y++) {
        const unsigned char* src = gScreenIndex.data() + static_cast<size_t>(y) * gScreenWidth;
        uint32_t* dest = gFramebuffer.data() + static_cast<size_t>(y) * scale * fbWidth;
        bool rowChanged = false;
        if (gHdActive) {
            for (int x = 0; x < gScreenWidth; x++) {
                if (gPaletteChanged[src[x]]) {
                    presenterWriteBlock(x, y, dest + x * kHdScale, fbWidth);
                    if (gHdLightsOn) {
                        hdLightApplyRow(gHdLightField, y, x, 1, gHdMask.data() + static_cast<size_t>(y) * gScreenWidth + x, dest + x * kHdScale, fbWidth);
                    }
                    rowChanged = true;
                }
            }
        } else {
            for (int x = 0; x < gScreenWidth; x++) {
                unsigned char index = src[x];
                if (gPaletteChanged[index]) {
                    uint32_t color = gPaletteRgb[index];
                    for (int k = 0; k < scale; k++) {
                        dest[x * scale + k] = color;
                    }
                    rowChanged = true;
                }
            }

            if (rowChanged) {
                for (int k = 1; k < scale; k++) {
                    memcpy(dest + k * fbWidth, dest, sizeof(uint32_t) * fbWidth);
                }
            }
        }

        if (rowChanged) {
            changedTop = std::min(changedTop, y);
            changedBottom = std::max(changedBottom, y);
        }
    }

    if (changedTop <= changedBottom) {
        presenterMarkDirty(0, changedTop, gScreenWidth, changedBottom - changedTop + 1);
    }

    memset(gPaletteChanged, 0, sizeof(gPaletteChanged));
    gPaletteChangedCount = 0;

    gStats.paletteTicks += SDL_GetPerformanceCounter() - startCounter;
    gStats.paletteUpdates++;
}

// Updates `gHdMask` for a rectangle about to be converted: copied from the
// staging mask when the blit comes from the window manager's screen buffer,
// cleared otherwise.
static void presenterUpdateHdMask(int left, int top, int width, int height)
{
    if (!gHdActive) {
        return;
    }

    for (int y = top; y < top + height; y++) {
        unsigned char* mask = gHdMask.data() + static_cast<size_t>(y) * gScreenWidth + left;
        if (gHdSourceIsStaging) {
            memcpy(mask, gHdStagingMask.data() + static_cast<size_t>(y) * gScreenWidth + left, width);
        } else {
            memset(mask, 0, width);
        }
    }
}

// TODO: Remove once migration to update-render cycle is completed.
FpsLimiter sharedFpsLimiter;

// 0x4CAD08 init_mode_320_200
int _init_mode_320_200()
{
    return _GNW95_init_mode_ex(320, 200, 8);
}

// 0x4CAD40 init_mode_320_400
int _init_mode_320_400()
{
    return _GNW95_init_mode_ex(320, 400, 8);
}

// 0x4CAD5C init_mode_640_480_16
int _init_mode_640_480_16()
{
    return -1;
}

// 0x4CAD64 init_mode_640_480
int _init_mode_640_480()
{
    return _init_vesa_mode(640, 480);
}

// 0x4CAD94 init_mode_640_400
int _init_mode_640_400()
{
    return _init_vesa_mode(640, 400);
}

// 0x4CADA8 init_mode_800_600
int _init_mode_800_600()
{
    return _init_vesa_mode(800, 600);
}

// 0x4CADBC init_mode_1024_768
int _init_mode_1024_768()
{
    return _init_vesa_mode(1024, 768);
}

// 0x4CADD0 init_mode_1280_1024
int _init_mode_1280_1024()
{
    return _init_vesa_mode(1280, 1024);
}

// 0x4CADF8
void _get_start_mode_()
{
}

// 0x4CADFC zero_vid_mem
void _zero_vid_mem()
{
    if (_zero_mem) {
        _zero_mem();
    }
}

// 0x4CAE1C GNW95_init_mode_ex
int _GNW95_init_mode_ex(int width, int height, int bpp)
{
    width = settings.screen.resolution_x;
    height = settings.screen.resolution_y;
    int scale = settings.screen.scale;

    // Only allow scaling if resulting game resolution is >= 640x480
    if ((width / scale) < 640 || (height / scale) < 480) {
        scale = 1;
    } else {
        width /= scale;
        height /= scale;
    }

    // CE: with [graphics] hd=1 and a 2x scale the framebuffer is kept at the
    // output resolution, so HD layers can show full-resolution pixels.
    gFramebufferScale = (settings.graphics.hd && scale == 2) ? 2 : 1;

    if (_GNW95_init_window(width, height, settings.screen.windowed, scale) == -1) {
        return -1;
    }

    if (directDrawInit(width, height, bpp) == -1) {
        return -1;
    }

    // macOS seems to require dequeuing NSApp events in order for window to
    // become visible. There is no concrete number of calls required to make
    // it happen. Sadly there is no particular event to watch for because SDL
    // marks window as shown immediately after creation (see
    // `SDL_FinishWindowCreation`).
    for (int i = 0; i < 10; i++) {
        SDL_PumpEvents();
    }

    _scr_size.left = 0;
    _scr_size.top = 0;
    _scr_size.right = width - 1;
    _scr_size.bottom = height - 1;

    _mouse_blit_trans = nullptr;
    _scr_blit = _GNW95_ShowRect;
    _zero_mem = _GNW95_zero_vid_mem;
    _mouse_blit = gFramebufferScale == kHdScale ? _GNW95_ShowCursorRect : _GNW95_ShowRect;

    // CE: HD text needs the HD presenter.
    hdTextUpdateActive();

    return 0;
}

// 0x4CAECC init_vesa_mode
int _init_vesa_mode(int width, int height)
{
    return _GNW95_init_mode_ex(width, height, 8);
}

// 0x4CAEDC GNW95_init_window
int _GNW95_init_window(int width, int height, WindowMode mode, int scale)
{
    if (gSdlWindow == nullptr) {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");

        Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI;

        if (mode == WindowMode::Fullscreen) {
            windowFlags |= SDL_WINDOW_FULLSCREEN;
        } else if (mode == WindowMode::WindowedFullscreen) {
            windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

            // Prevent SDL from minimizing the window when it loses focus.
            // Without this, SDL_WINDOW_FULLSCREEN_DESKTOP on Linux with
            // NVIDIA multi-monitor setups deactivates secondary displays
            // or triggers X11 BadRRCrtc errors.
            SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
        }

        gSdlWindow = SDL_CreateWindow(gProgramWindowTitle, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width * scale, height * scale, windowFlags);

        if (gSdlWindow == nullptr) {
            return -1;
        }

        if (!createRenderer(width, height)) {
            destroyRenderer();

            SDL_DestroyWindow(gSdlWindow);
            gSdlWindow = nullptr;

            return -1;
        }
    }

    return 0;
}

// 0x4CAF9C GNW95_init_DirectDraw
//
// CE: the screen is an owned 8-bit index plane plus a 32-bit framebuffer that
// is uploaded to the streaming texture. Indices are converted through an 8-bit
// precision palette (the game's 6-bit palette expanded by bit replication, with
// gamma and fades applied at 8 bits, see color.cc).
int directDrawInit(int width, int height, int bpp)
{
    (void)bpp;

    // HD commands for scripted test runs, HD art on Stimpak's screens.
    hdDevAutomationInit();
    hdArtAttachStimpak();

    if (!gScreenIndex.empty()) {
        directDrawFree();
    }

    gScreenWidth = width;
    gScreenHeight = height;
    gScreenIndex.assign(static_cast<size_t>(width) * height, 0);
    presenterAllocateFramebuffer();
    presenterMarkAllDirty();

    return 0;
}

// 0x4CB1B0 GNW95_reset_mode
void directDrawFree()
{
    gScreenIndex.clear();
    gScreenIndex.shrink_to_fit();
    gFramebuffer.clear();
    gFramebuffer.shrink_to_fit();
    gScreenWidth = 0;
    gScreenHeight = 0;
}

void directDrawSetPalette8InRange(const unsigned char* palette, int start, int count)
{
    if (start < 0 || count <= 0 || start + count > 256) {
        return;
    }

    for (int index = 0; index < count; index++) {
        int entry = start + index;
        unsigned char r = palette[index * 3];
        unsigned char g = palette[index * 3 + 1];
        unsigned char b = palette[index * 3 + 2];
        uint32_t rgb = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
        if (gPaletteRgb[entry] != rgb || !gPaletteValid) {
            gPaletteRgb[entry] = rgb;
            gPalette8[entry * 3] = r;
            gPalette8[entry * 3 + 1] = g;
            gPalette8[entry * 3 + 2] = b;
            gPaletteChanged[entry] = true;
            gPaletteChangedCount++;
        }
    }

    gPaletteValid = true;

    presenterUpdateHdModulation(start, count);
}

// 0x4CB310 GNW95_SetPaletteEntries
void directDrawSetPaletteInRange(unsigned char* palette, int start, int count)
{
    unsigned char palette8[256 * 3];
    if (start < 0 || count <= 0 || start + count > 256) {
        return;
    }

    for (int index = 0; index < count * 3; index++) {
        palette8[index] = colorExpand6(palette[index]);
    }

    directDrawSetPalette8InRange(palette8, start, count);
}

// 0x4CB568 GNW95_SetPalette
void directDrawSetPalette(unsigned char* palette)
{
    directDrawSetPaletteInRange(palette, 0, 256);
}

// 0x4CB68C GNW95_GetPalette
unsigned char* directDrawGetPalette()
{
    // 0x6ACA24
    static unsigned char palette[768];

    for (int index = 0; index < 768; index++) {
        palette[index] = gPalette8[index] >> 2;
    }

    return palette;
}

const unsigned char* directDrawGetPalette8()
{
    return gPalette8;
}

// 0x4CB850 GNW95_ShowRect
void _GNW95_ShowRect(unsigned char* src, int srcPitch, int unused, int srcX, int srcY, int srcWidth, int srcHeight, int destX, int destY)
{
    (void)unused;

    if (gScreenIndex.empty()) {
        return;
    }

    // Callers clip against the screen; guard anyway since the index plane is
    // owned memory now.
    if (destX < 0) {
        srcX -= destX;
        srcWidth += destX;
        destX = 0;
    }
    if (destY < 0) {
        srcY -= destY;
        srcHeight += destY;
        destY = 0;
    }
    srcWidth = std::min(srcWidth, gScreenWidth - destX);
    srcHeight = std::min(srcHeight, gScreenHeight - destY);
    if (srcWidth <= 0 || srcHeight <= 0) {
        return;
    }

    blitBufferToBuffer(src + srcPitch * srcY + srcX, srcWidth, srcHeight, srcPitch, gScreenIndex.data() + gScreenWidth * destY + destX, gScreenWidth);
    presenterUpdateHdMask(destX, destY, srcWidth, srcHeight);
    presenterConvertRect(destX, destY, srcWidth, srcHeight);
}

// CE: cursor blit in HD mode. `src` is the window content under the cursor
// with the cursor drawn over it (see `mouseShowCursor`); the screen already
// shows that window content, so pixels that are unchanged keep their HD
// block and only the cursor's own pixels become legacy.
void _GNW95_ShowCursorRect(unsigned char* src, int srcPitch, int unused, int srcX, int srcY, int srcWidth, int srcHeight, int destX, int destY)
{
    if (!gHdActive) {
        _GNW95_ShowRect(src, srcPitch, unused, srcX, srcY, srcWidth, srcHeight, destX, destY);
        return;
    }

    if (destX < 0) {
        srcX -= destX;
        srcWidth += destX;
        destX = 0;
    }
    if (destY < 0) {
        srcY -= destY;
        srcHeight += destY;
        destY = 0;
    }
    srcWidth = std::min(srcWidth, gScreenWidth - destX);
    srcHeight = std::min(srcHeight, gScreenHeight - destY);
    if (srcWidth <= 0 || srcHeight <= 0) {
        return;
    }

    for (int y = 0; y < srcHeight; y++) {
        const unsigned char* srcRow = src + static_cast<size_t>(srcY + y) * srcPitch + srcX;
        size_t offset = static_cast<size_t>(destY + y) * gScreenWidth + destX;
        for (int x = 0; x < srcWidth; x++) {
            if (gScreenIndex[offset + x] != srcRow[x]) {
                gScreenIndex[offset + x] = srcRow[x];
                gHdMask[offset + x] = 0;
            }
        }
    }

    presenterConvertRect(destX, destY, srcWidth, srcHeight);
}

bool presenterGetHdStaging(unsigned char** maskPtr, uint32_t** rgbPtr)
{
    if (!gHdActive) {
        return false;
    }

    *maskPtr = gHdStagingMask.data();
    *rgbPtr = gHdStagingRgb.data();
    return true;
}

void presenterSetHdSource(bool staging)
{
    gHdSourceIsStaging = staging && gHdActive;
}

// Clears drawing surface.
//
// 0x4CBBC8 GNW95_zero_vid_mem
void _GNW95_zero_vid_mem()
{
    if (!gProgramIsActive) {
        return;
    }

    std::fill(gScreenIndex.begin(), gScreenIndex.end(), 0);
    std::fill(gHdMask.begin(), gHdMask.end(), 0);
    presenterConvertRect(0, 0, gScreenWidth, gScreenHeight);
}

int screenGetWidth()
{
    // TODO: Make it on par with _xres;
    return rectGetWidth(&_scr_size);
}

int screenGetHeight()
{
    // TODO: Make it on par with _yres.
    return rectGetHeight(&_scr_size);
}

int screenGetVisibleHeight()
{
    int windowBottomMargin = 0;

    if (!settings.ui.iface_bar_mode) {
        windowBottomMargin = INTERFACE_BAR_HEIGHT;
    }
    return screenGetHeight() - windowBottomMargin;
}

// returns true if the game is running in fullscreen mode, false otherwise (including windowed fullscreen mode)
bool screenIsExclusiveFullscreen()
{
    Uint32 flags = SDL_GetWindowFlags(gSdlWindow);
    return (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN;
}

static bool createRenderer(int width, int height)
{
    gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, 0);
    if (gSdlRenderer == nullptr) {
        return false;
    }

    if (SDL_RenderSetLogicalSize(gSdlRenderer, width, height) != 0) {
        return false;
    }

    gSdlTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, width * gFramebufferScale, height * gFramebufferScale);
    if (gSdlTexture == nullptr) {
        return false;
    }

    presenterMarkAllDirty();

    return true;
}

static void destroyRenderer()
{
    if (gSdlTexture != nullptr) {
        SDL_DestroyTexture(gSdlTexture);
        gSdlTexture = nullptr;
    }

    if (gSdlRenderer != nullptr) {
        SDL_DestroyRenderer(gSdlRenderer);
        gSdlRenderer = nullptr;
    }
}

void handleWindowSizeChanged()
{
    movieHandleRendererReset();
    destroyRenderer();
    if (createRenderer(screenGetWidth(), screenGetHeight())) {
        // Repaint the newly created texture surface, just as on a window expose.
        windowRefreshAll(&_scr_size);
    }
    mouseDeviceRefreshWindowMapping();
}

void renderFpsCounter()
{
    if (!settings.debug.show_fps || gScreenIndex.empty()) {
        return;
    }

    static unsigned int sampleStartTicks = 0;
    static int sampleFrames = 0;
    static double fps = 0.0;

    unsigned int now = SDL_GetTicks();
    if (sampleStartTicks == 0) {
        sampleStartTicks = now;
    }

    sampleFrames++;

    unsigned int elapsed = now - sampleStartTicks;
    if (elapsed >= 500) {
        fps = sampleFrames * 1000.0 / elapsed;
        sampleFrames = 0;
        sampleStartTicks = now;
    }

    char text[32];
    snprintf(text, sizeof(text), "FPS: %.1f", fps);

    ScopedFont font(101);

    constexpr int kPadding = 2;
    int textWidth = fontGetStringWidth(text);
    int textHeight = fontGetLineHeight();
    int width = textWidth + kPadding * 2;
    int height = textHeight + kPadding * 2;

    if (width > gScreenWidth) {
        width = gScreenWidth;
    }

    if (height > gScreenHeight) {
        height = gScreenHeight;
    }

    unsigned char* pixels = gScreenIndex.data();
    bufferFill(pixels, width, height, gScreenWidth, COLOR_BLACK);
    presenterUpdateHdMask(0, 0, width, height);
    if (width > kPadding * 2 && height > kPadding * 2) {
        fontDrawText(pixels + gScreenWidth * kPadding + kPadding, text, width - kPadding * 2, gScreenWidth, COLOR_LIGHT_GREY);
    }

    presenterConvertRect(0, 0, width, height);
}

// Rebuilds the light field when the map side reports a change and converts
// the areas where it changed.
static void presenterUpdateLights()
{
    if (!gHdActive || gScreenIndex.empty()) {
        return;
    }

    if (!gHdLightsOn && !hdLightsMapEnabled()) {
        return;
    }

    // Building the field may load art, and file reads refresh the mouse,
    // which presents again. A present from inside an art read keeps the
    // previous field: locking art there would enter the art cache while it
    // is fetching (the same frame would be cached twice).
    static bool busy = false;
    if (busy || artIsLoading()) {
        return;
    }
    busy = true;

    uint64_t startCounter = SDL_GetPerformanceCounter();

    HdLightScreen screen;
    screen.index = gScreenIndex.data();
    screen.mask = gHdMask.data();
    screen.width = gScreenWidth;
    screen.height = gScreenHeight;
    screen.palette8 = gPalette8;
    if (gHdLightFieldNext.width != gHdLightField.width || gHdLightFieldNext.height != gHdLightField.height) {
        gHdLightFieldNext = gHdLightField;
    }
    bool changed = hdLightsMapUpdate(screen, &gHdLightFieldNext);
    busy = false;
    if (!changed) {
        return;
    }

    if (gHdLightField.width != gScreenWidth || gHdLightField.height != gScreenHeight) {
        hdLightFieldInit(&gHdLightField, gScreenWidth, gScreenHeight);
        hdLightFieldFinish(&gHdLightField);
    }
    hdLightFieldDiff(gHdLightField, gHdLightFieldNext, &gHdLightDirty);
    std::swap(gHdLightField, gHdLightFieldNext);
    gHdLightsOn = hdLightsMapEnabled();

    for (const HdLightRect& rect : gHdLightDirty) {
        presenterConvertRect(rect.left, rect.top, rect.width, rect.height);
    }

    gStats.lightTicks += SDL_GetPerformanceCounter() - startCounter;
    gStats.lightUpdates++;
}

const HdLightField* presenterGetLightField()
{
    return gHdLightsOn ? &gHdLightField : nullptr;
}

void renderPresent()
{
    if (gSdlTexture == nullptr || gFramebuffer.empty()) {
        return;
    }

    uint64_t startCounter = SDL_GetPerformanceCounter();

    presenterUpdateLights();

    presenterApplyPaletteChanges();

    if (gDirty.right >= gDirty.left && gDirty.bottom >= gDirty.top) {
        int fbWidth = gScreenWidth * gFramebufferScale;
        SDL_Rect rect;
        rect.x = gDirty.left * gFramebufferScale;
        rect.y = gDirty.top * gFramebufferScale;
        rect.w = (gDirty.right - gDirty.left + 1) * gFramebufferScale;
        rect.h = (gDirty.bottom - gDirty.top + 1) * gFramebufferScale;
        SDL_UpdateTexture(gSdlTexture, &rect, gFramebuffer.data() + static_cast<size_t>(rect.y) * fbWidth + rect.x, fbWidth * static_cast<int>(sizeof(uint32_t)));
        gStats.uploadedPixels += static_cast<uint64_t>(rect.w) * rect.h;
        gDirty = { INT_MAX, INT_MAX, INT_MIN, INT_MIN };
    }

    SDL_RenderClear(gSdlRenderer);
    SDL_RenderCopy(gSdlRenderer, gSdlTexture, nullptr, nullptr);
    // render movie SDL texture if present
    movieRenderDirectOverlay();
    SDL_RenderPresent(gSdlRenderer);

    uint64_t elapsed = SDL_GetPerformanceCounter() - startCounter;
    hdDevAutomationOnPresent();
    gStats.presents++;
    gStats.presentTicks += elapsed;
    gStats.presentMaxTicks = std::max(gStats.presentMaxTicks, elapsed);
}

int screenGetFramebufferScale()
{
    return gFramebufferScale;
}

int screenGetFramebufferWidth()
{
    return gScreenWidth * gFramebufferScale;
}

int screenGetFramebufferHeight()
{
    return gScreenHeight * gFramebufferScale;
}

const unsigned char* screenGetIndexPlane()
{
    return gScreenIndex.empty() ? nullptr : gScreenIndex.data();
}

const uint32_t* screenGetFramebuffer()
{
    presenterApplyPaletteChanges();
    return gFramebuffer.empty() ? nullptr : gFramebuffer.data();
}

void presenterGetStats(PresenterStats* stats)
{
    double frequency = static_cast<double>(SDL_GetPerformanceFrequency());
    stats->presents = gStats.presents;
    stats->presentMs = gStats.presents != 0 ? gStats.presentTicks * 1000.0 / frequency / gStats.presents : 0.0;
    stats->presentMaxMs = gStats.presentMaxTicks * 1000.0 / frequency;
    stats->convertMs = gStats.convertTicks * 1000.0 / frequency;
    stats->convertedPixels = gStats.convertedPixels;
    stats->paletteMs = gStats.paletteTicks * 1000.0 / frequency;
    stats->paletteUpdates = gStats.paletteUpdates;
    stats->uploadedPixels = gStats.uploadedPixels;
    stats->lightMs = gStats.lightTicks * 1000.0 / frequency;
    stats->lightUpdates = gStats.lightUpdates;
    stats->framebufferWidth = screenGetFramebufferWidth();
    stats->framebufferHeight = screenGetFramebufferHeight();
}

void presenterResetStats()
{
    gStats = {};
}

} // namespace fallout
