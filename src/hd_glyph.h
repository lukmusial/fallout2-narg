#ifndef FALLOUT_HD_GLYPH_H_
#define FALLOUT_HD_GLYPH_H_

#include <vector>

namespace fallout {

// 2x glyph sources for HD text (Phase 2.9, see hd_text.h). Engine-independent
// so the upscalers can be unit-tested without game data.
//
// A glyph comes in as a level map: one byte per pixel, 0 = background, 7 =
// the text colour, 1..6 the interface fonts' (AAF) anti-aliasing steps
// (level / 7 of the text colour over the background); values above 7 count
// as 7. Bitmap fonts (FON) are levels 0 and 7.
enum class HdGlyphStyle {
    // Edge-preserving 2x of the original pixels (EPX / Scale2x on the level
    // map): the original look, with rounded diagonals and corners.
    Sharp,
    // Re-rasterised with real anti-aliasing: the coverage is interpolated
    // (bicubic, 8x), thresholded softly and box-filtered down to 2x.
    Smooth,
};

// The 2x glyph may reach this many logical pixels outside the glyph box.
constexpr int kHdGlyphMargin = 1;

// Upscales a glyph to 2x coverage (alpha 0..255). The result is
// (width + 2 * kHdGlyphMargin) * 2 by (height + 2 * kHdGlyphMargin) * 2;
// the glyph box starts at (2 * kHdGlyphMargin, 2 * kHdGlyphMargin).
void hdGlyphUpscale(const unsigned char* levels, int width, int height, HdGlyphStyle style, std::vector<unsigned char>* alpha);

// Parses a style name ("sharp", "smooth"); false when unknown.
bool hdGlyphStyleFromName(const char* name, HdGlyphStyle* style);
const char* hdGlyphStyleName(HdGlyphStyle style);

} // namespace fallout

#endif /* FALLOUT_HD_GLYPH_H_ */
