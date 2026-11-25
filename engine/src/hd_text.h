#ifndef FALLOUT_HD_TEXT_H_
#define FALLOUT_HD_TEXT_H_

#include "color.h"
#include "draw.h"
#include "hd_glyph.h"

namespace fallout {

// HD text (Phase 2.9): text drawn at 2x output resolution.
//
// With `[graphics] hd=1` and `hd_text=1` every text draw still draws the
// string into the 8-bit buffer exactly as before (layout, hit-testing and
// the legacy pixels do not change), then draws the same string with 2x
// glyphs (`hd_text_style`, see hd_glyph.h) into the text overlay of the
// window's HD layer (hdLayerDrawText). The legacy pixels stay the truth: the
// 2x text shows only while the window buffer still holds the legacy text.
//
// Text drawn into a buffer that is not a window (button images with labels,
// floating text objects) is remembered with the pixels it wrote; when that
// buffer is later copied into a window (blitBufferToBuffer[Trans]) and the
// text pixels are still there, the 2x text is drawn at the destination.

// True while HD text is on (hd=1 at scale 2 and hd_text=1); fontDrawText
// (text_font.h) then calls hdTextDraw.
extern bool gHdTextActive;

// Reads the settings; call once the presenter is up (and after changing them).
void hdTextUpdateActive();

// Dev/test: switches HD text on or off and selects the glyph style live
// (cached glyphs are dropped). Text already drawn keeps its look until redrawn.
void hdTextSetMode(bool enabled, HdGlyphStyle style);
bool hdTextIsActive();
HdGlyphStyle hdTextGetStyle();

// The scaled interface-font draw (main menu labels with scaled controls).
// `legacy` performs the 8-bit draw.
void hdTextDrawScaled(const Buffer2D& dest, int x, int y, const char* string, ColorWithFlags color, float scale,
    void (*legacy)(const Buffer2D& dest, int x, int y, const char* string, ColorWithFlags color, float scale));

// Copies a rectangle like blitBufferToBuffer / blitBufferToBufferTrans and
// carries remembered text along (see above). Called by those functions when
// hdTextHasRecords() is true.
bool hdTextHasRecords();
void hdTextBlit(const unsigned char* src, int width, int height, int srcPitch, unsigned char* dest, int destPitch, bool transparent);

// After bufferOutline drew an outline around remembered text in `buffer`
// (floating text): the 2x text gets the outline too.
void hdTextOutline(unsigned char* buffer, int width, int height, int pitch, Color color);

struct HdTextStats {
    unsigned long long draws; // strings drawn at 2x into windows
    unsigned long long replays; // remembered strings drawn at a blit
    unsigned long long glyphsCached;
    double totalMs; // time in the 2x path (layout, glyphs, layer)
};

void hdTextGetStats(HdTextStats* stats);

} // namespace fallout

#endif /* FALLOUT_HD_TEXT_H_ */
