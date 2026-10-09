#include "StbTextRenderer.h"

#include <algorithm>
#include <cmath>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../../../third_party/stb/stb_truetype.h"

namespace sq8l::gui {

namespace fonts {
extern const uint8_t kLiberationSansRegular[];
extern const size_t kLiberationSansRegularSize;
extern const uint8_t kLiberationSansItalic[];
extern const size_t kLiberationSansItalicSize;
extern const uint8_t kLiberationSansBoldItalic[];
extern const size_t kLiberationSansBoldItalicSize;
}  // namespace fonts

struct StbTextRenderer::Face {
    stbtt_fontinfo info{};
    explicit Face(const uint8_t* data) { stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0)); }
};

struct StbTextRenderer::Sized {
    float scale = 1;
    int ascent = 0, descent = 0;
    const Face* face = nullptr;
    bool antialias = false;
    std::map<int, Glyph> glyphs;

    const Glyph& glyph(int cp) {
        auto it = glyphs.find(cp);
        if (it != glyphs.end()) return it->second;
        Glyph g;
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&face->info, cp, &adv, &lsb);
        g.advance = static_cast<int>(std::lround(adv * scale));
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&face->info, cp, scale, scale, &x0, &y0, &x1, &y1);
        g.w = x1 - x0;
        g.h = y1 - y0;
        g.xoff = x0;
        g.yoff = y0;
        if (g.w > 0 && g.h > 0) {
            std::vector<uint8_t> cov(static_cast<size_t>(g.w) * g.h);
            stbtt_MakeCodepointBitmap(&face->info, cov.data(), g.w, g.h, g.w, scale, scale, cp);
            g.mask.resize(cov.size());
            for (size_t i = 0; i < cov.size(); i++) g.mask[i] = antialias ? cov[i] : (cov[i] >= 128 ? 255 : 0);
        }
        return glyphs.emplace(cp, std::move(g)).first->second;
    }
};

StbTextRenderer::StbTextRenderer(bool antialias)
    : antialias_(antialias),
      regular_(std::make_unique<Face>(fonts::kLiberationSansRegular)),
      italic_(std::make_unique<Face>(fonts::kLiberationSansItalic)),
      boldItalic_(std::make_unique<Face>(fonts::kLiberationSansBoldItalic)) {}

StbTextRenderer::~StbTextRenderer() = default;

const StbTextRenderer::Face* StbTextRenderer::face(const Font& font) const {
    if (font.italic) return font.bold ? boldItalic_.get() : italic_.get();
    return regular_.get();
}

StbTextRenderer::Sized& StbTextRenderer::sized(const Font& font) {
    const int px = font.height < 0 ? -font.height : (font.height > 0 ? font.height : 11);
    const auto key = std::make_tuple(face(font), font.height);
    auto it = cache_.find(key);
    if (it != cache_.end()) return *it->second;
    auto s = std::make_unique<Sized>();
    s->face = face(font);
    s->antialias = antialias_;
    // Negative lfHeight: em height; positive: cell height (ascent + descent).
    s->scale = font.height < 0 ? stbtt_ScaleForMappingEmToPixels(&s->face->info, static_cast<float>(px))
                               : stbtt_ScaleForPixelHeight(&s->face->info, static_cast<float>(px));
    int a, d, g;
    stbtt_GetFontVMetrics(&s->face->info, &a, &d, &g);
    s->ascent = static_cast<int>(std::lround(a * s->scale));
    s->descent = static_cast<int>(std::lround(-d * s->scale));
    return *cache_.emplace(key, std::move(s)).first->second;
}

int StbTextRenderer::textWidth(const Font& font, const std::string& text) {
    Sized& s = sized(font);
    int w = 0;
    for (unsigned char c : text) w += s.glyph(c).advance;
    return w;
}

int StbTextRenderer::textHeight(const Font& font) {
    Sized& s = sized(font);
    return s.ascent + s.descent;
}

void StbTextRenderer::drawText(Bitmap& target, int x, int y, const Rect& clip, const Font& font, const std::string& text) {
    Sized& s = sized(font);
    const Rect c = clip.intersect(Rect{0, 0, target.width(), target.height()});
    int pen = x;
    const int baseline = y + s.ascent;
    for (unsigned char ch : text) {
        const Glyph& g = s.glyph(ch);
        for (int gy = 0; gy < g.h; gy++) {
            const int py = baseline + g.yoff + gy;
            if (py < c.top || py >= c.bottom) continue;
            for (int gx = 0; gx < g.w; gx++) {
                const int px = pen + g.xoff + gx;
                if (px < c.left || px >= c.right) continue;
                const unsigned a = g.mask[static_cast<size_t>(gy) * g.w + gx];
                if (a == 0) continue;
                if (a == 255) {
                    target.setPixel(px, py, font.color);
                    continue;
                }
                const Color bg = target.pixel(px, py);
                const auto mix = [a](unsigned f, unsigned b) { return (f * a + b * (255 - a) + 127) / 255; };
                target.setPixel(px, py, rgb(static_cast<int>(mix((font.color >> 16) & 0xFF, (bg >> 16) & 0xFF)),
                                            static_cast<int>(mix((font.color >> 8) & 0xFF, (bg >> 8) & 0xFF)),
                                            static_cast<int>(mix(font.color & 0xFF, bg & 0xFF))));
            }
        }
        pen += g.advance;
    }
}

