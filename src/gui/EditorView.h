// The editor window (TplugEditForm, 626x430) as a tree of ported controls: composition in
// the original paint order and mouse routing with Windows/VCL capture semantics.
//
// EditorView knows nothing about pages, parameters or programs: the editor logic (layer 3)
// drives the controls' public state (LCD cells, knob ranges/values, button AniIdx, LED
// values, status texts) and installs event handlers (onClick, onChange, onCellMouseDown...).
// The constructor reproduces the state of the original form right after FormShow (the
// runtime set-up of plugEdit FUN_00483d9c and lcdControl FUN_0047e340 included), before any
// page content is written.
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Bitmap.h"
#include "controls/AniDisplay.h"
#include "controls/GraphButton.h"
#include "controls/Knob.h"
#include "controls/LcdDisplay.h"
#include "controls/StaticControls.h"

namespace sq8l::gui {

// Win32 mouse message key flags (wParam of WM_MOUSEMOVE etc.).
enum : uint32_t { MK_LBUTTON_ = 0x1, MK_RBUTTON_ = 0x2, MK_SHIFT_ = 0x4, MK_CONTROL_ = 0x8, MK_MBUTTON_ = 0x10 };

// The form itself as a mouse target (OnMouseMove = MouseMoveHint, OnContextPopup...).
class FormArea : public Control {
public:
    FormArea() : Control("plugEditForm", true) { setBounds(0, 0, 626, 430); }
    void paint(Canvas&) override {}
};

class EditorView : public ControlHost {
public:
    static constexpr int kWidth = 626, kHeight = 430;
    // The form colour (Color = 2366234) used by panels and the erase brush.
    static constexpr Color kFormColor = rgb(0x1A, 0x1B, 0x24);
    // TLCD3 charsets set up by the original (lcdControl FUN_0047e340, plugEdit FUN_00483d9c).
    static const char* const kLcdCharset;     // ' ' .. '`', '{' .. '~'  (68 glyphs x 4 attributes)
    static const char* const kNumLcdCharset;  // "?0123456789ABCDPU": index 0 = blank 7-segment

    EditorView();
    ~EditorView() override;

    // ------------------------------------------------------------ controls
    LcdDisplay& lcd() { return *lcd_; }
    LcdDisplay& numLcd() { return *numLcd_; }
    Knob& knob(int i) { return *knobs_[i]; }  // lcdKnob0..9 (0-4 top row, 5-9 bottom row)
    GraphButton& button(const std::string& name);  // upButton, buttOsc1, pscrUpButton, ...
    AniDisplay& ledSync() { return *ledSync_; }
    AniDisplay& ledAm() { return *ledAm_; }
    AniDisplay& ledMono() { return *ledMono_; }
    Panel& statusPanel2() { return *statusPanel2_; }  // voices counter panel (top right)
    Panel& panel1() { return *panel1_; }              // status bar panel (bottom)
    Label& statusLabel1() { return *statusLabel1_; }  // status bar text (hints, messages)
    Label& statusLabel2() { return *statusLabel2_; }  // "used/available" voices
    NameEdit& progNameEdit() { return *progNameEdit_; }
    ImageArea& image(const std::string& name);        // menuFileImage, menuOptImage, ...
    FormArea& form() { return form_; }
    Control* find(const std::string& name);
    // Windowed children in z-order (bottom to top), then graphic controls of the form.
    const std::vector<Control*>& windowedChildren() const { return windowed_; }
    const std::vector<Control*>& formGraphicControls() const { return graphic_; }
    std::vector<GraphButton*> buttons() const;

    // Convenience for the logic layer.
    void setStatusText(const std::string& text) { statusLabel1_->setCaption(text, text_); }  // FUN_0048367c
    void setVoicesText(const std::string& text) { statusLabel2_->setCaption(text, text_); }  // FUN_004836cc

    // ------------------------------------------------------------ rendering
    void setTextRenderer(TextRenderer* t) { text_ = t; }
    // Paint every visible control (WM_PAINT on the whole tree) and compose the 626x430 frame:
    // background (Form.Brush.Bitmap), then each windowed child's surface in z-order.
    void render(Bitmap& out);

