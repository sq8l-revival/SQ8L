#include "EditorView.h"

#include <stdexcept>

#include "Sprite.h"

namespace sq8l::gui {

const char* const EditorView::kLcdCharset =
    " \"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`{|}~";
const char* const EditorView::kNumLcdCharset = " 0123456789ABCDPU";

namespace {

enum { WM_MOUSEMOVE_ = 0x200, WM_LBUTTONDOWN_, WM_LBUTTONUP_, WM_LBUTTONDBLCLK_, WM_RBUTTONDOWN_, WM_RBUTTONUP_,
       WM_RBUTTONDBLCLK_, WM_MBUTTONDOWN_, WM_MBUTTONUP_, WM_MBUTTONDBLCLK_ };

struct ButtonDef {
    const char* name;
    int left, top, width, height;
    const Sprite& (*gif)();
    int aniIdx;  // after FormShow
    bool twoFrames;
    int tag;
    const char* hint;
};

// TGraphButtons of TPLUGEDITFORM (DFM order). Positions/sizes from the form, AniIdx as
// after the original's start-up (e.g. the page scroll arrows are set by lcdControl).
const ButtonDef kButtons[] = {
    {"upButton", 281, 39, 30, 18, assets::upDownGif, 0, true, 0, "Load next sound program"},
    {"downButton", 281, 58, 30, 18, assets::upDownGif, 2, true, 0, "Load previous sound program"},
    {"writeButton", 384, 52, 44, 22, assets::buttGif, 2, true, 0, "Write sound program to library"},
    {"initButton", 432, 52, 44, 22, assets::buttGif, 2, true, 0, "Initialize sound program"},
    {"sysExSendButton", 497, 52, 44, 22, assets::buttGif, 0, true, 0, "Send sound to ESQ1/SQ80"},
    {"sysExReqButton", 546, 52, 44, 22, assets::buttGif, 0, true, 0, "Request sound from ESQ1/SQ80"},
    {"buttOsc1", 158, 110, 44, 22, assets::buttGif, 2, true, 16, "Oscillator 1"},
    {"buttOsc2", 158, 138, 44, 22, assets::buttGif, 2, true, 32, "Oscillator 2"},
    {"buttOsc3", 158, 166, 44, 22, assets::buttGif, 2, true, 48, "Oscillator 3"},
    {"buttDca1", 277, 110, 44, 22, assets::buttGif, 0, true, 17, "Oscillator 1 volume"},
    {"buttDca2", 277, 138, 44, 22, assets::buttGif, 0, true, 33, "Oscillator 2 volume"},
    {"buttDca3", 277, 166, 44, 22, assets::buttGif, 0, true, 49, "Oscillator 3 volume"},
    {"buttFilt", 376, 138, 44, 22, assets::buttGif, 0, true, 64, "Filter"},
    {"buttDca4", 445, 138, 44, 22, assets::buttGif, 0, true, 80, "Final volume, panning and saturation"},
    {"buttModes", 552, 160, 44, 22, assets::buttGif, 4, true, 287, "Modes / Emulation settings"},
    {"buttSync", 110, 126, 30, 18, assets::smallButtGif, 0, true, 0, "Sync oscilllator 2 to oscillator 1"},
    {"buttAm", 223, 126, 30, 18, assets::smallButtGif, 0, true, 0,
     "Amplitude of oscillator 2 is modulated by oscillator 1"},
    {"buttLfo1", 49, 370, 44, 22, assets::buttGif, 0, true, 111, "LFO 1"},
    {"buttLfo2", 95, 370, 44, 22, assets::buttGif, 0, true, 127, "LFO 2"},
    {"buttLfo3", 141, 370, 44, 22, assets::buttGif, 0, true, 143, "LFO 3"},
    {"buttLfo4", 187, 370, 44, 22, assets::buttGif, 0, true, 159, "LFO 4"},
    {"buttEnv1", 243, 370, 44, 22, assets::buttGif, 0, true, 175, "Envelope 1"},
    {"buttEnv2", 289, 370, 44, 22, assets::buttGif, 0, true, 191, "Envelope 2"},
    {"buttEnv3", 335, 370, 44, 22, assets::buttGif, 0, true, 207, "Envelope 3"},
    {"buttEnv4", 381, 370, 44, 22, assets::buttGif, 0, true, 223, "Envelope 4 (modulates final volume)"},
    {"buttMat1", 440, 370, 44, 22, assets::buttGif, 0, true, 224, "Matrix modulator 1"},
    {"buttMat2", 486, 370, 44, 22, assets::buttGif, 0, true, 240, "Matrix modulator 2"},
    {"buttMat3", 532, 370, 44, 22, assets::buttGif, 0, true, 256, "Matrix modulator 3"},
    {"buttWav", 28, 138, 44, 22, assets::buttGif, 4, true, 15, "Oscillator volumes, waves and pitches"},
    {"buttMono", 561, 110, 30, 18, assets::smallButtGif, 0, true, 0, "Monophonic play"},
    {"BankButton", 318, 52, 44, 22, assets::buttGif, 4, true, 0, "Switch current sound bank"},
    {"pscrUpButton", 570, 252, 20, 18, assets::scrButtGif, 0, false, 0, ""},
    {"pscrDownButton", 570, 270, 20, 18, assets::scrButtGif, 3, false, 0, ""},
};

// Windowed children of the form in z-order (creation order of the window handles).
const char* const kZOrder[] = {
    "lcdKnob0", "lcdKnob1", "lcdKnob2", "lcdKnob3", "lcdKnob4", "lcdKnob9", "lcdKnob8", "lcdKnob7", "lcdKnob6",
    "lcdKnob5", "StatusPanel2", "Panel1", "lcd", "upButton", "downButton", "writeButton", "initButton",
    "progNameEdit", "sysExSendButton", "sysExReqButton", "numLcd", "ledSync", "buttOsc1", "buttOsc2", "buttOsc3",
    "buttDca1", "buttDca2", "buttDca3", "buttFilt", "buttDca4", "buttModes", "buttSync", "ledAm", "buttAm",
    "buttLfo1", "buttLfo2", "buttLfo3", "buttLfo4", "buttEnv1", "buttEnv2", "buttEnv3", "buttEnv4", "buttMat1",
    "buttMat2", "buttMat3", "buttWav", "ledMono", "buttMono", "BankButton", "pscrUpButton", "pscrDownButton",
};

}  // namespace

EditorView::EditorView() {
    Font labelFont;  // MS Sans Serif 8, Font.Color = 10521997
    labelFont.face = "MS Sans Serif";
    labelFont.height = -11;
    labelFont.color = fromTColor(10521997);

    // ---- knobs: lcdKnob0..9, KnobGif, Radius 20, no caption/value; positions after lcdControl
    // aligned them on the LCD columns (8 characters apart).
    for (int i = 0; i < 10; i++) {
        auto k = std::make_unique<Knob>("lcdKnob" + std::to_string(i));
        k->setBounds(118 + 96 * (i % 5), i < 5 ? 194 : 304, 40, 40);
        k->setMinValue(-127);
        k->setMaxValue(127);
        k->setValueStep(1);
        k->setValue(1);
        k->setSnapToZero(true);
        k->setMinFineFac(0.2f);
        k->setMaxFinePixDist(100);
        k->setMaxPixDist(200);
        k->setRadius(20);
        k->setShowCaption(false);
        k->setShowValue(false);
        k->setAniGif(&assets::knobGif());
        knobs_[i] = std::move(k);
    }

    // ---- main VFD: 44 x 2 characters of charGIF (4 attributes), gaps 1/3, frame 4.
    lcd_ = std::make_unique<LcdDisplay>("lcd");
    lcd_->setBounds(34, 247, 8, 8);
    lcd_->setAniGif(&assets::charGif());
    lcd_->setCharMap(kLcdCharset, 4);
    lcd_->mapLowerToUpper();
    lcd_->setWidthChar(44);
    lcd_->setHeightChar(2);
    lcd_->setCharGapX(1);
    lcd_->setCharGapY(3);

    // ---- program number: 4 x 1 characters of numCharGIF, no gaps/frame.
    numLcd_ = std::make_unique<LcdDisplay>("numLcd");
    numLcd_->setBounds(30, 44, 0, 0);
    numLcd_->setFrameWidth(0);
    numLcd_->setFrameHeight(0);
    numLcd_->setColor(0, fromTColor(393251));
    numLcd_->setColor(1, fromTColor(393251));
    numLcd_->setColor(2, fromTColor(393251));
    numLcd_->setAniGif(&assets::numCharGif());
    numLcd_->setWidthChar(4);
    numLcd_->setHeightChar(1);
    numLcd_->setCharGapX(0);
    numLcd_->setCharGapY(0);
    numLcd_->setCharMap(kNumLcdCharset, 1);
    numLcd_->hint = "Sound program bank/number";

    // ---- buttons
    for (const ButtonDef& d : kButtons) {
        auto b = std::make_unique<GraphButton>(d.name);
        b->setPressDisplacement(0);
        b->setBounds(d.left, d.top, d.width, d.height);
        b->setAniGif(&d.gif());
        b->setAniIdx(d.aniIdx);
        b->setHasTwoFrames(d.twoFrames);
        b->hint = d.hint;
        b->font.face = "Arial";
        b->font.italic = true;
        buttons_.push_back(std::move(b));
    }
    // FormShow: hover frame for the page scroll arrows.
    button("pscrUpButton").setFrameColor(fromTColor(0x5f7c0e));
    button("pscrUpButton").setFrameWidth(1);
    button("pscrDownButton").setFrameColor(fromTColor(0x5f7c0e));
    button("pscrDownButton").setFrameWidth(1);

    // ---- LEDs: LedGIF, 2 frames, 0..1.
    auto led = [](const char* name, int l, int t) {
        auto a = std::make_unique<AniDisplay>(name);
        a->setBounds(l, t, 14, 14);
        a->setAniGif(&assets::ledGif());
        a->setStartFrame(0);
        a->setNumFrames(2);
        a->setMaxVal(1.0f);
        return a;
    };
    ledSync_ = led("ledSync", 94, 127);
    ledAm_ = led("ledAm", 207, 127);
    ledMono_ = led("ledMono", 545, 111);

    // ---- panels and labels
    statusPanel2_ = std::make_unique<Panel>("StatusPanel2");
    statusPanel2_->setBounds(496, 4, 106, 18);
    statusLabel2_ = std::make_unique<Label>("StatusLabel2");
    statusLabel2_->setBounds(86, 2, 17, 13);
    statusLabel2_->font = labelFont;
    statusLabel2_->alignment = Label::RightJustify;
    statusLabel2_->hint = "Voices used / available";
    statusLabel2_->parent = statusPanel2_.get();
    statusPanel2_->children.push_back(statusLabel2_.get());

    panel1_ = std::make_unique<Panel>("Panel1");
    panel1_->setBounds(24, 412, 545, 16);
    statusLabel1_ = std::make_unique<Label>("StatusLabel1");
    statusLabel1_->setBounds(1, 1, 62, 13);
    statusLabel1_->font = labelFont;
    statusLabel1_->parent = panel1_.get();
    panel1_->children.push_back(statusLabel1_.get());

    // ---- program name
    progNameEdit_ = std::make_unique<NameEdit>("progNameEdit");
    progNameEdit_->setBounds(110, 48, 158, 15);
    progNameEdit_->font.face = "Arial";
    progNameEdit_->font.height = -11;
    progNameEdit_->font.color = fromTColor(9140344);
    progNameEdit_->hint = "Sound program name";

    // ---- menu areas (TImage without picture), DFM order
    struct ImageDef {
        const char* name;
        int l, t, w, h;
        const char* hint;
    };
    const ImageDef images[] = {{"menuFileImage", 9, 0, 56, 24, "File menu"},
                               {"menuInfoImage", 146, 0, 44, 24, "Info menu"},
                               {"menuOptImage", 66, 0, 79, 24, "Options menu"},
                               {"menuPanicImage", 194, 0, 55, 24, "Panic (reset all voices)"}};
    for (const ImageDef& d : images) {
        auto im = std::make_unique<ImageArea>(d.name);
        im->setBounds(d.l, d.t, d.w, d.h);
        im->hint = d.hint;
        graphic_.push_back(im.get());
        images_.push_back(std::move(im));
    }
    for (Control* c : graphic_) c->host = this;

    // ---- z-order
    for (const char* n : kZOrder) add(find(n));
    form_.host = this;
    statusLabel1_->host = this;
    statusLabel2_->host = this;
}

EditorView::~EditorView() = default;

void EditorView::add(Control* c) {
    c->host = this;
    windowed_.push_back(c);
}

GraphButton& EditorView::button(const std::string& name) {
    for (auto& b : buttons_)
        if (b->name() == name) return *b;
    throw std::out_of_range("no button " + name);
}

ImageArea& EditorView::image(const std::string& name) {
    for (auto& i : images_)
        if (i->name() == name) return *i;
    throw std::out_of_range("no image " + name);
}

std::vector<GraphButton*> EditorView::buttons() const {
    std::vector<GraphButton*> v;
    for (auto& b : buttons_) v.push_back(b.get());
    return v;
}

Control* EditorView::find(const std::string& name) {
    if (name == "lcd") return lcd_.get();
    if (name == "numLcd") return numLcd_.get();
    for (auto& k : knobs_)
        if (k->name() == name) return k.get();
    for (auto& b : buttons_)
        if (b->name() == name) return b.get();
    for (AniDisplay* a : {ledSync_.get(), ledAm_.get(), ledMono_.get()})
        if (a->name() == name) return a;
    if (name == "StatusPanel2") return statusPanel2_.get();
    if (name == "Panel1") return panel1_.get();
    if (name == "StatusLabel1") return statusLabel1_.get();
    if (name == "StatusLabel2") return statusLabel2_.get();
    if (name == "progNameEdit") return progNameEdit_.get();
    for (auto& i : images_)
        if (i->name() == name) return i.get();
    if (name == form_.name()) return &form_;
    return nullptr;
}

// ------------------------------------------------------------------ rendering

void EditorView::render(Bitmap& out) {
    if (out.width() != kWidth || out.height() != kHeight) out.resize(kWidth, kHeight);
    // Form: WM_ERASEBKGND with the pattern brush Form.Brush.Bitmap (626x430, origin 0,0); its
    // graphic controls (picture-less TImages) paint nothing.
    out.blit(0, 0, assets::background().view());
    for (Control* c : windowed_) {
        if (!c->visible()) continue;
        c->paintWindow(text_);
        out.blit(c->left(), c->top(), c->surface().view());
    }
}

// ------------------------------------------------------------------ input

void EditorView::cancelMouseMode() {
    if (capture_) capture_->clicked = false;
    setMouseCapture(nullptr);
    // A knob in the middle of a drag would otherwise only notice at the next mouse move,
    // and the editor keeps the cursor hidden for as long as a drag lasts: a value menu
    // opened by a double click would leave it hidden until the mouse moved again.
    for (auto& k : knobs_) k->cancelDrag();
}

void EditorView::setMouseCapture(Control* c) {
    capture_ = c;
    if (c == nullptr)
        captureWin_ = nullptr;
    else if (c->windowed())
        captureWin_ = c;
    else
        captureWin_ = c->parent ? c->parent : &form_;  // SetCaptureControl: the parent's window
}

Control* EditorView::windowAt(int x, int y) const {
    for (auto it = windowed_.rbegin(); it != windowed_.rend(); ++it) {
        Control* c = *it;
        if (c->visible() && c->bounds().contains(x, y)) return c;
    }
    return nullptr;
}

Control* EditorView::graphicAt(Control* window, int x, int y) const {
    // TWinControl.ControlAtPos(P, AllowDisabled = False): last added first.
    const std::vector<Control*>& list = window == &form_ ? graphic_ : window->children;
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        Control* c = *it;
        if (c->visible() && c->enabled && c->bounds().contains(x, y)) return c;
    }
    return nullptr;
}

