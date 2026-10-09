// TextRenderer for the editor's Windows-font text (status bar, program name, menus),
// using stb_truetype and the embedded Liberation Sans (metric-compatible with Arial):
// regular, italic and bold italic (a bold upright font is drawn regular).
// Integer advances like GDI (lfHeight < 0 means character (em) height in pixels).
// antialias = false renders like Windows XP with ClearType off (binary glyphs); true
// blends the glyph coverage with the background (smoother, as preferred by the user).
#pragma once

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "../TextRenderer.h"

namespace sq8l::gui {

class StbTextRenderer final : public TextRenderer {
public:
    explicit StbTextRenderer(bool antialias = false);
    ~StbTextRenderer() override;

    int textWidth(const Font& font, const std::string& text) override;
    int textHeight(const Font& font) override;
    void drawText(Bitmap& target, int x, int y, const Rect& clip, const Font& font, const std::string& text) override;
    // Antialiased, glyphs placed at fractional positions; grid fitted below twice the size,
    // where anti-aliasing alone leaves the strokes of a small face without a solid pixel.
    void drawTextScaled(Bitmap& target, float x, float baseline, float scale, const Rect& clip, const Font& font,
                        const std::string& text, float embolden = 0) override;

private:
    struct Face;
    struct Glyph {
        int advance = 0, xoff = 0, yoff = 0, w = 0, h = 0;
        std::basic_string<uint8_t> mask;  // w*h coverage 0..255 (binary mode: 0 or 255)
    };
    struct Sized;
    Sized& sized(const Font& font);

    const Face* face(const Font& font) const;

    bool antialias_;
    std::unique_ptr<Face> regular_, italic_, boldItalic_;
    std::map<std::tuple<const Face*, int>, std::unique_ptr<Sized>> cache_;
    std::vector<uint8_t> scratch_, scratch2_;  // a glyph rendered by drawTextScaled
};

}  // namespace sq8l::gui
