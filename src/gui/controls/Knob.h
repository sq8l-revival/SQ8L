// Port of TGraphKnobB (unit graphKnobB, 0x4796ec-0x47b3e4): a sprite knob dragged
// vertically, with finer resolution when the mouse moves horizontally away from the
// click point or with Shift held.
//
// Floating point follows the x87 code at 53-bit precision (the GUI thread's host control
// word, round to nearest): values stored as Delphi Single are `float`, intermediates on the
// FPU stack are `double`.
//
// Not ported (unused by SQ8L: ShowCaption = ShowValue = False, an AniGIF is always set):
// the caption/value text and the value TEdit, the TImageList renderer and the vector
// fallback drawing (ellipse + pointer).
#pragma once

#include <functional>

#include "../Sprite.h"
#include "Control.h"

namespace sq8l::gui {

class Knob : public Control {
public:
    explicit Knob(std::string name);  // TGraphKnobB.Create defaults (0x479d08)

    // ------------------------------------------------------------ value model
    float value() const { return value_; }     // +0x22c
    float minValue() const { return min_; }    // +0x230
    float maxValue() const { return max_; }    // +0x234
    float range() const { return range_; }     // +0x238
    float valueStep() const { return step_; }  // +0x23c
    void setValue(float v);                    // FUN_0047aa8c (quantize to step, clamp, OnChange not fired)
    void setMinValue(float v);                 // FUN_0047abd0
    void setMaxValue(float v);                 // FUN_0047acd0
    void setValueStep(float v);                // FUN_0047add0
    void setMaxPixDist(int pixels);            // FUN_0047aa44: drag pixels for the full range
    void setMaxFinePixDist(int pixels);        // 0x47aa30 (< 0 -> 0)
    void setMinFineFac(float f) { minFineFac_ = f; }
    void setSnapToZero(bool s) { snapToZero_ = s; }
    void setRadius(int r);                     // FUN_0047af34 (layout)
    void setAniGif(const Sprite* gif);         // FUN_0047b158
    void setDoRestoreMousePos(bool r) { restoreMouse_ = r; }
    // (port) A knob the display page has no parameter for turns nothing: it is drawn faded
    // into its backdrop so that it reads as doing nothing (issue #24). The original drew
    // every knob the same and left the user to find out by dragging.
    void setActive(bool a);
    // FUN_0047b1c0 / FUN_0047b1d4: caption above / value box below the knob (geometry only:
    // the text itself is not ported, SQ8L sets both to False).
    void setShowCaption(bool s) {
        showCaption_ = s;
        layout(radius_);
    }
    void setShowValue(bool s) {
        showValue_ = s;
        layout(radius_);
    }

    int maxPixDist() const { return maxPixDist_; }          // +0x218
    int maxFinePixDist() const { return maxFinePixDist_; }  // +0x21c
    float minFineFac() const { return minFineFac_; }        // +0x1fc
    float pixFactor() const { return pixFactor_; }          // +0x244: value units per pixel
    bool snapToZero() const { return snapToZero_; }         // +0x248
    float snapZone() const { return snapZone_; }            // +0x24c = 0.04 * range
    bool intMode() const { return intMode_; }               // +0x240
    float angle() const { return angle_; }                  // +0x278 (radians, -0.8pi..0.8pi)
    int radius() const { return radius_; }                  // +0x250
    int frameX() const { return frameX_; }                  // +0x254
    int frameY() const { return frameY_; }                  // +0x258
    const Sprite* aniGif() const { return gif_; }
    bool active() const { return active_; }
    // How far a faded (inactive) knob is pulled towards its backdrop, 0.5 = half opaque.
    static constexpr float kFadeAmount = 0.5f;
    // The sprite frame Paint would show for the current value.
    int frameIndex() const;

    // ------------------------------------------------------------ drag (also driven by the
    // editor for drags that start on the LCD, see lcdControl)
    void beginDrag(ShiftState shift, int x, int y);  // FUN_0047a1d0
    void dragMove(ShiftState shift, int x, int y);   // FUN_0047a27c
    void endDrag();                                  // FUN_0047a248 (restores the mouse position)
    void cancelDrag();                               // FUN_0047a268
    bool dragging() const { return dragging_; }      // +0x200 (mouse pressed on the knob)
    bool dragActive() const { return dragActive_; }  // +0x201
    float dragAccumulator() const { return acc_; }   // +0x204

