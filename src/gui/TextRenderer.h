// Text drawn with Windows fonts (status bar labels, program name edit box, menus) is the
// only part of the editor that is not made of the original's bitmaps. It is delegated to a
// TextRenderer supplied by the platform layer; without one, text is simply not drawn.
#pragma once

#include <string>

#include "Bitmap.h"

namespace sq8l::gui {

// A VCL TFont as used by the form (Font.Name, Font.Height in pixels: negative = character
// height without internal leading, Font.Style, Font.Color).
struct Font {
    std::string face = "MS Sans Serif";
    int height = -11;
    bool bold = false, italic = false;
    Color color = 0;
};

class TextRenderer {
public:
    virtual ~TextRenderer() = default;
    // Advance width of `text` in pixels.
    virtual int textWidth(const Font& font, const std::string& text) = 0;
    // Line height (tmHeight = ascent + descent) in pixels.
    virtual int textHeight(const Font& font) = 0;
    // Draw `text` with its top-left cell corner at (x, y) (TA_TOP | TA_LEFT, transparent
    // background), clipped to `clip` (target coordinates).
    virtual void drawText(Bitmap& target, int x, int y, const Rect& clip, const Font& font,
                          const std::string& text) = 0;
    // `text` laid out at `font` (its glyph advances as drawText has them), then drawn enlarged
    // by `scale` with its baseline's left end at (x, baseline), fractional target pixels (HD
    // graphics); `embolden` widens the strokes by that many layout pixels. The default draws
    // the enlarged font with drawText.
    virtual void drawTextScaled(Bitmap& target, float x, float baseline, float scale, const Rect& clip,
                                const Font& font, const std::string& text, float embolden = 0) {
        (void)embolden;
        Font f = font;
        f.height = static_cast<int>(font.height * scale + (font.height < 0 ? -0.5f : 0.5f));
        drawText(target, static_cast<int>(x + 0.5f), static_cast<int>(baseline - textHeight(f) * 0.8f + 0.5f), clip, f,
                 text);
    }
};

}  // namespace sq8l::gui