Control* EditorView::mouseTarget(int x, int y, int& lx, int& ly) {
    // Windows: the capture window, else the window under the point.
    Control* w = captureWin_ ? captureWin_ : windowAt(x, y);
    if (!w) w = &form_;
    int wx = 0, wy = 0;  // window origin in form coordinates
    if (w != &form_) {
        wx = w->left();
        wy = w->top();
    }
    lx = x - wx;
    ly = y - wy;
    // VCL TWinControl.IsControlMouseMsg: route to a graphic child.
    Control* g = nullptr;
    if (captureWin_ == w)
        g = (capture_ && !capture_->windowed() && (capture_->parent ? capture_->parent : &form_) == w) ? capture_
                                                                                                         : nullptr;
    else
        g = graphicAt(w, lx, ly);
    if (g) {
        lx -= g->left();
        ly -= g->top();
        return g;
    }
    return w;
}

ShiftState EditorView::shiftOf(uint32_t keys) const {
    ShiftState s = 0;
    if (keys & MK_SHIFT_) s |= ssShift;
    if (keys & MK_CONTROL_) s |= ssCtrl;
    if (keys & MK_LBUTTON_) s |= ssLeft;
    if (keys & MK_RBUTTON_) s |= ssRight;
    if (keys & MK_MBUTTON_) s |= ssMiddle;
    if (altDown) s |= ssAlt;
    return s;
}

