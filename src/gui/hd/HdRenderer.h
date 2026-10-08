// HD graphics (OPTIONS -> HD graphics, a port addition): the editor drawn at the window's
// resolution instead of its 626x430 pixels enlarged.
//
// The editor still renders its classic frame; that frame, enlarged with sharp scaling, is the
// base (background, panels and everything not redrawn yet). On top of it the controls whose
// look is geometric are redrawn from their state with anti-aliased vector shapes: the knobs,
// the LEDs, the buttons, the VFD (16-segment glyphs) and the red LED digits (7 segments),
// and the texts with the font at the window's size. Which segments a glyph lights is read
// from the original's own bitmaps, so every character keeps its shape.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "Bitmap.h"
#include "TextRenderer.h"

namespace sq8l::gui {

class EditorView;

class HdRenderer {
public:
    explicit HdRenderer(TextRenderer& text) : text_(text) {}

    // Note what the controls show (call with the editor's state stable, e.g. under the lock
    // that guards it; this only copies values).
    void capture(const EditorView& view);

    // Draw the editor as captured at `scale` window pixels per form pixel into `out` (resized
    // to the window size). `classic` is the editor's 626x430 frame; only the part of the window
    // covering `dirty` (form coordinates) is redrawn, the rest of `out` is kept. Returns the
    // part of `out` that was redrawn (window pixels).
    Rect render(const Bitmap& classic, double scale, Bitmap& out, const Rect& dirty) const;

    // The bounding rectangle of the pixels that differ (empty if none; everything if the sizes
    // differ): what changed between two classic frames.
    static Rect changedArea(const Bitmap& before, const Bitmap& after);

    // What is read from the original's bitmaps (tests): the segments a frame lights (VFD,
    // charGIF: bits 0-15 = a1 a2 b c d1 d2 e f g1 g2 h i j k l m, 16 = underline, 17 = decimal
    // point; red digits, numCharGIF: bits 0-6 = a b c d e f g) and the angle of a knob frame's
    // pointer in degrees (0 = up, clockwise).
    static uint32_t vfdSegments(int frame);
    static uint32_t ledSegments(int frame);
    static double knobAngle(int frame);

private:
    struct Item {
        Rect r;  // form coordinates
        std::function<void(Bitmap& out, const Rect& clip, float scale)> draw;
    };
    TextRenderer& text_;
    std::vector<Item> items_;
};

}  // namespace sq8l::gui
