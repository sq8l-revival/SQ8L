#include "Canvas.h"

#include <cstdlib>

namespace sq8l::gui {

namespace {
constexpr int kHuge = 1 << 28;
}

Canvas::Canvas(Bitmap& target, int originX, int originY, TextRenderer* text)
    : t_(target), ox_(originX), oy_(originY), clip_{-kHuge, -kHuge, kHuge, kHuge}, text_(text) {}

void Canvas::setClip(const Rect& r) { clip_ = r.offset(ox_, oy_); }

// The target may be resized while painting (a control resizing itself in Paint): clip to
// its current size at each operation.
Rect Canvas::effectiveClip() const { return clip_.intersect(Rect{0, 0, t_.width(), t_.height()}); }

void Canvas::plot(int x, int y, Color c) {
    x += ox_;
    y += oy_;
    if (effectiveClip().contains(x, y)) t_.row(y)[x] = c;
}

void Canvas::moveTo(int x, int y) {
    penX_ = x;
    penY_ = y;
}

void Canvas::lineTo(int x1, int y1) {
    // Bresenham, end point excluded (same stepping as the GUI oracle's GDI LineTo).
    int x = penX_, y = penY_;
    int dx = std::abs(x1 - x), dy = -std::abs(y1 - y);
    int sx = x < x1 ? 1 : -1, sy = y < y1 ? 1 : -1;
    int err = dx + dy;
    while (x != x1 || y != y1) {
        plot(x, y, penColor);
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y += sy;
        }
    }
    penX_ = x1;
    penY_ = y1;
}

void Canvas::fillRect(const Rect& r) {
    Rect d = r.offset(ox_, oy_).intersect(effectiveClip());
    if (d.empty()) return;
    for (int y = d.top; y < d.bottom; y++) std::fill(t_.row(y) + d.left, t_.row(y) + d.right, brushColor);
}

void Canvas::frameRect(const Rect& r) {
    fillRect(Rect{r.left, r.top, r.right, r.top + 1});
    fillRect(Rect{r.left, r.bottom - 1, r.right, r.bottom});
    fillRect(Rect{r.left, r.top, r.left + 1, r.bottom});
    fillRect(Rect{r.right - 1, r.top, r.right, r.bottom});
}

void Canvas::draw(int x, int y, const ImageView& img) {
    if (!img) return;
    Rect d = boundsRect(x + ox_, y + oy_, img.width, img.height).intersect(effectiveClip());
    if (d.empty()) return;
    int sx = d.left - (x + ox_), sy = d.top - (y + oy_);
    for (int yy = 0; yy < d.height(); yy++) {
        const Color* s = img.px + (sy + yy) * img.stride + sx;
        std::copy(s, s + d.width(), t_.row(d.top + yy) + d.left);
    }
}

void Canvas::drawFaded(int x, int y, const ImageView& img, Color toward, float amount) {
    if (!img) return;
    Rect d = boundsRect(x + ox_, y + oy_, img.width, img.height).intersect(effectiveClip());
    if (d.empty()) return;
    const int sx = d.left - (x + ox_), sy = d.top - (y + oy_);
    const int t = static_cast<int>(amount * 256.0f + 0.5f);
    const int tr = int((toward >> 16) & 0xFF), tg = int((toward >> 8) & 0xFF), tb = int(toward & 0xFF);
    for (int yy = 0; yy < d.height(); yy++) {
        const Color* s = img.px + (sy + yy) * img.stride + sx;
        Color* o = t_.row(d.top + yy) + d.left;
        for (int xx = 0; xx < d.width(); xx++) {
            const Color c = s[xx];
            const int r = int((c >> 16) & 0xFF), g = int((c >> 8) & 0xFF), b = int(c & 0xFF);
            o[xx] = rgb(r + (tr - r) * t / 256, g + (tg - g) * t / 256, b + (tb - b) * t / 256);
        }
    }
}

void Canvas::textOut(int x, int y, const std::string& text) {
    if (text_ && !text.empty()) text_->drawText(t_, x + ox_, y + oy_, effectiveClip(), font, text);
}

int Canvas::textWidth(const std::string& text) { return text_ ? text_->textWidth(font, text) : 0; }

int Canvas::textHeight() { return text_ ? text_->textHeight(font) : 0; }

}  // namespace sq8l::gui