    // ------------------------------------------------------------ input
    // Form client coordinates; keys = MK_* flags as in the Win32 message wParam.
    void mouseMove(int x, int y, uint32_t keys);
    void mouseDown(MouseButton button, int x, int y, uint32_t keys);
    void mouseUp(MouseButton button, int x, int y, uint32_t keys);
    void mouseDoubleClick(MouseButton button, int x, int y, uint32_t keys);  // WM_xBUTTONDBLCLK
    bool altDown = false;  // ssAlt (GetKeyState(VK_MENU))
    // Topmost visible windowed child at a form point, nullptr for the form itself.
    Control* windowAt(int x, int y) const;
    // Where a mouse message at (x, y) goes, honouring the capture (control + its coordinates).
    Control* mouseTarget(int x, int y, int& lx, int& ly);
    // Called after a right button up (where Windows would send WM_CONTEXTMENU).
    std::function<void(Control& target, int formX, int formY)> onContextMenu;
    // (port) Pointer lock, for turning a knob with the cursor hidden (issue #25). While it is
    // on, the cursor is put back to `x, y` after every move and the controls are given the
    // distance it has travelled since the lock started instead of where it is: the pointer
    // can never reach the edge of the screen and stop the drag. Unlocking parks it at `x, y`,
    // where the turn began. Without `warpCursor` the lock still tracks correctly, it just
    // cannot pin anything.
    void lockPointer(int x, int y);
    void unlockPointer();
    bool pointerLocked() const { return locked_; }
    // Move the cursor to (x, y) in form coordinates and write back where it actually ended
    // up -- a platform that refuses to move it (XWayland) reports the position unchanged, so
    // the next movement is still measured from the right place.
    std::function<void(int& x, int& y)> warpCursor;

    // ControlHost
    Control* mouseCapture() const override { return capture_; }
    void setMouseCapture(Control* c) override;
    // Windows WM_CANCELMODE / WM_CAPTURECHANGED: a menu or modal loop took the mouse, so the
    // control that had it gets no button-up (the platform layer calls it after such a loop).
    void cancelMouseMode();
    void setFocus(Control* c) override { focus_ = c; }
    TextRenderer* textRenderer() override { return text_; }
    Control* focused() const { return focus_; }
    Control* captureWindow() const { return captureWin_; }

private:
    void add(Control* c);
    void dispatch(int msg, MouseButton button, int x, int y, uint32_t keys);
    ShiftState shiftOf(uint32_t keys) const;
    Control* graphicAt(Control* window, int x, int y) const;

    std::unique_ptr<LcdDisplay> lcd_, numLcd_;
    std::array<std::unique_ptr<Knob>, 10> knobs_;
    std::vector<std::unique_ptr<GraphButton>> buttons_;
    std::unique_ptr<AniDisplay> ledSync_, ledAm_, ledMono_;
    std::unique_ptr<Panel> statusPanel2_, panel1_;
    std::unique_ptr<Label> statusLabel1_, statusLabel2_;
    std::unique_ptr<NameEdit> progNameEdit_;
    std::vector<std::unique_ptr<ImageArea>> images_;
    FormArea form_;
    std::vector<Control*> windowed_;  // z-order
    std::vector<Control*> graphic_;   // form graphic controls (DFM order)
    Control* captureWin_ = nullptr;   // Windows capture (a windowed control or &form_)
    // pointer lock: where the cursor is parked, where it was last seen, and the point the
    // controls are given (the start plus everything the mouse has travelled since)
    bool locked_ = false;
    int anchorX_ = 0, anchorY_ = 0, lockX_ = 0, lockY_ = 0, virtX_ = 0, virtY_ = 0;
    Control* capture_ = nullptr;      // VCL capture control (may be a graphic control)
    Control* focus_ = nullptr;
    TextRenderer* text_ = nullptr;
};

}  // namespace sq8l::gui