void EditorView::dispatch(int msg, MouseButton button, int x, int y, uint32_t keys) {
    int lx, ly;
    Control* t = mouseTarget(x, y, lx, ly);
    ShiftState shift = shiftOf(keys);
    switch (msg) {
        case WM_MOUSEMOVE_:
            t->mouseMove(shift, lx, ly);
            break;
        case WM_LBUTTONDOWN_:  // TControl.WMLButtonDown
            if (t->captureMouse) setMouseCapture(t);
            if (t->clickEvents) t->clicked = true;
            t->mouseDown(MouseButton::Left, shift, lx, ly);
            break;
        case WM_LBUTTONDBLCLK_:  // TControl.WMLButtonDblClk
            if (t->captureMouse) setMouseCapture(t);
            if (t->clickEvents) t->dblClick();
            t->mouseDown(MouseButton::Left, shift | ssDouble, lx, ly);
            break;
        case WM_LBUTTONUP_:  // TControl.WMLButtonUp
            if (t->captureMouse && capture_ == t) setMouseCapture(nullptr);
            if (t->clicked) {
                t->clicked = false;
                if (t->clientRect().contains(lx, ly)) t->click();
            }
            t->mouseUp(MouseButton::Left, shift, lx, ly);
            break;
        case WM_RBUTTONDOWN_:
        case WM_MBUTTONDOWN_:
            t->mouseDown(button, shift, lx, ly);
            break;
        case WM_RBUTTONDBLCLK_:
        case WM_MBUTTONDBLCLK_:
            t->mouseDown(button, shift | ssDouble, lx, ly);
            break;
        case WM_RBUTTONUP_:
        case WM_MBUTTONUP_:
            t->mouseUp(button, shift, lx, ly);
            if (msg == WM_RBUTTONUP_ && onContextMenu) onContextMenu(*t, x, y);
            break;
        default:
            break;
    }
}

