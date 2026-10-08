#include "HdRenderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

#include "EditorView.h"
#include "Sprite.h"

namespace sq8l::gui {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr Color kNoFrame = 0xFFFFFFFFu;  // (colours are 0x00RRGGBB)

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
inline int ch(Color c, int shift) { return int((c >> shift) & 0xff); }
inline Color mix(Color a, Color b, float t) {  // a..b
    return rgb(int(ch(a, 16) + (ch(b, 16) - ch(a, 16)) * t + 0.5f), int(ch(a, 8) + (ch(b, 8) - ch(a, 8)) * t + 0.5f),
               int(ch(a, 0) + (ch(b, 0) - ch(a, 0)) * t + 0.5f));
}

// ---------------------------------------------------------------- drawing at window resolution
struct Hd {
    Bitmap& b;
    Rect clip;  // window pixels

    void blend(int x, int y, Color c, float a) {
        if (a <= 0.f || !clip.contains(x, y)) return;
        Color* p = b.row(y) + x;
        *p = a >= 1.f ? c : mix(*p, c, a);
    }
    void fill(const Rect& r, Color c) {
        const Rect k = r.intersect(clip);
        for (int y = k.top; y < k.bottom; y++)
            for (int x = k.left; x < k.right; x++) b.row(y)[x] = c;
    }
    // Area of (x0,y0)-(x1,y1) grown by `pad`, clipped, in whole pixels.
    Rect area(float x0, float y0, float x1, float y1, float pad) const {
        return Rect{int(std::floor(std::min(x0, x1) - pad)), int(std::floor(std::min(y0, y1) - pad)),
                    int(std::ceil(std::max(x0, x1) + pad)) + 1, int(std::ceil(std::max(y0, y1) + pad)) + 1}
            .intersect(clip);
    }
    // A segment with round ends, half width r; `soft` = width of the edge ramp (1 = anti-aliasing,
    // more = a glow), alpha at the centre.
    void capsule(float x0, float y0, float x1, float y1, float r, float soft, Color c, float alpha = 1.f) {
        const float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
        const Rect k = area(x0, y0, x1, y1, r + soft);
        for (int y = k.top; y < k.bottom; y++)
            for (int x = k.left; x < k.right; x++) {
                const float px = x + 0.5f - x0, py = y + 0.5f - y0;
                const float t = len2 > 0 ? clampf((px * dx + py * dy) / len2, 0.f, 1.f) : 0.f;
                const float d = std::hypot(px - t * dx, py - t * dy);
                blend(x, y, c, alpha * clampf((r - d) / soft + 0.5f, 0.f, 1.f));
            }
    }
    // A disc with a colour per normalized position (-1..1 at the radius).
    void disc(float cx, float cy, float r, float soft, const std::function<Color(float, float)>& shade,
              float alpha = 1.f) {
        const Rect k = area(cx, cy, cx, cy, r + soft);
        for (int y = k.top; y < k.bottom; y++)
            for (int x = k.left; x < k.right; x++) {
                const float px = x + 0.5f - cx, py = y + 0.5f - cy, d = std::hypot(px, py);
                const float a = clampf((r - d) / soft + 0.5f, 0.f, 1.f);
                if (a > 0) blend(x, y, shade(px / r, py / r), a * alpha);
            }
    }
    // A rounded rectangle, colour per normalized position (0..1 across, 0..1 down).
    void roundRect(float x0, float y0, float x1, float y1, float rad, const std::function<Color(float, float)>& shade,
                   float alpha = 1.f) {
        const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, hx = (x1 - x0) / 2, hy = (y1 - y0) / 2;
        const Rect k = area(x0, y0, x1, y1, 1);
        for (int y = k.top; y < k.bottom; y++)
            for (int x = k.left; x < k.right; x++) {
                const float px = std::fabs(x + 0.5f - cx) - (hx - rad), py = std::fabs(y + 0.5f - cy) - (hy - rad);
                const float d =
                    std::hypot(std::max(px, 0.f), std::max(py, 0.f)) + std::min(std::max(px, py), 0.f) - rad;
                const float a = clampf(0.5f - d, 0.f, 1.f);
                if (a > 0) blend(x, y, shade((x + 0.5f - x0) / (x1 - x0), (y + 0.5f - y0) / (y1 - y0)), a * alpha);
            }
    }
    // A filled triangle (any winding).
    void triangle(float ax, float ay, float bx, float by, float cx, float cy, Color c) {
        const float area2 = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        if (std::fabs(area2) < 1e-3f) return;
        const float s = area2 > 0 ? 1.f : -1.f;
        auto edge = [&](float x0, float y0, float x1, float y1, float px, float py) {
            const float ex = x1 - x0, ey = y1 - y0, l = std::hypot(ex, ey);
            return s * ((px - x0) * ey - (py - y0) * ex) / l;  // > 0 outside
        };
        const Rect k =
            area(std::min({ax, bx, cx}), std::min({ay, by, cy}), std::max({ax, bx, cx}), std::max({ay, by, cy}), 1);
        for (int y = k.top; y < k.bottom; y++)
            for (int x = k.left; x < k.right; x++) {
                const float px = x + 0.5f, py = y + 0.5f;
                const float d = std::max(
                    {edge(ax, ay, bx, by, px, py), edge(bx, by, cx, cy, px, py), edge(cx, cy, ax, ay, px, py)});
                blend(x, y, c, clampf(0.5f - d, 0.f, 1.f));
            }
    }
};

// The classic frame enlarged with sharp bilinear sampling (crisp, even pixels at any scale).
void sharpBlit(const Bitmap& src, double S, Bitmap& out, const Rect& region) {
    const int sw = src.width(), sh = src.height();
    std::vector<int> x0(size_t(out.width())), x1(size_t(out.width()));
    std::vector<float> fx(size_t(out.width()));
    for (int X = region.left; X < region.right; X++) {
        const double sx = (X + 0.5) / S - 0.5;
        const int ix = int(std::floor(sx));
        const float f = clampf(float((sx - ix - 0.5) * S + 0.5), 0.f, 1.f);
        x0[size_t(X)] = clampi(ix, 0, sw - 1);
        x1[size_t(X)] = clampi(ix + 1, 0, sw - 1);
        fx[size_t(X)] = f;
    }
    for (int Y = region.top; Y < region.bottom; Y++) {
        const double sy = (Y + 0.5) / S - 0.5;
        const int iy = int(std::floor(sy));
        const float fy = clampf(float((sy - iy - 0.5) * S + 0.5), 0.f, 1.f);
        const Color* r0 = src.row(clampi(iy, 0, sh - 1));
        const Color* r1 = src.row(clampi(iy + 1, 0, sh - 1));
        Color* o = out.row(Y);
        for (int X = region.left; X < region.right; X++) {
            const size_t i = size_t(X);
            const Color top = mix(r0[x0[i]], r0[x1[i]], fx[i]), bottom = mix(r1[x0[i]], r1[x1[i]], fx[i]);
            o[X] = mix(top, bottom, fy);
        }
    }
}

// ---------------------------------------------------------------- glyphs read from the original
// A display character as segments (capsules in sprite pixels, measured from the original's
// glyphs). Which segments a frame lights is read from the frame itself: a segment is lit when
// its probe pixels, which no other segment touches, are bright.
struct Seg {
    float x0, y0, x1, y1;
    int px0, py0, px1, py1;  // probe pixels
    float level = 1.f;       // brightness when lit
};

struct SegmentFont {
    std::vector<Seg> segs;
    size_t ghosts = 0;  // the first `ghosts` segments also show unlit (dim)
    float radius = 0.6f;
    std::vector<uint32_t> lit;  // per frame: bit i = segment i lit
    Color litColor = 0, ghostColor = 0, background = 0;
};

int channelOf(Color c, int channel) { return ch(c, channel == 0 ? 16 : channel == 1 ? 8 : 0); }

SegmentFont analyse(std::vector<Seg> segs, size_t ghosts, float radius, const Sprite& sprite, int channel,
                    int threshold) {
    SegmentFont font;
    font.segs = std::move(segs);
    font.ghosts = ghosts;
    font.radius = radius;
    font.lit.assign(size_t(sprite.count()), 0);
    int brightest = 0;
    for (int f = 0; f < sprite.count(); f++) {
        const ImageView img = sprite.frame(f);
        for (size_t s = 0; s < font.segs.size(); s++) {
            const Seg& g = font.segs[s];
            const int probe =
                std::min(channelOf(img.at(g.px0, g.py0), channel), channelOf(img.at(g.px1, g.py1), channel));
            if (probe > threshold)
                font.lit[size_t(f)] |= 1u << s;
        }
        for (int y = 0; y < img.height; y++)
            for (int x = 0; x < img.width; x++) brightest = std::max(brightest, channelOf(img.at(x, y), channel));
    }
    // lit colour: the average of the brightest pixels (the cores of the lit segments)
    long sum[3] = {}, n = 0;
    for (int f = 0; f < sprite.count(); f++) {
        const ImageView img = sprite.frame(f);
        for (int y = 0; y < img.height; y++)
            for (int x = 0; x < img.width; x++) {
                const Color c = img.at(x, y);
                if (channelOf(c, channel) < brightest - 16) continue;
                sum[0] += ch(c, 16), sum[1] += ch(c, 8), sum[2] += ch(c, 0), n++;
            }
    }
    if (n) font.litColor = rgb(int(sum[0] / n), int(sum[1] / n), int(sum[2] / n));
    // background and unlit segments from the blank first frame
    const ImageView blank = sprite.frame(0);
    font.background = blank.at(0, 0);
    long g[3] = {};
    for (size_t s = 0; s < ghosts; s++) {
        const Color c = blank.at(font.segs[s].px0, font.segs[s].py0);
        g[0] += ch(c, 16), g[1] += ch(c, 8), g[2] += ch(c, 0);
    }
    if (ghosts) font.ghostColor = rgb(int(g[0] / long(ghosts)), int(g[1] / long(ghosts)), int(g[2] / long(ghosts)));
    return font;
}

// charGIF (11x18): 14 segments in italics (0.19 pixel per row), the underline (attributes 1
// and 3, and the '.' glyph) and the decimal point (attributes 2 and 3).
SegmentFont makeVfdFont(const Sprite& sprite) {
    constexpr float L = 2.95f, C = 5.8f, R = 8.65f, T = 1.5f, M = 6.5f, B = 11.5f;
    auto seg = [](float u0, float y0, float u1, float y1, int px0, int py0, int px1, int py1) {
        return Seg{u0 + (M - y0) * 0.19f, y0, u1 + (M - y1) * 0.19f, y1, px0, py0, px1, py1};
    };
    std::vector<Seg> s = {
        seg(L + 0.65f, T, C - 0.2f, T, 5, 1, 5, 1),               // a1
        seg(C + 0.2f, T, R - 0.65f, T, 8, 1, 8, 1),               // a2
        seg(R, T + 1, R, M - 1, 9, 3, 9, 4),                      // b
        seg(R, M + 1, R, B - 1, 8, 8, 8, 9),                      // c
        seg(L + 0.65f, B, C - 0.2f, B, 4, 11, 4, 11),             // d1
        seg(C + 0.2f, B, R - 0.65f, B, 6, 11, 6, 11),             // d2
        seg(L, M + 1, L, B - 1, 2, 8, 2, 9),                      // e
        seg(L, T + 1, L, M - 1, 3, 3, 3, 4),                      // f
        seg(L + 0.65f, M, C - 0.7f, M, 4, 6, 4, 6),               // g1
        seg(C + 0.7f, M, R - 0.65f, M, 7, 6, 7, 6),               // g2
        seg(L + 0.9f, T + 1.1f, C - 0.9f, M - 1.1f, 4, 3, 4, 4),  // h
        seg(C, T + 1.1f, C, M - 1.1f, 5, 4, 5, 5),                // i
        seg(R - 0.9f, T + 1.1f, C + 0.9f, M - 1.1f, 7, 3, 7, 4),  // j
        seg(C - 0.9f, M + 1.1f, L + 0.9f, B - 1.1f, 3, 9, 4, 8),  // k
        seg(C, M + 1.1f, C, B - 1.1f, 5, 7, 5, 8),                // l
        seg(C + 0.9f, M + 1.1f, R - 0.9f, B - 1.1f, 6, 8, 6, 9),  // m
        Seg{1.5f, 14.5f, 7.5f, 14.5f, 2, 14, 6, 14},              // underline
        Seg{9.5f, 11.5f, 9.5f, 11.5f, 9, 11, 9, 11, 0.7f},        // decimal point (dimmer, no ghost)
    };
    return analyse(std::move(s), 17, 0.6f, sprite, 1, 145);
}

// numCharGIF (18x23): 7 segments in italics (0.134 pixel per row).
SegmentFont makeLedFont(const Sprite& sprite) {
    constexpr float L = 4.93f, R = 12.9f, X0 = 6.5f, X1 = 11.5f, T = 3.96f, M = 11.08f, B = 18.95f;
    auto seg = [](float u0, float y0, float u1, float y1, int px0, int py0, int px1, int py1) {
        return Seg{u0 + (11.5f - y0) * 0.134f, y0, u1 + (11.5f - y1) * 0.134f, y1, px0, py0, px1, py1};
    };
    std::vector<Seg> s = {
        seg(X0, T, X1, T, 9, 3, 9, 3),                // a
        seg(R, 5.45f, R, 9.55f, 13, 6, 13, 7),        // b
        seg(R, 12.9f, R, 17.5f, 12, 14, 12, 15),      // c
        seg(X0, B, X1, B, 8, 18, 8, 18),              // d
        seg(L, 12.9f, L, 17.5f, 4, 14, 4, 15),        // e
        seg(L, 5.45f, L, 9.55f, 5, 6, 5, 7),          // f
        seg(X0, M, X1, M, 8, 11, 8, 11),              // g
    };
    return analyse(std::move(s), 7, 1.0f, sprite, 0, 150);
}

const SegmentFont& vfdFont(const Sprite& sprite) {
    static const SegmentFont font = makeVfdFont(sprite);
    return font;
}
const SegmentFont& ledDigitFont(const Sprite& sprite) {
    static const SegmentFont font = makeLedFont(sprite);
    return font;
}

// One character: unlit segments, then the lit ones with a glow. (ox, oy) = cell origin in
// window pixels, S = window pixels per sprite pixel.
void drawGlyph(Hd& hd, const SegmentFont& font, int frame, float ox, float oy, float S) {
    const uint32_t lit = frame >= 0 && frame < int(font.lit.size()) ? font.lit[size_t(frame)] : 0;
    auto seg = [&](const Seg& s, float r, float soft, Color c, float a) {
        hd.capsule(ox + s.x0 * S, oy + s.y0 * S, ox + s.x1 * S, oy + s.y1 * S, r * S, soft, c, a);
    };
    for (size_t i = 0; i < font.ghosts; i++)
        if (!(lit & (1u << i))) seg(font.segs[i], font.radius, 1.f, font.ghostColor, 1.f);
    auto colour = [&](const Seg& s) {
        return s.level < 1 ? mix(font.background, font.litColor, s.level) : font.litColor;
    };
    for (size_t i = 0; i < font.segs.size(); i++)
        if (lit & (1u << i)) seg(font.segs[i], font.radius * 1.9f, 2.2f * S, colour(font.segs[i]), 0.28f);
    for (size_t i = 0; i < font.segs.size(); i++)
        if (lit & (1u << i)) seg(font.segs[i], font.radius, 1.f, colour(font.segs[i]), 1.f);
}

// ---------------------------------------------------------------- LEDs, buttons, knobs
// LED (LedGIF 14x14): a dark bezel lit from the lower right around a red lens.
void drawLed(Hd& hd, bool on, float ox, float oy, float S, Color background) {
    hd.fill(hd.clip, background);
    const float cx = ox + 6.6f * S, cy = oy + 6.7f * S;
    hd.disc(cx, cy, 5.7f * S, 1.f, [](float nx, float ny) {
        return mix(rgb(14, 3, 7), rgb(126, 84, 112), clampf((nx + ny) * 0.8f - 0.3f, 0.f, 1.f));
    });
    hd.disc(cx, cy, 4.3f * S, 1.f, [on](float nx, float ny) {
        const float d = std::hypot(nx, ny);
        return on ? mix(rgb(255, 3, 44), rgb(128, 0, 16), clampf((d - 0.55f) / 0.45f, 0.f, 1.f))
                  : mix(rgb(84, 0, 15), rgb(40, 0, 6), clampf((d - 0.3f) / 0.7f, 0.f, 1.f));
    });
    hd.disc(cx - 1.5f * S, cy - 1.5f * S, 0.9f * S, 0.9f * S,  // the reflection
            [on](float, float) { return on ? rgb(255, 196, 206) : rgb(196, 112, 128); }, on ? 0.55f : 0.75f);
}

// GraphButton's hover frame: 1 form pixel around the control (the page arrows).
void drawHoverFrame(Hd& hd, float ox, float oy, int w, int h, float S, Color c) {
    if (c == kNoFrame) return;
    const int X0 = int(std::lround(ox)), Y0 = int(std::lround(oy));
    const int X1 = int(std::lround(ox + w * S)), Y1 = int(std::lround(oy + h * S));
    const int t = std::max(1, int(std::lround(S)));
    hd.fill(Rect{X0, Y0, X1, Y0 + t}, c);
    hd.fill(Rect{X0, Y1 - t, X1, Y1}, c);
    hd.fill(Rect{X0, Y0, X0 + t, Y1}, c);
    hd.fill(Rect{X1 - t, Y0, X1, Y1}, c);
}

// Push buttons (ButtGIF, smallButtGIF, upDownGif), modelled from each frame: the dark 1-pixel
// border, then the body as a colour per row and per column (gradient and bevels, kept sharp)
// plus what remains of the original's pixels (its mottled texture, smoothed). A white arrow
// is redrawn as a triangle.
struct ButtonModel {
    bool ok = false;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // border, inclusive (sprite pixels)
    Color border = 0;
    int bw = 0, bh = 0;  // body: (x0 + 1, y0 + 1), bw x bh
    std::vector<std::array<float, 3>> row, col, rest;
    bool arrow = false, arrowUp = false;
    float ax0 = 0, ay0 = 0, ax1 = 0, ay1 = 0;
};

int luma(Color c) { return (ch(c, 16) * 3 + ch(c, 8) * 6 + ch(c, 0)) / 10; }
bool isWhite(Color c) { return ch(c, 16) > 225 && ch(c, 8) > 225 && ch(c, 0) > 225; }

ButtonModel modelButton(const ImageView& f) {
    ButtonModel m;
    int x0 = f.width, y0 = f.height, x1 = -1, y1 = -1;
    for (int y = 0; y < f.height; y++)
        for (int x = 0; x < f.width; x++)
            if (luma(f.at(x, y)) < 28)
                x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
    if (x1 - x0 < 6 || y1 - y0 < 6) return m;
    m.x0 = x0, m.y0 = y0, m.x1 = x1, m.y1 = y1;
    m.border = f.at((x0 + x1) / 2, y0);
    m.bw = x1 - x0 - 1, m.bh = y1 - y0 - 1;
    auto px = [&](int bx, int by) { return f.at(x0 + 1 + bx, y0 + 1 + by); };
    auto median = [](std::vector<float>& v) {
        if (v.empty()) return 0.f;
        std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
        return v[v.size() / 2];
    };
    const int shifts[3] = {16, 8, 0};
    m.row.assign(size_t(m.bh), {});
    m.col.assign(size_t(m.bw), {});
    m.rest.assign(size_t(m.bw * m.bh), {});
    std::vector<float> v;
    for (int c = 0; c < 3; c++) {
        for (int by = 0; by < m.bh; by++) {
            v.clear();
            for (int bx = 2; bx < m.bw - 2; bx++)
                if (!isWhite(px(bx, by))) v.push_back(float(ch(px(bx, by), shifts[c])));
            m.row[size_t(by)][size_t(c)] = median(v);
        }
        for (int bx = 0; bx < m.bw; bx++) {
            v.clear();
            for (int by = 2; by < m.bh - 2; by++)
                if (!isWhite(px(bx, by))) v.push_back(float(ch(px(bx, by), shifts[c])) - m.row[size_t(by)][size_t(c)]);
            m.col[size_t(bx)][size_t(c)] = median(v);
        }
        for (int by = 0; by < m.bh; by++)
            for (int bx = 0; bx < m.bw; bx++) {
                const float model = m.row[size_t(by)][size_t(c)] + m.col[size_t(bx)][size_t(c)];
                const Color p = px(bx, by);
                m.rest[size_t(by * m.bw + bx)][size_t(c)] = isWhite(p) ? 0.f : ch(p, shifts[c]) - model;
            }
    }
    // the arrow: white pixels, wider at the bottom = pointing up
    int wx0 = f.width, wy0 = f.height, wx1 = -1, wy1 = -1, n = 0;
    for (int y = 0; y < f.height; y++)
        for (int x = 0; x < f.width; x++)
            if (isWhite(f.at(x, y)))
                wx0 = std::min(wx0, x), wy0 = std::min(wy0, y), wx1 = std::max(wx1, x), wy1 = std::max(wy1, y), n++;
    if (n > 6) {
        int top = 0, bottom = 0;
        for (int x = wx0; x <= wx1; x++) top += isWhite(f.at(x, wy0)), bottom += isWhite(f.at(x, wy1));
        m.arrow = true, m.arrowUp = bottom > top;
        m.ax0 = float(wx0), m.ay0 = float(wy0), m.ax1 = float(wx1 + 1), m.ay1 = float(wy1 + 1);
    }
    m.ok = true;
    return m;
}

const ButtonModel& buttonModel(const ImageView& f) {
    static std::mutex lock;
    static std::map<const Color*, ButtonModel> cache;  // the frames are static assets
    std::lock_guard<std::mutex> g(lock);
    auto it = cache.find(f.px);
    if (it == cache.end()) it = cache.emplace(f.px, modelButton(f)).first;
    return it->second;
}

// Sharp sampling of a profile at p (its pixels): the two entries to mix (i0, i1) and the
// weight of i1. 1-pixel features stay crisp at any scale.
float sharpAt(float p, float S, int n, int& i0, int& i1) {
    const int i = int(std::floor(p));
    i0 = clampi(i, 0, n - 1), i1 = clampi(i + 1, 0, n - 1);
    return clampf((p - i - 0.5f) * S + 0.5f, 0.f, 1.f);
}

void drawButton(Hd& hd, const ImageView& f, float ox, float oy, float S, Color hoverFrame) {
    const ButtonModel& m = buttonModel(f);
    if (m.ok) {
        hd.roundRect(ox + m.x0 * S, oy + m.y0 * S, ox + (m.x1 + 1) * S, oy + (m.y1 + 1) * S, 1.1f * S,
                     [&m](float, float) { return m.border; });
        const float bx = ox + (m.x0 + 1) * S, by = oy + (m.y0 + 1) * S;
        const Rect body = Rect{int(std::lround(bx)), int(std::lround(by)), int(std::lround(bx + m.bw * S)),
                               int(std::lround(by + m.bh * S))}
                              .intersect(hd.clip);
        for (int Y = body.top; Y < body.bottom; Y++) {
            const float py = (Y + 0.5f - by) / S - 0.5f;
            int r0, r1;
            const float fy = sharpAt(py, S, m.bh, r0, r1);
            const int t0 = clampi(int(std::floor(py)), 0, m.bh - 1), t1 = clampi(int(std::floor(py)) + 1, 0, m.bh - 1);
            const float ty = clampf(py - std::floor(py), 0.f, 1.f);
            Color* out = hd.b.row(Y);
            for (int X = body.left; X < body.right; X++) {
                const float pxl = (X + 0.5f - bx) / S - 0.5f;
                int c0, c1;
                const float fx = sharpAt(pxl, S, m.bw, c0, c1);
                const int s0 = clampi(int(std::floor(pxl)), 0, m.bw - 1);
                const int s1 = clampi(int(std::floor(pxl)) + 1, 0, m.bw - 1);
                const float tx = clampf(pxl - std::floor(pxl), 0.f, 1.f);
                int rgbv[3];
                for (size_t c = 0; c < 3; c++) {
                    const float rowv = m.row[size_t(r0)][c] + (m.row[size_t(r1)][c] - m.row[size_t(r0)][c]) * fy;
                    const float colv = m.col[size_t(c0)][c] + (m.col[size_t(c1)][c] - m.col[size_t(c0)][c]) * fx;
                    auto rest = [&](int x, int y) { return m.rest[size_t(y * m.bw + x)][c]; };
                    const float restv = (rest(s0, t0) * (1 - tx) + rest(s1, t0) * tx) * (1 - ty) +
                                        (rest(s0, t1) * (1 - tx) + rest(s1, t1) * tx) * ty;
                    rgbv[c] = clampi(int(std::lround(rowv + colv + restv)), 0, 255);
                }
                out[X] = rgb(rgbv[0], rgbv[1], rgbv[2]);
            }
        }
        if (m.arrow) {
            const float ax0 = ox + m.ax0 * S, ax1 = ox + m.ax1 * S, ay0 = oy + m.ay0 * S, ay1 = oy + m.ay1 * S;
            const Color white = rgb(255, 255, 255);
            if (m.arrowUp) hd.triangle((ax0 + ax1) / 2, ay0, ax1, ay1, ax0, ay1, white);
            else hd.triangle(ax0, ay0, ax1, ay0, (ax0 + ax1) / 2, ay1, white);
        }
    }
    drawHoverFrame(hd, ox, oy, f.width, f.height, S, hoverFrame);
}

// The page arrows beside the VFD (scrButtGif 20x18): a green triangle, dim or lit like the
// display's segments, on black.
void drawScrollArrow(Hd& hd, const ImageView& f, float ox, float oy, float S, Color hoverFrame) {
    const Color bg = f.at(0, 0);
    hd.fill(hd.clip, bg);
    int peak = 0;
    Color colour = 0;
    for (int y = 0; y < f.height; y++)
        for (int x = 0; x < f.width; x++)
            if (ch(f.at(x, y), 8) > peak) peak = ch(f.at(x, y), 8), colour = f.at(x, y);
    int x0 = f.width, y0 = f.height, x1 = -1, y1 = -1;
    for (int y = 0; y < f.height; y++)
        for (int x = 0; x < f.width; x++)
            if (ch(f.at(x, y), 8) * 3 > peak)
                x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
    if (x1 >= x0) {
        int top = 0, bottom = 0;
        for (int x = x0; x <= x1; x++) top += ch(f.at(x, y0), 8) * 3 > peak, bottom += ch(f.at(x, y1), 8) * 3 > peak;
        const float X0 = ox + x0 * S, X1 = ox + (x1 + 1) * S, Y0 = oy + y0 * S, Y1 = oy + (y1 + 1) * S;
        if (peak > 128) {  // lit: a glow first
            const float g = 1.2f * S;
            const Color glow = mix(bg, colour, 0.25f);
            if (bottom > top) hd.triangle((X0 + X1) / 2, Y0 - g, X1 + g, Y1 + g, X0 - g, Y1 + g, glow);
            else hd.triangle(X0 - g, Y0 - g, X1 + g, Y0 - g, (X0 + X1) / 2, Y1 + g, glow);
        }
        if (bottom > top) hd.triangle((X0 + X1) / 2, Y0, X1, Y1, X0, Y1, colour);
        else hd.triangle(X0, Y0, X1, Y0, (X0 + X1) / 2, Y1, colour);
    }
    drawHoverFrame(hd, ox, oy, f.width, f.height, S, hoverFrame);
}

// Knob (KnobGif, 128 frames 40x40). Measured from the original: the pointer turns about
// (20.17, 22.10), the body has a radius of 11.6, nine ticks every 33.75 degrees. The rest is
// read from the frames: the knob without its pointer (each pixel's median over the frames)
// gives the body's shading and the shadow, smoothly enlarged; the pointer's angle in each
// frame (not linear in the frame number) is its pixels' mean direction.
struct KnobModel {
    static constexpr float kCx = 20.17f, kCy = 22.10f, kBody = 11.6f;
    int w = 0, h = 0;
    std::vector<std::array<float, 3>> body, around;  // without the pointer; around: without the ticks
    std::vector<float> angle;                        // per frame, radians (0 = up, clockwise)
    Color tick = 0, pointer = 0;
};

KnobModel makeKnobModel(const Sprite& sprite) {
    KnobModel m;
    m.w = sprite.frameWidth(), m.h = sprite.frameHeight();
    const size_t n = size_t(m.w * m.h);
    m.body.assign(n, {});
    std::vector<int> v(size_t(sprite.count()));
    const int shifts[3] = {16, 8, 0};
    for (int y = 0; y < m.h; y++)
        for (int x = 0; x < m.w; x++)
            for (size_t c = 0; c < 3; c++) {
                for (int f = 0; f < sprite.count(); f++) v[size_t(f)] = ch(sprite.frame(f).at(x, y), shifts[c]);
                std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
                m.body[size_t(y * m.w + x)][c] = float(v[v.size() / 2]);
            }
    const Color bg = sprite.frame(0).at(0, 0);
    auto lumaOf = [](const std::array<float, 3>& p) { return (p[0] * 3 + p[1] * 6 + p[2]) / 10; };
    const float bgLuma = float(luma(bg));
    m.around = m.body;
    long t[3] = {}, tn = 0;
    for (int y = 0; y < m.h; y++)
        for (int x = 0; x < m.w; x++) {
            auto& p = m.around[size_t(y * m.w + x)];
            const float r = std::hypot(x + 0.5f - KnobModel::kCx, y + 0.5f - KnobModel::kCy);
            if (r > KnobModel::kBody + 0.5f && lumaOf(p) > bgLuma + 10) {
                if (lumaOf(p) > bgLuma + 40) t[0] += long(p[0]), t[1] += long(p[1]), t[2] += long(p[2]), tn++;
                p = {float(ch(bg, 16)), float(ch(bg, 8)), float(ch(bg, 0))};
            }
        }
    m.tick = tn ? rgb(int(t[0] / tn), int(t[1] / tn), int(t[2] / tn)) : rgb(110, 110, 118);
    m.angle.assign(size_t(sprite.count()), 0.f);
    long p[3] = {}, pn = 0;
    for (int f = 0; f < sprite.count(); f++) {
        const ImageView img = sprite.frame(f);
        double sx = 0, sy = 0;
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) {
                const float dx = x + 0.5f - KnobModel::kCx, dy = y + 0.5f - KnobModel::kCy, r = std::hypot(dx, dy);
                const int l = luma(img.at(x, y));
                if (r < 3 || r > 11.5f || l <= 120) continue;
                const double a = std::atan2(dx, -dy);
                sx += (l - 120) * std::sin(a), sy += (l - 120) * std::cos(a);
                const Color c = img.at(x, y);
                if (l > 200) p[0] += ch(c, 16), p[1] += ch(c, 8), p[2] += ch(c, 0), pn++;
            }
        m.angle[size_t(f)] = float(std::atan2(sx, sy));
    }
    m.pointer = pn ? rgb(int(p[0] / pn), int(p[1] / pn), int(p[2] / pn)) : rgb(235, 235, 240);
    return m;
}