void StbTextRenderer::drawTextScaled(Bitmap& target, float x, float baseline, float scale, const Rect& clip,
                                     const Font& font, const std::string& text, float embolden) {
    Sized& s = sized(font);  // the layout: drawText's advances
    const stbtt_fontinfo* info = &s.face->info;
    // Grid fitting at the small sizes. stb_truetype does not hint, so around one window
    // pixel per form pixel the strokes of an 11 pixel face fall across pixel boundaries and
    // hardly a pixel is fully covered: the text turns soft, where the original's hinted text
    // had solid stems. Here the origin is snapped to the pixel grid and the coverage is
    // pushed towards 0 or 1, which gives the stems their cores back. It fades out by twice
    // the size, where plain anti-aliasing is what is wanted; `embolden`, which compensates
    // for the thin unhinted strokes, fades out with it.
    const float sharp = std::max(0.f, std::min(1.f, 2.f - scale));
    if (sharp > 0) x = std::floor(x + 0.5f), baseline = std::floor(baseline + 0.5f);
    const float contrast = 1.f + sharp, mid = 0.5f - 0.15f * sharp;
    const float sc = s.scale * scale, wider = std::max(embolden * (1.f - 0.5f * sharp) * scale, 0.f);
    const Rect c = clip.intersect(Rect{0, 0, target.width(), target.height()});
    const float fy = baseline - std::floor(baseline);
    const int by = static_cast<int>(std::floor(baseline));
    float pen = x;
    for (unsigned char ch : text) {
        // the glyph at pen and, when emboldened, again `wider` to the right (the coverage is
        // the larger of the two)
        int ux0 = 0, uy0 = 0, ux1 = 0, uy1 = 0, bx[2] = {}, gx0[2] = {}, gy0[2] = {}, gw[2] = {}, gh[2] = {};
        float fx[2] = {};
        const int copies = wider > 0 ? 2 : 1;
        for (int k = 0; k < copies; k++) {
            const float p = pen + (k ? wider : 0.f);
            bx[k] = static_cast<int>(std::floor(p));
            fx[k] = p - std::floor(p);
            int x0, y0, x1, y1;
            stbtt_GetCodepointBitmapBoxSubpixel(info, ch, sc, sc, fx[k], fy, &x0, &y0, &x1, &y1);
            gx0[k] = bx[k] + x0, gy0[k] = by + y0, gw[k] = x1 - x0, gh[k] = y1 - y0;
            if (k == 0) ux0 = gx0[0], uy0 = gy0[0], ux1 = gx0[0] + gw[0], uy1 = gy0[0] + gh[0];
            else ux0 = std::min(ux0, gx0[1]), uy0 = std::min(uy0, gy0[1]), ux1 = std::max(ux1, gx0[1] + gw[1]),
                 uy1 = std::max(uy1, gy0[1] + gh[1]);
        }
        const int w = ux1 - ux0, h = uy1 - uy0;
        if (gw[0] > 0 && gh[0] > 0 && !Rect{ux0, uy0, ux1, uy1}.intersect(c).empty()) {
            scratch_.assign(static_cast<size_t>(w) * h, 0);
            for (int k = 0; k < copies; k++) {
                scratch2_.assign(static_cast<size_t>(gw[k]) * gh[k], 0);
                stbtt_MakeCodepointBitmapSubpixel(info, scratch2_.data(), gw[k], gh[k], gw[k], sc, sc, fx[k], fy, ch);
                for (int yy = 0; yy < gh[k]; yy++)
                    for (int xx = 0; xx < gw[k]; xx++) {
                        uint8_t& d = scratch_[static_cast<size_t>(gy0[k] - uy0 + yy) * w + (gx0[k] - ux0 + xx)];
                        d = std::max(d, scratch2_[static_cast<size_t>(yy) * gw[k] + xx]);
                    }
            }
            if (contrast > 1.f)
                for (uint8_t& a : scratch_) {
                    const float v = (a / 255.f - mid) * contrast + 0.5f;
                    a = static_cast<uint8_t>(std::max(0.f, std::min(1.f, v)) * 255.f + 0.5f);
                }
            for (int gy = 0; gy < h; gy++) {
                const int py = uy0 + gy;
                if (py < c.top || py >= c.bottom) continue;
                for (int gx = 0; gx < w; gx++) {
                    const int px = ux0 + gx;
                    const unsigned a = scratch_[static_cast<size_t>(gy) * w + gx];
                    if (a == 0 || px < c.left || px >= c.right) continue;
                    const Color bg = target.pixel(px, py);
                    const auto mix = [a](unsigned f, unsigned b) { return (f * a + b * (255 - a) + 127) / 255; };
                    target.setPixel(px, py, rgb(static_cast<int>(mix((font.color >> 16) & 0xFF, (bg >> 16) & 0xFF)),
                                                static_cast<int>(mix((font.color >> 8) & 0xFF, (bg >> 8) & 0xFF)),
                                                static_cast<int>(mix(font.color & 0xFF, bg & 0xFF))));
                }
            }
        }
        pen += s.glyph(ch).advance * scale;
    }
}

}  // namespace sq8l::gui