    // ------------------------------------------------------------ events
    std::function<void(Knob&)> onChange;  // +0x2e0 (fired by drags only)
    // +0x2e8: replaces reading Mouse.CursorPos at drag start; +0x2f0: replaces restoring it at
    // the end (SQ8L's form installs both: mouseJump). Without them, the host's cursor
    // position callbacks below are used.
    std::function<void(Knob&)> onGetMousePos, onRestoreMousePos;
    std::function<void(int& x, int& y)> getCursorPos;       // Mouse.CursorPos (screen)
    std::function<void(int x, int y)> setCursorPos;         // Mouse.CursorPos := ...
    int savedMouseX = 0, savedMouseY = 0;                    // +0x220 / +0x224

    void paint(Canvas& canvas) override;                                          // 0x47a554
    void mouseDown(MouseButton button, ShiftState shift, int x, int y) override;  // 0x47a3cc
    void mouseMove(ShiftState shift, int x, int y) override;                      // 0x47a498
    void mouseUp(MouseButton button, ShiftState shift, int x, int y) override;    // 0x47a444
    void dblClick() override;                                                     // 0x47a4f8

    // Raw state access for tests (field-by-field mirror of the original object).
    struct State {
        float value, min, max, range, step, pixFactor, snapZone, minFineFac, acc, angle, angleK;
        int maxPixDist, maxFinePixDist, radius, frameX, frameY, downX, downY, lastX, lastY;
        bool snapToZero, intMode, dragging, dragActive, restoreMouse, loaded, force, textValid;
    };
    State state() const;
    void setState(const State& s);

private:
    void recalcRange();       // shared tail of setMin/setMax
    void layout(int radius);  // FUN_0047af34

    bool loaded_ = false;          // +0x1f8 (set by the first Paint)
    bool force_ = false;           // +0x1f9
    float minFineFac_ = 0.2f;      // +0x1fc
    bool dragging_ = false;        // +0x200
    bool dragActive_ = false;      // +0x201
    float acc_ = 0;                // +0x204
    int downX_ = 0, downY_ = 0;    // +0x208 / +0x20c
    int lastX_ = 0, lastY_ = 0;    // +0x210 / +0x214
    int maxPixDist_ = 256;         // +0x218
    int maxFinePixDist_ = 200;     // +0x21c
    bool restoreMouse_ = false;    // +0x228
    float value_ = 0;              // +0x22c
    float min_ = 0;                // +0x230
    float max_ = 127;              // +0x234
    float range_ = 128;            // +0x238
    float step_ = 0;               // +0x23c
    bool intMode_ = false;         // +0x240
    float pixFactor_ = 0;          // +0x244
    bool snapToZero_ = true;       // +0x248
    float snapZone_ = 0;           // +0x24c
    int radius_ = 16;              // +0x250
    int frameX_ = 0, frameY_ = 0;  // +0x254 / +0x258
    int centerX_ = 0, centerY_ = 0;  // +0x264 / +0x268
    float angleMin_, angleMax_, angleRange_, angle_ = 0, angleK_;  // +0x26c..+0x27c
    bool labelClick_ = false;      // +0x280
    int numDecimals_ = 3;          // +0x298
    bool textValid_ = false;       // +0x2b4
    bool showCaption_ = false;     // +0x2b5 (only the geometry is ported)
    bool showValue_ = false;       // +0x2b6
    // +0x29c / +0x2a0: TextHeight('0') + 1 of FontCaption / FontValue (MS Sans Serif 8: 13 + 1
    // on Windows); only used when the caption / value box is shown.
    int captionHeight_ = 14, valueHeight_ = 14;
    int valueTop_ = 0;             // +0x2a8
    const Sprite* gif_ = nullptr;  // +0x2dc
    bool active_ = true;           // (port) see setActive
};

}  // namespace sq8l::gui