void EditorView::lockPointer(int x, int y) {
    locked_ = true;
    anchorX_ = lockX_ = virtX_ = x;
    anchorY_ = lockY_ = virtY_ = y;
}

void EditorView::unlockPointer() {
    if (!locked_) return;
    locked_ = false;
    if (warpCursor) {  // leave it where the turn began
        int x = anchorX_, y = anchorY_;
        warpCursor(x, y);
    }
}

void EditorView::mouseMove(int x, int y, uint32_t keys) {
    if (!locked_) {
        dispatch(WM_MOUSEMOVE_, MouseButton::Left, x, y, keys);
        return;
    }
    const int dx = x - lockX_, dy = y - lockY_;
    if (dx == 0 && dy == 0) return;  // the move our own warp caused, or no movement at all
    virtX_ += dx;
    virtY_ += dy;
    lockX_ = x;
    lockY_ = y;
    if (warpCursor) {
        lockX_ = anchorX_;
        lockY_ = anchorY_;
        warpCursor(lockX_, lockY_);  // corrects lockX_/lockY_ if the platform refused
    }
    // The capture is the control being dragged, so mouseTarget routes to it however far the
    // travelled point is from the window.
    dispatch(WM_MOUSEMOVE_, MouseButton::Left, virtX_, virtY_, keys);
}

void EditorView::mouseDown(MouseButton button, int x, int y, uint32_t keys) {
    int msg = button == MouseButton::Left ? WM_LBUTTONDOWN_ : button == MouseButton::Right ? WM_RBUTTONDOWN_ : WM_MBUTTONDOWN_;
    dispatch(msg, button, x, y, keys);
}

void EditorView::mouseUp(MouseButton button, int x, int y, uint32_t keys) {
    int msg = button == MouseButton::Left ? WM_LBUTTONUP_ : button == MouseButton::Right ? WM_RBUTTONUP_ : WM_MBUTTONUP_;
    dispatch(msg, button, x, y, keys);
}

void EditorView::mouseDoubleClick(MouseButton button, int x, int y, uint32_t keys) {
    int msg = button == MouseButton::Left    ? WM_LBUTTONDBLCLK_
              : button == MouseButton::Right ? WM_RBUTTONDBLCLK_
                                             : WM_MBUTTONDBLCLK_;
    dispatch(msg, button, x, y, keys);
}

}  // namespace sq8l::gui