const KnobModel& knobModel(const Sprite& sprite) {
    static const KnobModel m = makeKnobModel(sprite);
    return m;
}

Color bilinear(const std::vector<std::array<float, 3>>& img, int w, int h, float x, float y) {
    x -= 0.5f, y -= 0.5f;
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const float tx = x - x0, ty = y - y0;
    auto at = [&](int xx, int yy) { return img[size_t(clampi(yy, 0, h - 1) * w + clampi(xx, 0, w - 1))]; };
    const auto &a = at(x0, y0), &b = at(x0 + 1, y0), &c = at(x0, y0 + 1), &d = at(x0 + 1, y0 + 1);
    int out[3];
    for (size_t i = 0; i < 3; i++) {
        const float v = (a[i] * (1 - tx) + b[i] * tx) * (1 - ty) + (c[i] * (1 - tx) + d[i] * tx) * ty;
        out[i] = clampi(int(std::lround(v)), 0, 255);
    }
    return rgb(out[0], out[1], out[2]);
}

void drawKnob(Hd& hd, const KnobModel& m, int frame, float ox, float oy, float S) {
    const Rect area =
        Rect{int(std::lround(ox)), int(std::lround(oy)), int(std::lround(ox + m.w * S)), int(std::lround(oy + m.h * S))}
            .intersect(hd.clip);
    const float R = KnobModel::kBody, inner = R - 1.2f;
    for (int Y = area.top; Y < area.bottom; Y++) {
        Color* out = hd.b.row(Y);
        for (int X = area.left; X < area.right; X++) {
            const float sx = (X + 0.5f - ox) / S, sy = (Y + 0.5f - oy) / S;
            const float dx = sx - KnobModel::kCx, dy = sy - KnobModel::kCy, r = std::hypot(dx, dy);
            const float a = clampf((R - r) * S + 0.5f, 0.f, 1.f);  // body coverage
            Color c = a < 1 ? bilinear(m.around, m.w, m.h, sx, sy) : 0;
            if (a > 0) {  // body shading, sampled inside its anti-aliased edge
                const float k = r > inner ? inner / r : 1.f;
                const Color body = bilinear(m.body, m.w, m.h, KnobModel::kCx + dx * k, KnobModel::kCy + dy * k);
                c = a >= 1 ? body : mix(c, body, a);
            }
            out[X] = c;
        }
    }
    const float cx = ox + KnobModel::kCx * S, cy = oy + KnobModel::kCy * S;
    for (int i = 0; i < 9; i++) {
        const float t = (-135.f + 33.75f * i) * kPi / 180.f, sx = std::sin(t), sy = -std::cos(t);
        hd.capsule(cx + sx * 15.9f * S, cy + sy * 15.9f * S, cx + sx * 17.8f * S, cy + sy * 17.8f * S, 0.62f * S, 1.f,
                   m.tick);
    }
    const float t = frame >= 0 && frame < int(m.angle.size()) ? m.angle[size_t(frame)] : 0.f;
    const float sx = std::sin(t), sy = -std::cos(t);
    hd.capsule(cx + sx * 0.8f * S, cy + sy * 0.8f * S, cx + sx * 11.9f * S, cy + sy * 11.9f * S, 0.75f * S, 1.f,
               m.pointer);
}

