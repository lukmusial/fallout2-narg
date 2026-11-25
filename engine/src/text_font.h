#ifndef TEXT_FONT_H
#define TEXT_FONT_H

#include "color.h"
#include "draw.h"

#include <algorithm>
#include <vector>

namespace fallout {

typedef void FontManagerSetCurrentFontProc(int font);
typedef void FontManagerDrawTextProc(unsigned char* buffer, const char* string, int length, int pitch, ColorWithFlags color);
typedef int FontManagerGetLineHeightProc();
typedef int FontManagerGetStringWidthProc(const char* string);
typedef int FontManagerGetCharacterWidthProc(int ch);
typedef int FontManagerGetMonospacedStringWidthProc(const char* string);
typedef int FontManagerGetLetterSpacingProc();
typedef int FontManagerGetBufferSizeProc(const char* string);
typedef int FontManagerGetMonospacedCharacterWidth();

typedef struct FontManager {
    int minFont;
    int maxFont;
    FontManagerSetCurrentFontProc* setCurrentProc;
    FontManagerDrawTextProc* drawTextProc;
    FontManagerGetLineHeightProc* getLineHeightProc;
    FontManagerGetStringWidthProc* getStringWidthProc;
    FontManagerGetCharacterWidthProc* getCharacterWidthProc;
    FontManagerGetMonospacedStringWidthProc* getMonospacedStringWidthProc;
    FontManagerGetLetterSpacingProc* getLetterSpacingProc;
    FontManagerGetBufferSizeProc* getBufferSizeProc;
    FontManagerGetMonospacedCharacterWidth* getMonospacedCharacterWidthProc;
} FontManager;

extern FontManager gTextFontManager;
extern int gCurrentFont;
extern int gFontManagersCount;
extern FontManagerDrawTextProc* fontDrawTextPtr;
extern FontManagerGetLineHeightProc* fontGetLineHeight;
extern FontManagerGetStringWidthProc* fontGetStringWidth;
extern FontManagerGetCharacterWidthProc* fontGetCharacterWidth;
extern FontManagerGetMonospacedStringWidthProc* fontGetMonospacedStringWidth;
extern FontManagerGetLetterSpacingProc* fontGetLetterSpacing;
extern FontManagerGetBufferSizeProc* fontGetBufferSize;
extern FontManagerGetMonospacedCharacterWidth* fontGetMonospacedCharacterWidth;

// CE: HD text (hd_text.h): with [graphics] hd=1 and hd_text=1 every text
// draw also draws the string at 2x into the window's HD layer.
extern bool gHdTextActive;
void hdTextDraw(unsigned char* buffer, const char* string, int length, int pitch, ColorWithFlags color);

inline void fontDrawText(unsigned char* buffer, const char* string, int length, int pitch, ColorWithFlags color)
{
    if (fontDrawTextPtr != nullptr) {
        if (gHdTextActive) {
            hdTextDraw(buffer, string, length, pitch, color);
        } else {
            fontDrawTextPtr(buffer, string, length, pitch, color);
        }
    }
}

inline void fontDrawText(unsigned char* buffer, const char* string, int length, int pitch, Color color)
{
    fontDrawText(buffer, string, length, pitch, color | DRAW_TEXT_FLAG_NONE);
}

void fontDrawText2D(const Buffer2D& dest, int xPos, int yPos, const char* string, int length, ColorWithFlags color);

inline void fontDrawText2D(const Buffer2D& dest, int xPos, int yPos, const char* string, int length, Color color)
{
    fontDrawText2D(dest, xPos, yPos, string, length, color | DRAW_TEXT_FLAG_NONE);
}

// CE: font data for the HD text renderer (hd_text.cc), which lays out the
// same glyphs at 2x.
struct FontHdInfo {
    // Interface font (AAF, 100+) or bitmap font (FON).
    bool interfaceFont = false;
    // Rows glyphs are drawn in: FON line height, AAF maximum glyph height.
    int rows = 0;
    int letterSpacing = 0;
    // AAF: advance of a space.
    int wordSpacing = 0;
    // FON: number of glyphs.
    int glyphCount = 0;
    // What fontGetMonospacedCharacterWidth returns for the font.
    int monospacedWidth = 0;
};

bool fontGetHdInfo(int font, FontHdInfo* info);

// Glyph `index` of `font` as levels (0 background .. 7 text colour, see
// hd_glyph.h), row-major `width` x `height`. FON glyphs are `rows` tall,
// AAF glyphs are drawn bottom-aligned in `rows`.
bool fontGetHdGlyph(int font, int index, int* width, int* height, std::vector<unsigned char>* levels);

int textFontsInit();
void textFontsExit();
int textFontLoad(int font);
int fontManagerAdd(FontManager* fontManager);
int fontGetCurrent();
void fontSetCurrent(int font);

class ScopedFont {
public:
    ScopedFont(int font)
        : _previousFont(fontGetCurrent())
    {
        fontSetCurrent(font);
    }

    ~ScopedFont()
    {
        fontSetCurrent(_previousFont);
    }

    ScopedFont(const ScopedFont&) = delete;
    ScopedFont& operator=(const ScopedFont&) = delete;

private:
    int _previousFont;
};

} // namespace fallout

#endif /* TEXT_FONT_H */
