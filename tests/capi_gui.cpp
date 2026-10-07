// C API: the GUI EditorView and its controls, for tests/test_gui_*.py (comparison with the
// original editor running in the emulator).
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

#include "EditorView.h"
#include "Sprite.h"

#include "capi_export.h"

using namespace sq8l::gui;

namespace {

uint32_t fbits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
float bitsf(uint32_t b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

struct Box {
    EditorView view;
    Bitmap frame;
    std::ostringstream log;
    int cursorX = 0, cursorY = 0;

    Box() {
        // Record the events the logic layer would receive.
        auto rec = [this](Control& c) {
            c.onClick = [this](Control& s) { log << "click " << s.name() << "\n"; };
            c.onDblClick = [this](Control& s) { log << "dblclick " << s.name() << "\n"; };
            c.onMouseDown = [this](Control& s, MouseButton b, ShiftState sh, int x, int y) {
                log << "down " << s.name() << " " << int(b) << " " << sh << " " << x << " " << y << "\n";
            };
            c.onMouseUp = [this](Control& s, MouseButton b, ShiftState sh, int x, int y) {
                log << "up " << s.name() << " " << int(b) << " " << sh << " " << x << " " << y << "\n";
            };
        };
        for (Control* c : view.windowedChildren()) rec(*c);
        for (Control* c : view.formGraphicControls()) rec(*c);
        rec(view.form());
        for (int i = 0; i < 10; i++)
            view.knob(i).onChange = [this](Knob& k) { log << "change " << k.name() << " " << fbits(k.value()) << "\n"; };
        // (the form installs the cell handlers on the main LCD only)
        for (LcdDisplay* l : {&view.lcd()}) {
            l->onCellMouseDown = [this](LcdDisplay& s, MouseButton b, ShiftState sh, int col, int row) {
                log << "celldown " << s.name() << " " << int(b) << " " << sh << " " << col << " " << row << "\n";
            };
            l->onCellMouseUp = [this](LcdDisplay& s, MouseButton b, ShiftState sh, int col, int row) {
                log << "cellup " << s.name() << " " << int(b) << " " << sh << " " << col << " " << row << "\n";
            };
            l->onCellMouseMove = [this](LcdDisplay& s, ShiftState sh, int col, int row) {
                log << "cellmove " << s.name() << " " << sh << " " << col << " " << row << "\n";
            };
            l->onCellDblClick = [this](LcdDisplay& s, int col, int row) {
                log << "celldbl " << s.name() << " " << col << " " << row << "\n";
            };
        }
        view.getCursorPos = [this](int& x, int& y) {
            x = cursorX;
            y = cursorY;
        };
        view.setCursorPos = [this](int x, int y) { log << "setcursor " << x << " " << y << "\n"; };
    }
};

Box* B(void* v) { return static_cast<Box*>(v); }
Control* C(void* c) { return static_cast<Control*>(c); }

const Sprite* gifByName(const char* name) {
    if (!name || !*name) return nullptr;
    return assets::byName(name);
}

}  // namespace

// ------------------------------------------------------------------ view

SQ8L_API void* sq8l_gui_new() { return new Box; }
SQ8L_API void sq8l_gui_free(void* v) { delete B(v); }

SQ8L_API void sq8l_gui_render(void* v, uint8_t* rgb) {
    B(v)->view.render(B(v)->frame);
    B(v)->frame.toRGB24(rgb);
}

SQ8L_API void* sq8l_gui_find(void* v, const char* name) { return B(v)->view.find(name); }

// Child windows in z-order: names joined by '\n'.
SQ8L_API int32_t sq8l_gui_zorder(void* v, char* out, int32_t max) {
    std::string s;
    for (Control* c : B(v)->view.windowedChildren()) s += c->name() + "\n";
    std::strncpy(out, s.c_str(), static_cast<size_t>(max));
    return static_cast<int32_t>(s.size());
}

SQ8L_API void sq8l_gui_set_cursor(void* v, int32_t x, int32_t y) {
    B(v)->cursorX = x;
    B(v)->cursorY = y;
}

SQ8L_API int32_t sq8l_gui_events(void* v, char* out, int32_t max) {
    std::string s = B(v)->log.str();
    B(v)->log.str("");
    std::strncpy(out, s.c_str(), static_cast<size_t>(max));
    return static_cast<int32_t>(s.size());
}

SQ8L_API void sq8l_gui_mouse(void* v, int32_t msg, int32_t x, int32_t y, int32_t keys) {
    EditorView& ev = B(v)->view;
    switch (msg) {
        case 0x200: ev.mouseMove(x, y, keys); break;
        case 0x201: ev.mouseDown(MouseButton::Left, x, y, keys); break;
        case 0x202: ev.mouseUp(MouseButton::Left, x, y, keys); break;
        case 0x203: ev.mouseDoubleClick(MouseButton::Left, x, y, keys); break;
        case 0x204: ev.mouseDown(MouseButton::Right, x, y, keys); break;
        case 0x205: ev.mouseUp(MouseButton::Right, x, y, keys); break;
        case 0x206: ev.mouseDoubleClick(MouseButton::Right, x, y, keys); break;
        default: break;
    }
}

// Name of the window holding the mouse capture (Win32 GetCapture; "" if none). For a graphic
// control (VCL CaptureControl) this is its parent window, e.g. "plugEditForm".
SQ8L_API int32_t sq8l_gui_capture(void* v, char* out, int32_t max) {
    Control* c = B(v)->view.captureWindow();
    std::string s = c ? c->name() : "";
    std::strncpy(out, s.c_str(), static_cast<size_t>(max));
    return static_cast<int32_t>(s.size());
}

SQ8L_API void sq8l_gui_set_capture(void* v, const char* name) {
    B(v)->view.setMouseCapture(name && *name ? B(v)->view.find(name) : nullptr);
}

// ------------------------------------------------------------------ any control

SQ8L_API void sq8l_gui_set_bounds(void* c, int32_t l, int32_t t, int32_t w, int32_t h) { C(c)->setBounds(l, t, w, h); }
SQ8L_API void sq8l_gui_get_bounds(void* c, int32_t* out) {
    out[0] = C(c)->left();
    out[1] = C(c)->top();
    out[2] = C(c)->width();
    out[3] = C(c)->height();
}
SQ8L_API void sq8l_gui_set_visible(void* c, int32_t vis) { C(c)->setVisible(vis != 0); }
SQ8L_API int32_t sq8l_gui_hint(void* c, char* out, int32_t max) {
    std::strncpy(out, C(c)->hint.c_str(), static_cast<size_t>(max));
    return static_cast<int32_t>(C(c)->hint.size());
}
// WM_PAINT of one windowed control; copies its surface (w*h*3) and returns w | h << 16.
SQ8L_API int32_t sq8l_gui_paint_control(void* c, uint8_t* rgb, int32_t maxPixels) {
    Control* k = C(c);
    k->paintWindow(nullptr);
    const Bitmap& s = k->surface();
    if (s.width() * s.height() <= maxPixels) s.toRGB24(rgb);
    return s.width() | (s.height() << 16);
}

// ------------------------------------------------------------------ LCD (TLCD3)

// Reconfigure like the setters would (clears the cells). tcolors: TColor x3 (back, lo, hi).
SQ8L_API void sq8l_gui_lcd_setup(void* c, const char* gif, int32_t cols, int32_t rows, int32_t gapX, int32_t gapY,
                                 int32_t frameW, int32_t frameH, const uint32_t* tcolors) {
    auto* l = static_cast<LcdDisplay*>(C(c));
    l->setAniGif(gifByName(gif));
    l->setFrameWidth(frameW);
    l->setFrameHeight(frameH);
    l->setCharGapX(gapX);
    l->setCharGapY(gapY);
    l->setWidthChar(cols);
    l->setHeightChar(rows);
    for (int i = 0; i < 3; i++) l->setColor(i, fromTColor(tcolors[i]));
}

SQ8L_API void sq8l_gui_lcd_set_charmap(void* c, const char* chars, int32_t nattr, int32_t lowerToUpper) {
    auto* l = static_cast<LcdDisplay*>(C(c));
    l->setCharMap(chars, nattr);
    if (lowerToUpper) l->mapLowerToUpper();
}

SQ8L_API void sq8l_gui_lcd_set_charmap_raw(void* c, const int32_t* map, int32_t nattr) {
    std::array<int32_t, 256> m;
    for (int i = 0; i < 256; i++) m[i] = map[i];
    static_cast<LcdDisplay*>(C(c))->setCharMapRaw(m, nattr);
}

SQ8L_API void sq8l_gui_lcd_set_cell(void* c, int32_t col, int32_t row, int32_t ch, int32_t attr, int32_t dirty) {
    static_cast<LcdDisplay*>(C(c))->setCell(col, row, static_cast<uint8_t>(ch), attr, dirty != 0);
}

// cells: rows*cols*(ch, attr, dirty)
SQ8L_API void sq8l_gui_lcd_get_cells(void* c, int32_t* out) {
    auto* l = static_cast<LcdDisplay*>(C(c));
    for (int r = 0; r < l->rows(); r++)
        for (int k = 0; k < l->cols(); k++) {
            const LcdDisplay::Cell& cl = l->cell(k, r);
            *out++ = cl.ch;
            *out++ = cl.attr;
            *out++ = cl.dirty;
        }
}

SQ8L_API void sq8l_gui_lcd_write(void* c, int32_t col, int32_t row, const char* text, int32_t len, int32_t attr) {
    static_cast<LcdDisplay*>(C(c))->writeText(col, row, std::string(text, static_cast<size_t>(len)), attr);
}

// charW, charH, gapX, gapY, frameW, frameH, widthPix, heightPix, cols, rows, nattr, width, height,
// needsRepaint, lastX, lastY, colors[5] (TColor)
SQ8L_API void sq8l_gui_lcd_info(void* c, int32_t* out) {
    auto* l = static_cast<LcdDisplay*>(C(c));
    int32_t v[] = {l->charWidth(), l->charHeight(), l->charGapX(),  l->charGapY(), l->frameWidth(), l->frameHeight(),
                   l->widthPix(),  l->heightPix(),  l->cols(),      l->rows(),     l->numAttr(),    l->width(),
                   l->height(),    l->redrawAllPending() | (l->anyDirtyPending() << 1), l->lastMouseX, l->lastMouseY};
    std::memcpy(out, v, sizeof v);
    for (int i = 0; i < 5; i++) out[16 + i] = static_cast<int32_t>(toTColor(l->color(i)));
}

SQ8L_API void sq8l_gui_lcd_charmap(void* c, int32_t* out) {
    auto* l = static_cast<LcdDisplay*>(C(c));
    for (int i = 0; i < 256; i++) out[i] = l->charMap()[i];
}

SQ8L_API void sq8l_gui_lcd_cell_at(void* c, int32_t x, int32_t y, int32_t* out) {
    static_cast<LcdDisplay*>(C(c))->cellAt(x, y, out[0], out[1]);
}

// ------------------------------------------------------------------ knob (TGraphKnobB)

// Field order (32-bit words, floats as bits): value, min, max, range, step, pixFactor,
// snapZone, minFineFac, acc, angle, angleK, maxPixDist, maxFinePixDist, radius, frameX, frameY,
// downX, downY, lastX, lastY, snapToZero, intMode, dragging, dragActive, restoreMouse, loaded,
// force, textValid
SQ8L_API void sq8l_gui_knob_get(void* c, uint32_t* o) {
    Knob::State s = static_cast<Knob*>(C(c))->state();
    uint32_t v[] = {fbits(s.value), fbits(s.min), fbits(s.max), fbits(s.range), fbits(s.step), fbits(s.pixFactor),
                    fbits(s.snapZone), fbits(s.minFineFac), fbits(s.acc), fbits(s.angle), fbits(s.angleK),
                    uint32_t(s.maxPixDist), uint32_t(s.maxFinePixDist), uint32_t(s.radius), uint32_t(s.frameX),
                    uint32_t(s.frameY), uint32_t(s.downX), uint32_t(s.downY), uint32_t(s.lastX), uint32_t(s.lastY),
                    s.snapToZero, s.intMode, s.dragging, s.dragActive, s.restoreMouse, s.loaded, s.force,
                    s.textValid};
    std::memcpy(o, v, sizeof v);
}

SQ8L_API void sq8l_gui_knob_set(void* c, const uint32_t* i) {
    Knob::State s{};
    s.value = bitsf(i[0]);
    s.min = bitsf(i[1]);
    s.max = bitsf(i[2]);
    s.range = bitsf(i[3]);
    s.step = bitsf(i[4]);
    s.pixFactor = bitsf(i[5]);
    s.snapZone = bitsf(i[6]);
    s.minFineFac = bitsf(i[7]);
    s.acc = bitsf(i[8]);
    s.angle = bitsf(i[9]);
    s.angleK = bitsf(i[10]);
    s.maxPixDist = int32_t(i[11]);
    s.maxFinePixDist = int32_t(i[12]);
    s.radius = int32_t(i[13]);
    s.frameX = int32_t(i[14]);
    s.frameY = int32_t(i[15]);
    s.downX = int32_t(i[16]);
    s.downY = int32_t(i[17]);
    s.lastX = int32_t(i[18]);
    s.lastY = int32_t(i[19]);
    s.snapToZero = i[20] != 0;
    s.intMode = i[21] != 0;
    s.dragging = i[22] != 0;
    s.dragActive = i[23] != 0;
    s.restoreMouse = i[24] != 0;
    s.loaded = i[25] != 0;
    s.force = i[26] != 0;
    s.textValid = i[27] != 0;
    static_cast<Knob*>(C(c))->setState(s);
}

// fn: 0 setValue, 1 setMinValue, 2 setMaxValue, 3 setValueStep (float bits), 4 setMaxPixDist,
// 5 setMaxFinePixDist (int), 6 beginDrag(shift, x, y), 7 dragMove, 8 endDrag, 9 cancelDrag
SQ8L_API void sq8l_gui_knob_call(void* c, int32_t fn, uint32_t a, int32_t x, int32_t y) {
    auto* k = static_cast<Knob*>(C(c));
    switch (fn) {
        case 0: k->setValue(bitsf(a)); break;
        case 1: k->setMinValue(bitsf(a)); break;
        case 2: k->setMaxValue(bitsf(a)); break;
        case 3: k->setValueStep(bitsf(a)); break;
        case 4: k->setMaxPixDist(int32_t(a)); break;
        case 5: k->setMaxFinePixDist(int32_t(a)); break;
        case 6: k->beginDrag(a, x, y); break;
        case 7: k->dragMove(a, x, y); break;
        case 8: k->endDrag(); break;
        case 9: k->cancelDrag(); break;
        default: break;
    }
}

SQ8L_API int32_t sq8l_gui_knob_frame(void* c) { return static_cast<Knob*>(C(c))->frameIndex(); }

// ------------------------------------------------------------------ button (TGraphButton)

SQ8L_API void sq8l_gui_button_set(void* c, const char* gif, int32_t aniIdx, int32_t two, int32_t pressed,
                                  int32_t hover, int32_t frameWidth, uint32_t frameTColor, int32_t pressDisp) {
    auto* b = static_cast<GraphButton*>(C(c));
    b->setAniGif(gifByName(gif));
    b->setAniIdx(aniIdx);
    b->setHasTwoFrames(two != 0);
    b->setPressed(pressed);
    b->setHover(hover != 0);
    b->setFrameWidth(frameWidth);
    b->setFrameColor(fromTColor(frameTColor));
    b->setPressDisplacement(pressDisp);
}

// aniIdx, twoFrames, pressed, hover, frameWidth, frameColor (TColor), pressDisp, frameIndex
SQ8L_API void sq8l_gui_button_get(void* c, int32_t* o) {
    auto* b = static_cast<GraphButton*>(C(c));
    int32_t v[] = {b->aniIdx(), b->hasTwoFrames(), b->pressed(), b->hover(), b->frameWidth(),
                   int32_t(toTColor(b->frameColor())), b->pressDisplacement(), b->frameIndex()};
    std::memcpy(o, v, sizeof v);
}

// ------------------------------------------------------------------ TAniDisplay

SQ8L_API void sq8l_gui_ani_set(void* c, const char* gif, int32_t start, int32_t num, uint32_t minb, uint32_t maxb,
                               uint32_t valb) {
    auto* a = static_cast<AniDisplay*>(C(c));
    a->setAniGif(gifByName(gif));
    a->setRaw(start, num, bitsf(minb), bitsf(maxb), bitsf(valb));
}

// start, num, min, max, value (bits), frameIndex
SQ8L_API void sq8l_gui_ani_get(void* c, uint32_t* o) {
    auto* a = static_cast<AniDisplay*>(C(c));
    uint32_t v[] = {uint32_t(a->startFrame()), uint32_t(a->numFrames()), fbits(a->minVal()), fbits(a->maxVal()),
                    fbits(a->value()), uint32_t(a->frameIndex())};
    std::memcpy(o, v, sizeof v);
}

// fn: 0 setValue, 1 setMinVal, 2 setMaxVal (float bits), 3 setStartFrame, 4 setNumFrames
SQ8L_API void sq8l_gui_ani_call(void* c, int32_t fn, uint32_t a) {
    auto* d = static_cast<AniDisplay*>(C(c));
    switch (fn) {
        case 0: d->setValue(bitsf(a)); break;
        case 1: d->setMinVal(bitsf(a)); break;
        case 2: d->setMaxVal(bitsf(a)); break;
        case 3: d->setStartFrame(int32_t(a)); break;
        case 4: d->setNumFrames(int32_t(a)); break;
        default: break;
    }
}

// ------------------------------------------------------------------ text controls

SQ8L_API void sq8l_gui_label_set(void* c, const char* text) { static_cast<Label*>(C(c))->setCaption(text); }
SQ8L_API void sq8l_gui_edit_set(void* c, const char* text) { static_cast<NameEdit*>(C(c))->setText(text); }

// ------------------------------------------------------------------ assets

// Copies frame `index` of sprite `name` (w*h*3 RGB); returns w | h << 16 | count << 32 packed in
// out[0..2].
SQ8L_API int32_t sq8l_gui_sprite_frame(const char* name, int32_t index, uint8_t* rgb, int32_t* info) {
    const Sprite* s = assets::byName(name);
    if (!s) return 0;
    info[0] = s->frameWidth();
    info[1] = s->frameHeight();
    info[2] = s->count();
    ImageView f = s->frame(index);
    if (!f) return 0;
    for (int y = 0; y < f.height; y++)
        for (int x = 0; x < f.width; x++) {
            Color c = f.at(x, y);
            *rgb++ = uint8_t(c >> 16);
            *rgb++ = uint8_t(c >> 8);
            *rgb++ = uint8_t(c);
        }
    return 1;
}

SQ8L_API void sq8l_gui_background(uint8_t* rgb) { assets::background().toRGB24(rgb); }

#include "text/StbTextRenderer.h"
SQ8L_API void sq8l_gui_use_text(void* v) {
    static sq8l::gui::StbTextRenderer text;
    B(v)->view.setTextRenderer(&text);
}
