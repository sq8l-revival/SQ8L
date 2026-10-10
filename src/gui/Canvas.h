// A TCanvas-like drawing context on a Bitmap: GDI semantics for the few primitives the
// original controls use (FillRect, FrameRect, MoveTo/LineTo with a 1-pixel pen, Draw of an
// opaque bitmap = SRCCOPY blit, TextOut through a TextRenderer).
#pragma once

#include <string>

#include "Bitmap.h"
#include "TextRenderer.h"

namespace sq8l::gui {

class Canvas {
public:
    // Drawing coordinates are relative to (originX, originY) in `target`; everything is
    // clipped to the target and to the clip rectangle (canvas coordinates).
    explicit Canvas(Bitmap& target, int originX = 0, int originY = 0, TextRenderer* text = nullptr);

    void setClip(const Rect& r);  // canvas coordinates
    Bitmap& target() { return t_; }

    Color penColor = 0;           // Pen.Color (1-pixel solid pen, R2_COPYPEN)
    Color brushColor = 0xFFFFFF;  // Brush.Color (solid brush)
    Font font;

    // GDI MoveToEx / LineTo: the end point is not drawn.
    void moveTo(int x, int y);
    void lineTo(int x, int y);
    void fillRect(const Rect& r);   // Windows FillRect with the brush
    void frameRect(const Rect& r);  // Windows FrameRect with the brush (1 pixel)
    // TCanvas.Draw(x, y, Graphic) of a non-transparent TBitmap: plain copy (StretchBlt
    // SRCCOPY at 1:1). A null view draws nothing.
    void draw(int x, int y, const ImageView& img);
    // (port) No TCanvas equivalent: `draw` with every pixel faded towards `toward` by
    // `amount` (0 = the image, 1 = the flat colour). Used for the knobs the display page
    // does not use, which are drawn half sunk into their backdrop.
    void drawFaded(int x, int y, const ImageView& img, Color toward, float amount);
    // TCanvas.TextOut with a transparent brush (bsClear). No-op without a TextRenderer.
    void textOut(int x, int y, const std::string& text);
    int textWidth(const std::string& text);
    int textHeight();

private:
    void plot(int x, int y, Color c);
    Rect effectiveClip() const;
    Bitmap& t_;
    int ox_, oy_;
    Rect clip_;  // target coordinates
    int penX_ = 0, penY_ = 0;
    TextRenderer* text_;
};

}  // namespace sq8l::gui
