// HD graphics: the editor drawn at the window's resolution instead of its 626x430 pixels
// enlarged. Always on; the classic frame is shown only where this cannot be (a window
// larger than the largest texture, and the drawn menus and dialogs).
//
// The panel (the original's background picture) is drawn as vector shapes (HdPanel), the
// controls over it from their state: the knobs, the LEDs, the buttons, the VFD (16-segment
// glyphs), the red LED digits (7 segments) and the texts with the font at the window's
// size. What the original's bitmaps show is read from them (which segments a character
// lights, the knob's angle per frame, the shading of knobs and buttons), so the controls
// keep their look.
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
    // to the window size). Only the parts of the window covering `dirty` (form coordinates)
    // are redrawn, the rest of `out` is kept. Returns the parts of `out` that were redrawn
    // (window pixels), for the caller to upload; they may overlap.
    std::vector<Rect> render(double scale, Bitmap& out, const std::vector<Rect>& dirty) const;

    // What changed between two classic frames (EditorView::render), i.e. what to redraw: the
    // tiles of a coarse grid holding a pixel that differs, merged into runs (empty if none;
    // everything if the sizes differ). Tiles rather than one bounding rectangle because that
    // rectangle spans the panel as soon as two small things far apart change, which the
    // commonest gesture does: turning a knob also rewrites its cell in the display.
    static std::vector<Rect> changedTiles(const Bitmap& before, const Bitmap& after);
    static constexpr int kTile = 32;  // form pixels

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
