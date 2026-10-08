// Drawing at window resolution for the HD graphics (internal to src/gui/hd): anti-aliased
// shapes over a bitmap, clipped.
#pragma once

#include <algorithm>
#include <cmath>
#include <functional>

#include "Bitmap.h"

namespace sq8l::gui::hd {

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
inline int ch(Color c, int shift) { return int((c >> shift) & 0xff); }
inline Color mix(Color a, Color b, float t) {  // a..b
    return rgb(int(ch(a, 16) + (ch(b, 16) - ch(a, 16)) * t + 0.5f), int(ch(a, 8) + (ch(b, 8) - ch(a, 8)) * t + 0.5f),
               int(ch(a, 0) + (ch(b, 0) - ch(a, 0)) * t + 0.5f));
}

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
    // A rectangle with fractional edges (window pixels), anti-aliased; colour per pixel.
    void rect(float x0, float y0, float x1, float y1, const std::function<Color(int, int)>& colour, float alpha = 1.f) {
        const Rect k = Rect{int(std::floor(x0)), int(std::floor(y0)), int(std::ceil(x1)), int(std::ceil(y1))}.intersect(clip);
        for (int y = k.top; y < k.bottom; y++) {
            const float cy = std::min(y + 1.f, y1) - std::max(float(y), y0);
            for (int x = k.left; x < k.right; x++) {
                const float cx = std::min(x + 1.f, x1) - std::max(float(x), x0);
                blend(x, y, colour(x, y), alpha * cx * cy);
            }
        }
    }
    void rect(float x0, float y0, float x1, float y1, Color c, float alpha = 1.f) {
        rect(x0, y0, x1, y1, [c](int, int) { return c; }, alpha);
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

}  // namespace sq8l::gui::hd