// ---------------------------------------------------------------- the editor
// A control's rectangle in form coordinates (left/top are relative to the parent window).
Rect absRect(const Control& c, int x, int y, int w, int h) {
    int ax = c.left(), ay = c.top();
    for (const Control* p = c.parent; p; p = p->parent) ax += p->left(), ay += p->top();
    return boundsRect(ax + x, ay + y, w, h);
}

Rect toWindow(const Rect& r, double S) {
    return Rect{int(std::floor(r.left * S)), int(std::floor(r.top * S)), int(std::ceil(r.right * S)),
                int(std::ceil(r.bottom * S))};
}

bool spriteIs(const Sprite* s, const char* name) { return s && s->loaded() && s->name() == name; }

}  // namespace

uint32_t HdRenderer::vfdSegments(int frame) {
    const SegmentFont& f = vfdFont(assets::charGif());
    return frame >= 0 && frame < int(f.lit.size()) ? f.lit[size_t(frame)] : 0;
}

uint32_t HdRenderer::ledSegments(int frame) {
    const SegmentFont& f = ledDigitFont(assets::numCharGif());
    return frame >= 0 && frame < int(f.lit.size()) ? f.lit[size_t(frame)] : 0;
}

double HdRenderer::knobAngle(int frame) {
    const KnobModel& m = knobModel(assets::knobGif());
    return frame >= 0 && frame < int(m.angle.size()) ? m.angle[size_t(frame)] * 180.0 / kPi : 0.0;
}

