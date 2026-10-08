// Port of TGraphButton (unit GraphButton, 0x47ba3c-0x47c80c): a sprite push button.
//
// Frame shown = AniIdx (+1 while pressed if HasTwoFrames). The editor selects a look by
// changing AniIdx: ButtGIF 0/1 grey, 2/3 red, 4/5 green; upDownGif 0/1 up, 2/3 down;
// smallButtGIF 0/1; scrButtGif 0 up dim, 1 up lit, 2 down dim, 3 down lit.
// Hover: the button captures the mouse while it is inside (to see it leave) and, if
// frameWidth > 0, draws a 1-pixel frame in frameColor (the page scroll buttons).
// Click fires on mouse up if the button is still pressed (leaving it cancels the press).
#pragma once

#include <string>

#include "../Sprite.h"
#include "Control.h"

namespace sq8l::gui {

class GraphButton : public Control {
public:
    enum CaptionPlace : uint8_t { CapCenter = 0, CapTop = 1, CapRight = 2, CapBottom = 3, CapLeft = 4 };

    explicit GraphButton(std::string name);  // TGraphButton.Create (0x47bf9c)

    void setAniGif(const Sprite* gif);       // +0x200
    void setAniIdx(int idx);                 // FUN_0047c084 (< 0 -> 0)
    void setHasTwoFrames(bool two);          // FUN_0047c47c
    void setCaption(const std::string& c);   // FUN_0047c404
    void setCaptionPlace(CaptionPlace p);
    void setFontColor(Color c);              // FUN_0047c45c(0, c)
    void setFrameColor(Color c);             // FUN_0047c45c(1, c): hover frame colour
    void setFrameWidth(int w);               // FUN_0047c488: > 0 enables the hover frame
    void setPressDisplacement(int d);        // +0x234

    const Sprite* aniGif() const { return gif_; }
    int aniIdx() const { return aniIdx_; }
    bool hasTwoFrames() const { return twoFrames_; }
    const std::string& caption() const { return caption_; }
    int pressed() const { return pressed_; }  // +0x238 (> 0 while the left button is down on it)
    bool hover() const { return hover_; }     // +0x23c
    int frameWidth() const { return frameWidth_; }
    Color frameColor() const { return frameColor_; }
    int pressDisplacement() const { return pressDisp_; }
    int imageX() const { return imageX_; }  // where Paint draws the frame (set by layout)
    int imageY() const { return imageY_; }
    // The sprite frame Paint shows.
    int frameIndex() const { return aniIdx_ + ((twoFrames_ && pressed_ > 0) ? 1 : 0); }

    // Raw state (tests): pressed/hover as in the original object.
    void setPressed(int p) {
        pressed_ = p;
        invalidate();
    }
    void setHover(bool h) {
        hover_ = h;
        invalidate();
    }

    void paint(Canvas& canvas) override;                                          // 0x47c494
    void mouseDown(MouseButton button, ShiftState shift, int x, int y) override;  // 0x47c628
    void mouseUp(MouseButton button, ShiftState shift, int x, int y) override;    // 0x47c670
    void mouseMove(ShiftState shift, int x, int y) override;                      // 0x47c6bc
    void click() override {}  // dynamic -20 overridden empty: Click fires from mouseUp only
    void fireClick() { Control::click(); }  // TControl.Click (0x4428d4)

    Font font;  // Canvas font for the caption (form font; colour = fontColor)

private:
    void layout(Canvas& canvas);  // FUN_0047c100

    const Sprite* gif_ = nullptr;      // +0x200
    int aniIdx_ = 0;                   // +0x204
    int imageX_ = 0, imageY_ = 0;      // +0x208 / +0x20c
    bool twoFrames_ = false;           // +0x210
    std::string caption_;              // +0x214
    CaptionPlace place_ = CapCenter;   // +0x218
    int captionX_ = 0, captionY_ = 0;  // +0x220 / +0x224
    Color fontColor_ = 0xFFFFFF;       // +0x230
    int pressDisp_ = 1;                // +0x234
    int pressed_ = 0;                  // +0x238
    bool hover_ = false;               // +0x23c
    int frameWidth_ = 0;               // +0x240
    Color frameColor_ = 0x00FF00;      // +0x244 (TColor 0xff00 = lime)
};

}  // namespace sq8l::gui
