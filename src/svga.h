#ifndef FALLOUT_SVGA_H_
#define FALLOUT_SVGA_H_

#include <stdint.h>

#include <SDL.h>

#include "fps_limiter.h"
#include "geometry.h"

namespace fallout {

extern Rect _scr_size;
extern void (*_scr_blit)(unsigned char* src, int src_pitch, int unused, int src_x, int src_y, int src_width, int src_height, int dest_x, int dest_y);
extern void (*_zero_mem)();

extern SDL_Window* gSdlWindow;
extern SDL_Renderer* gSdlRenderer;
extern SDL_Texture* gSdlTexture;
extern FpsLimiter sharedFpsLimiter;

enum class WindowMode : int {
    Fullscreen = 0,
    Windowed = 1,
    WindowedFullscreen = 2,
};

int _init_mode_320_200();
int _init_mode_320_400();
int _init_mode_640_480_16();
int _init_mode_640_480();
int _init_mode_640_400();
int _init_mode_800_600();
int _init_mode_1024_768();
int _init_mode_1280_1024();
void _get_start_mode_();
void _zero_vid_mem();
int _GNW95_init_mode_ex(int width, int height, int bpp);
int _init_vesa_mode(int width, int height);
int _GNW95_init_window(int width, int height, WindowMode mode, int scale);
int directDrawInit(int width, int height, int bpp);
void directDrawFree();
void directDrawSetPaletteInRange(unsigned char* palette, int start, int count);
void directDrawSetPalette(unsigned char* palette);
unsigned char* directDrawGetPalette();
// CE: 8-bit precision palette (R, G, B bytes per entry), as presented.
void directDrawSetPalette8InRange(const unsigned char* palette, int start, int count);
const unsigned char* directDrawGetPalette8();
void _GNW95_ShowRect(unsigned char* src, int src_pitch, int unused, int src_x, int src_y, int src_width, int src_height, int dest_x, int dest_y);
void _GNW95_zero_vid_mem();

int screenGetWidth();
int screenGetHeight();
int screenGetVisibleHeight();
void handleWindowSizeChanged();
void renderFpsCounter();
void renderPresent();
bool screenIsExclusiveFullscreen();

// CE: the presented framebuffer (0x00RRGGBB), `screenGetFramebufferScale()`
// output pixels per logical pixel in each direction.
int screenGetFramebufferScale();
int screenGetFramebufferWidth();
int screenGetFramebufferHeight();
const uint32_t* screenGetFramebuffer();
// What the game put on screen, one palette index per logical pixel.
const unsigned char* screenGetIndexPlane();

struct PresenterStats {
    uint64_t presents;
    double presentMs; // average per renderPresent (palette apply, upload, present)
    double presentMaxMs;
    double convertMs; // total time converting index rects to the framebuffer
    uint64_t convertedPixels; // logical pixels converted
    double paletteMs; // total time applying palette changes
    uint64_t paletteUpdates;
    uint64_t uploadedPixels; // framebuffer pixels uploaded to the texture
    int framebufferWidth;
    int framebufferHeight;
};

void presenterGetStats(PresenterStats* stats);
void presenterResetStats();

} // namespace fallout

#endif /* FALLOUT_SVGA_H_ */