Rect HdRenderer::changedArea(const Bitmap& before, const Bitmap& after) {
    if (before.width() != after.width() || before.height() != after.height())
        return Rect{0, 0, after.width(), after.height()};
    Rect r{after.width(), after.height(), 0, 0};
    for (int y = 0; y < after.height(); y++) {
        const Color *a = before.row(y), *b = after.row(y);
        int x0 = 0, x1 = after.width();
        while (x0 < x1 && a[x0] == b[x0]) x0++;
        if (x0 == x1) continue;
        while (a[x1 - 1] == b[x1 - 1]) x1--;
        r = Rect{std::min(r.left, x0), std::min(r.top, y), std::max(r.right, x1), y + 1};
    }
    return r.empty() ? Rect{} : r;
}

void HdRenderer::capture(const EditorView& v) {
    EditorView& view = const_cast<EditorView&>(v);  // (its accessors are not const; nothing is changed)
    items_.clear();
    auto add = [this](const Rect& r, std::function<void(Hd&, float)> draw) {
        items_.push_back({r, [draw](Bitmap& out, const Rect& clip, float S) {
                              Hd hd{out, clip};
                              draw(hd, S);
                          }});
    };
    // Everything a control shows is copied here: the drawing runs later, without the editor.
    for (int i = 0; i < 10; i++) {
        const Knob& k = view.knob(i);
        if (!k.visible() || !spriteIs(k.aniGif(), "KnobGif")) continue;
        const KnobModel* m = &knobModel(*k.aniGif());
        const Rect r = absRect(k, k.frameX(), k.frameY(), m->w, m->h);
        const int frame = k.frameIndex();
        add(r, [=](Hd& hd, float S) { drawKnob(hd, *m, frame, r.left * S, r.top * S, S); });
    }
    for (const AniDisplay* led : {&view.ledSync(), &view.ledAm(), &view.ledMono()}) {
        if (!led->visible() || !spriteIs(led->aniGif(), "LedGIF")) continue;
        const Rect r = absRect(*led, 0, 0, 14, 14);
        const bool on = led->frameIndex() == 1;
        const Color bg = led->aniGif()->frame(0).at(0, 0);
        add(r, [=](Hd& hd, float S) { drawLed(hd, on, r.left * S, r.top * S, S, bg); });
    }
    for (const GraphButton* b : view.buttons()) {
        const Sprite* g = b->aniGif();
        if (!b->visible() || !(spriteIs(g, "ButtGIF") || spriteIs(g, "smallButtGIF") || spriteIs(g, "upDownGif") ||
                               spriteIs(g, "scrButtGif")))
            continue;
        const ImageView f = g->frame(b->frameIndex());
        if (!f) continue;
        const int off = b->pressed() > 0 ? b->pressDisplacement() : 0;
        const Rect r = absRect(*b, b->imageX() + off, b->imageY() + off, f.width, f.height);
        const Color frame = b->frameWidth() > 0 && b->hover() ? b->frameColor() : kNoFrame;
        if (g->name() == "scrButtGif")
            add(r, [=](Hd& hd, float S) { drawScrollArrow(hd, f, r.left * S, r.top * S, S, frame); });
        else
            add(r, [=](Hd& hd, float S) { drawButton(hd, f, r.left * S, r.top * S, S, frame); });
    }
    // the displays, one item per character cell
    for (const LcdDisplay* lcd : {&view.lcd(), &view.numLcd()}) {
        const Sprite* g = lcd->aniGif();
        if (!lcd->visible() || !(spriteIs(g, "charGIF") || spriteIs(g, "numCharGIF"))) continue;
        const SegmentFont* font = g->name() == "charGIF" ? &vfdFont(*g) : &ledDigitFont(*g);
        for (int row = 0; row < lcd->rows(); row++)
            for (int col = 0; col < lcd->cols(); col++) {
                const LcdDisplay::Cell& c = lcd->cell(col, row);
                const int attr = c.attr >= lcd->numAttr() ? 0 : c.attr;
                const int frame = attr + lcd->charMap()[c.ch] * lcd->numAttr();
                const Rect r = absRect(*lcd, lcd->charX(col), lcd->charY(row), lcd->charWidth(), lcd->charHeight());
                add(r, [=](Hd& hd, float S) {
                                      hd.fill(hd.clip, font->background);
                                      drawGlyph(hd, *font, frame, r.left * S, r.top * S, S);
                                  });
            }
    }
    // texts: the status bar and voices panels with their labels, the program name (the font
    // at the window's size; the panel is the clip, as the text is not exactly S times wider)
    auto scaled = [](const Font& font, float S) {
        Font f = font;
        f.height = int(std::lround(font.height * S));
        return f;
    };
    struct Text {
        Rect r;
        std::string caption;
        Font font;
        Color color;
        bool fill;
        Label::Alignment alignment;
    };
    for (const Panel* p : {&view.panel1(), &view.statusPanel2()}) {
        if (!p->visible()) continue;
        const Rect r = absRect(*p, 0, 0, p->width(), p->height());
        std::vector<Text> labels;
        for (const Label* l : {&view.statusLabel1(), &view.statusLabel2()})
            if (l->parent == p && l->visible())
                labels.push_back({absRect(*l, 0, 0, l->width(), l->height()), l->caption(), l->font, l->color,
                                  !l->transparent, l->alignment});
        const Color color = p->color;
        add(r, [=](Hd& hd, float S) {
                              hd.fill(toWindow(r, S), color);
                              const Rect clip = toWindow(r, S).intersect(hd.clip);
                              for (const Text& l : labels) {
                                  if (l.fill) hd.fill(toWindow(l.r, S), l.color);
                                  const Font f = scaled(l.font, S);
                                  int x = int(std::lround(l.r.left * S));
                                  if (l.alignment == Label::RightJustify)
                                      x = int(std::lround(l.r.right * S)) - text_.textWidth(f, l.caption);
                                  else if (l.alignment == Label::Center)
                                      x = int(std::lround((l.r.left + l.r.right) * S / 2)) -
                                          text_.textWidth(f, l.caption) / 2;
                                  text_.drawText(hd.b, x, int(std::lround(l.r.top * S)), clip, f, l.caption);
                              }
                          });
    }
    const NameEdit& e = view.progNameEdit();
    if (e.visible()) {
        const Rect r = absRect(e, 0, 0, e.width(), e.height());
        const std::string text = e.text;
        const Font font = e.font;
        const Color color = e.color;
        add(r, [=](Hd& hd, float S) {
                              hd.fill(toWindow(r, S), color);
                              text_.drawText(hd.b, int(std::lround((r.left + 1) * S)), int(std::lround(r.top * S)),
                                             toWindow(r, S).intersect(hd.clip), scaled(font, S), text);
                          });
    }
}

Rect HdRenderer::render(const Bitmap& classic, double scale, Bitmap& out, const Rect& dirty) const {
    const float S = float(scale);
    const int W = int(std::lround(EditorView::kWidth * scale)), H = int(std::lround(EditorView::kHeight * scale));
    if (out.width() != W || out.height() != H) out.resize(W, H, 0);
    const Rect full{0, 0, W, H};
    // What to redraw: the dirty rectangle and, whole, every redrawn control that overlaps
    // what is redrawn (repeated, as a control's rectangle may overlap a neighbour's by a pixel).
    std::vector<Rect> blits{toWindow(dirty, S).intersect(full)};
    if (blits[0].empty()) return Rect{};
    std::vector<bool> todo(items_.size(), false);
    for (bool grew = true; grew;) {
        grew = false;
        for (size_t i = 0; i < items_.size(); i++) {
            if (todo[i]) continue;
            const Rect w = toWindow(items_[i].r, S).intersect(full);
            for (const Rect& b : blits)
                if (!w.intersect(b).empty()) {
                    todo[i] = grew = true;
                    blits.push_back(w);
                    break;
                }
        }
    }
    Rect updated = blits[0];
    for (const Rect& b : blits) {
        sharpBlit(classic, S, out, b);
        updated = Rect{std::min(updated.left, b.left), std::min(updated.top, b.top), std::max(updated.right, b.right),
                       std::max(updated.bottom, b.bottom)};
    }
    for (size_t i = 0; i < items_.size(); i++)
        if (todo[i]) {
            items_[i].draw(out, toWindow(items_[i].r, S).intersect(full), S);
        }
    return updated;
}

}  // namespace sq8l::gui
