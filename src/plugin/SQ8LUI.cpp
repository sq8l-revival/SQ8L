// SQ8L port: DPF UI hosting the editor.
//
// The editor (sq8l::gui::EditorView + EditorController, ported from the original) is
// software rendered into a 626x430 RGB frame — exactly the original's pixels — shown as
// one OpenGL texture with nearest-neighbour scaling. Mouse input is translated to the
// Win32-style events the ported controls expect; a 20 ms tick stands in for the
// original's CsimpleTimer. Menus, dialogs and file pickers come from PlatformUi: native
// on macOS and Windows, drawn in the window on Linux (src/gui/drawn, nested event loops;
// SQ8L_DRAWN_UI=1 selects them on macOS too, for testing).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "DistrhoUI.hpp"
#include "EditorView.h"
#include "SQ8LPlugin.hpp"
#include "hd/HdRenderer.h"
#include "logic/EditorController.h"
#include "logic/EditorHost.h"
#include "text/StbTextRenderer.h"
#include "drawn/PlatformUiDrawn.h"

#if defined(__APPLE__)
#include <OpenGL/gl.h>
#include "mac/PlatformUiMac.h"
using NativePlatform = sq8l::gui::PlatformUiMac;
#define SQ8L_NATIVE_UI 1
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>
#include "win/PlatformUiWin.h"
using NativePlatform = sq8l::gui::PlatformUiWin;
#define SQ8L_NATIVE_UI 1
#else
#include <GL/gl.h>
#include "Application.hpp"  // DGL: getApp().idle() for the nested loops
#include "linux/X11Pointer.h"
#define SQ8L_NATIVE_UI 0
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

START_NAMESPACE_DISTRHO

using sq8l::gui::EditorView;
using sq8l::gui::MouseButton;

namespace {

int gEditorsOpened = 0;  // DAT_004c3070: the welcome text only shows in the first editor

// The engine side of the editor (the original reaches it through the form's master and
// edit buffer pointers, the global sound library and CplugConfig).
class PluginEditorHost final : public sq8l::gui::EditorHost {
public:
    explicit PluginEditorHost(SQ8LPlugin& p) : p_(p) {}
    void setController(sq8l::gui::EditorController* c) { controller_ = c; }

    sq8l::EditBuffer& editBuffer() override { return p_.synth().editBuffer(); }
    sq8l::SoundLibrary& library() override { return p_.synth().library(); }
    const sq8l::Settings& settings() override { return p_.settings(); }

    void setGuiSetting(int index, bool value) override {
        if (index < 0 || index >= sq8l::Settings::kNumGui) return;
        const int v = value ? 1 : 0;
        if (p_.settings().gui[index] == v) return;
        p_.settings().gui[index] = v;
        sq8l::SharedLibrary::saveSettings();
        if (controller_) controller_->post(sq8l::gui::kMsgNotify, 0, index);
    }

    void setSynthSetting(int index, int value) override {
        if (index < 0 || index >= sq8l::Settings::kNumSynth) return;
        if (p_.settings().synth[index] == value) return;
        p_.settings().synth[index] = value;
        p_.synth().master().loadOverrides(p_.settings().synth);
        sq8l::SharedLibrary::saveSettings();
        if (controller_) controller_->post(sq8l::gui::kMsgNotify, 0, 0x10 + index);
    }

    void setPortSetting(int index, int value) override {
        if (!p_.settings().setPort(index, value)) return;
        sq8l::SharedLibrary::saveSettings();
        if (controller_) controller_->post(sq8l::gui::kMsgNotify, 0, 0x20 + index);
    }

    // OPTIONS -> Zoom, answered by the UI (window size).
    std::function<int()> currentZoom;
    std::function<void(int)> applyZoom;
    int zoom() override { return currentZoom ? currentZoom() : 100; }
    void setZoom(int percent) override {
        if (applyZoom) applyZoom(percent);
    }
    int polyphonyOverride() override { return p_.synth().polyphonyOverride(); }

    // (engine lock held) The host reads the value back with getState when it saves; the port
    // does not notify it of edits, like every other edit the editor makes.
    void setPolyphonyOverride(int voices) override { p_.synth().setPolyphonyOverride(voices); }

    void panic() override { p_.synth().master().panic(); }
    int voicesUsed() override { return p_.synth().master().activeVoiceCount(); }
    int voicesMax() override { return p_.synth().polyphony(); }

    std::string pluginDirectory() override {
        std::string d = sq8l::userDataDir();
        if (!d.empty() && (d.back() == '/' || d.back() == '\\')) d.pop_back();
        return d;
    }

    // MIDI ports for SEND/REQ: not available yet in the port (the dialogs list no ports).
    std::vector<std::string> midiInPorts() override { return {}; }
    std::vector<std::string> midiOutPorts() override { return {}; }
    void midiCloseAll() override {}
    bool midiOpenOut(int) override { return false; }
    void midiSendOut(const std::vector<uint8_t>&) override {}
    bool midiOpenIn(int) override { return false; }

private:
    SQ8LPlugin& p_;
    sq8l::gui::EditorController* controller_ = nullptr;
};

}  // namespace

class SQ8LUI : public UI {
public:
    SQ8LUI()
        : UI(EditorView::kWidth, EditorView::kHeight),
          plugin_(*static_cast<SQ8LPlugin*>(getPluginInstancePointer())),
          host_(plugin_),
          view_(std::make_unique<EditorView>()),
          frame_(EditorView::kWidth, EditorView::kHeight) {
        // Resizable from the original's size up, aspect ratio kept (OPTIONS -> Zoom or the
        // host's window handle); opens at the size last chosen ([port] zoom in SQ8L.ini).
        const double scale = getScaleFactor();
        setGeometryConstraints(static_cast<uint>(EditorView::kWidth * scale),
                               static_cast<uint>(EditorView::kHeight * scale), true, false);
        zoom_ = plugin_.settings().zoomPercent();
        setSize(static_cast<uint>(EditorView::kWidth * scale * zoom_ / 100.0 + 0.5),
                static_cast<uint>(EditorView::kHeight * scale * zoom_ / 100.0 + 0.5));
        host_.currentZoom = [this] { return zoom_; };
        host_.applyZoom = [this](int percent) { setZoom(percent); };
        rgb_.resize(static_cast<size_t>(EditorView::kWidth) * EditorView::kHeight * 3);
        view_->setTextRenderer(&text_);

        auto releaseHook = [this] {  // a native or drawn modal loop starts
            ++modalLoops_;
            releaseEngine();
        };
        auto acquireHook = [this] { acquireEngine(); };
        auto pumpHook = [this] {
            if (!controller_) return;
            deliverNotifications();
            controller_->pump();
        };
        auto nameFocusHook = [this](bool focused, const std::string& text) {
            Engine lock(*this);
            // The original's name box is a Windows EDIT control: after editing it keeps
            // showing the typed text (here the box is drawn from NameEdit::text).
            if (!focused) view_->progNameEdit().setText(text);
            if (controller_) controller_->nameEditFocus(focused, text);
            repaint();
        };
        auto nameKeyHook = [this](int key) {
            Engine lock(*this);
            if (controller_) controller_->nameEditKeyDown(key);
        };
#if SQ8L_NATIVE_UI
        NativePlatform::Hooks hooks;
        hooks.releaseEngine = releaseHook;
        hooks.acquireEngine = acquireHook;
        hooks.pump = pumpHook;
        hooks.nameFocus = nameFocusHook;
        hooks.nameKey = nameKeyHook;
        native_ = std::make_unique<NativePlatform>(reinterpret_cast<void*>(getWindow().getNativeWindowHandle()), hooks);
#endif
#if defined(__APPLE__) || !SQ8L_NATIVE_UI
        if (!SQ8L_NATIVE_UI || std::getenv("SQ8L_DRAWN_UI")) {
            sq8l::gui::PlatformUiDrawn::Hooks dh;
            dh.releaseEngine = releaseHook;
            dh.acquireEngine = acquireHook;
            dh.pump = pumpHook;
            dh.nameFocus = nameFocusHook;
            dh.nameKey = nameKeyHook;
            dh.runLoopStep = [this] { return nestedLoopStep(); };
            dh.repaint = [this] { repaint(); };
#if SQ8L_NATIVE_UI
            dh.fileDialog = [this](const sq8l::gui::FileDialogRequest& r, std::string& path) {
                return native_->fileDialog(r, path);
            };
            dh.cursorPos = [this](sq8l::gui::Point& p) {
                p = native_->cursorPos();
                return true;
            };
            dh.setCursorPos = [this](sq8l::gui::Point p) { native_->setCursorPos(p); };
#else
            dh.fileDialog = [this](const sq8l::gui::FileDialogRequest& r, std::string& path) {
                return fileBrowser(r, path);
            };
            dh.cursorPos = [this](sq8l::gui::Point& p) {
                int wx, wy;
                if (!sq8l::x11::queryPointer(getWindow().getNativeWindowHandle(), wx, wy)) return false;
                toForm(wx, wy, p.x, p.y);
                return true;
            };
            dh.setCursorPos = [this](sq8l::gui::Point p) {
                sq8l::x11::warpPointer(getWindow().getNativeWindowHandle(),
                                       static_cast<int>(p.x * static_cast<double>(getWidth()) / EditorView::kWidth),
                                       static_cast<int>(p.y * static_cast<double>(getHeight()) / EditorView::kHeight));
            };
#endif
            drawn_ = std::make_unique<sq8l::gui::PlatformUiDrawn>(text_, dh);
        }
#endif
        Engine lock(*this);
        controller_ = std::make_unique<sq8l::gui::EditorController>(*view_, host_, platform(), gEditorsOpened++ == 0);
        host_.setController(controller_.get());
        view_->onContextMenu = [this](sq8l::gui::Control&, int x, int y) { controller_->contextMenu(x, y); };
        controller_->show();
        // The LEDs size themselves on their first paint, which mouse hit-testing depends on.
        view_->render(frame_);
        opening_ = Opening::hostsTurn;  // see askHostForSize
    }

    ~SQ8LUI() override {
        // Debugging aid: SQ8L_UI_DUMP=/path/frame.ppm saves the last editor frame.
        if (const char* dump = std::getenv("SQ8L_UI_DUMP")) {
            if (FILE* f = std::fopen(dump, "wb")) {
                std::fprintf(f, "P6\n%d %d\n255\n", EditorView::kWidth, EditorView::kHeight);
                std::fwrite(rgb_.data(), 1, rgb_.size(), f);
                std::fclose(f);
            }
        }
        {
            Engine lock(*this);
            host_.setController(nullptr);
            controller_.reset();
        }
        if (zoom_ != plugin_.settings().zoom) {  // dragged to a new size: remember it
            plugin_.settings().zoom = zoom_;
            sq8l::SharedLibrary::saveSettings();
        }
        if (texture_) glDeleteTextures(1, &texture_);
    }

protected:
    void parameterChanged(uint32_t, float) override {}

    void uiIdle() override {
        if (opening_ != Opening::open) {  // nothing resized us, or the request was dropped
            askHostForSize();
            opening_ = Opening::open;  // the window is up: a resize is the user's from here on
        }
        if (drawn_ && drawn_->modal()) {  // (a host timer inside a nested loop: the dialog runs)
            repaint();
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - lastTick_ < std::chrono::milliseconds(20)) return;
        int ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTick_).count());
        lastTick_ = now;
        {
            Engine lock(*this);
            deliverNotifications();
            controller_->idle(ms > 200 ? 200 : ms);
        }
        repaint();
    }

    void onDisplay() override {
        {
            Engine lock(*this);
            view_->render(frame_);
            hdRenderer_.capture(*view_);
        }
        const int W = static_cast<int>(getWidth()), H = static_cast<int>(getHeight());
        if (!(drawn_ && drawn_->hasOverlay()) && displayHd(W, H)) return;
        hdValid_ = false;
        if (drawn_ && drawn_->hasOverlay()) {  // drawn menus and dialogs over the editor
            overlay_ = frame_;
            drawn_->render(overlay_);
            overlay_.toRGB24(rgb_.data());
        } else {
            frame_.toRGB24(rgb_.data());
        }
        if (!texture_) createTexture();
        // Debugging aid: SQ8L_UI_FRAME=/path/frame.ppm keeps the last frame shown (with overlays).
        saveFrame(EditorView::kWidth, EditorView::kHeight, rgb_);
        // Sharp scaling: the frame is enlarged with nearest neighbour to the integer multiple k
        // that covers the window, then filtered down to the window size. Pixels stay crisp
        // and even at any size; at an exact multiple the frame is drawn with nearest only.
        int k = static_cast<int>(std::ceil(std::max(W / double(EditorView::kWidth), H / double(EditorView::kHeight)) - 1e-6));
        k = std::max(1, std::min(k, 6));
        const bool exact = W == EditorView::kWidth * k && H == EditorView::kHeight * k;
        const int texK = exact ? 1 : k;
        const bool changed = rgb_ != shown_;
        if (changed) shown_ = rgb_;
        glBindTexture(GL_TEXTURE_2D, texture_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (changed || texK != textureK_) {
            const uint8_t* src = rgb_.data();
            if (texK > 1) {
                upscale(rgb_, texK, scaled_);
                src = scaled_.data();
            }
            const GLint filter = texK > 1 ? GL_LINEAR : GL_NEAREST;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, EditorView::kWidth * texK, EditorView::kHeight * texK, 0, GL_RGB,
                         GL_UNSIGNED_BYTE, src);
            textureK_ = texK;
        }
        drawTexture(W, H);
    }

    // HD graphics (src/gui/hd): the editor drawn at the window's size, redrawn where the
    // classic frame changed and uploaded 1:1. False when the window is larger than the
    // largest texture (the classic frame is shown then).
    bool displayHd(int W, int H) {
        using sq8l::gui::HdRenderer;
        using sq8l::gui::Rect;
        if (!maxTexture_) glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexture_);
        const double S = W / double(EditorView::kWidth);
        const int w = static_cast<int>(std::lround(EditorView::kWidth * S));
        const int h = static_cast<int>(std::lround(EditorView::kHeight * S));
        if (w > maxTexture_ || h > maxTexture_) return false;
        if (!texture_) createTexture();
        const bool full = !hdValid_ || hdOut_.width() != w || hdOut_.height() != h;
        const Rect dirty =
            full ? Rect{0, 0, EditorView::kWidth, EditorView::kHeight} : HdRenderer::changedArea(hdClassic_, frame_);
        glBindTexture(GL_TEXTURE_2D, texture_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (!dirty.empty()) {
            const Rect r = hdRenderer_.render(S, hdOut_, dirty);
            hdClassic_ = frame_;
            if (full) {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                hdRgb_.resize(static_cast<size_t>(w) * h * 3);
                hdOut_.toRGB24(hdRgb_.data());
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, hdRgb_.data());
                hdValid_ = true;
                textureK_ = 0;  // (the classic frame is uploaded again when it is shown)
                shown_.clear();
            } else if (!r.empty()) {
                hdRgb_.resize(static_cast<size_t>(r.width()) * r.height() * 3);
                uint8_t* o = hdRgb_.data();
                for (int y = r.top; y < r.bottom; y++)
                    for (const sq8l::gui::Color* p = hdOut_.row(y) + r.left, *e = p + r.width(); p < e; p++) {
                        *o++ = static_cast<uint8_t>(*p >> 16);
                        *o++ = static_cast<uint8_t>(*p >> 8);
                        *o++ = static_cast<uint8_t>(*p);
                    }
                glTexSubImage2D(GL_TEXTURE_2D, 0, r.left, r.top, r.width(), r.height(), GL_RGB, GL_UNSIGNED_BYTE,
                                hdRgb_.data());
            }
        }
        if (std::getenv("SQ8L_UI_FRAME")) {  // (the debugging aid above, at the window's size)
            hdRgb_.resize(static_cast<size_t>(w) * h * 3);
            hdOut_.toRGB24(hdRgb_.data());
            saveFrame(w, h, hdRgb_);
        }
        drawTexture(W, H);
        return true;
    }

    static void saveFrame(int w, int h, const std::vector<uint8_t>& rgb) {
        if (const char* path = std::getenv("SQ8L_UI_FRAME")) {
            if (FILE* f = std::fopen(path, "wb")) {
                std::fprintf(f, "P6\n%d %d\n255\n", w, h);
                std::fwrite(rgb.data(), 1, rgb.size(), f);
                std::fclose(f);
            }
        }
    }

    void createTexture() {
        glGenTextures(1, &texture_);
        glBindTexture(GL_TEXTURE_2D, texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    // The texture over the whole window.
    void drawTexture(int W, int H) {
        const float w = static_cast<float>(W), h = static_cast<float>(H);
        glEnable(GL_TEXTURE_2D);
        glColor4f(1, 1, 1, 1);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(0, 0);
        glTexCoord2f(1, 0); glVertex2f(w, 0);
        glTexCoord2f(1, 1); glVertex2f(w, h);
        glTexCoord2f(0, 1); glVertex2f(0, h);
        glEnd();
        glDisable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // The window was resized (OPTIONS -> Zoom, or the host's handle): note the zoom.
    void onResize(const ResizeEvent& ev) override {
        UI::onResize(ev);
        // The host sizing the window it had settled on before the editor existed (VST3 in
        // REAPER forces its own rect in postInit): ask for ours right back. This runs before
        // the host shows the window, so the editor never appears at the wrong size.
        if (opening_ == Opening::hostsTurn) {
            opening_ = Opening::asked;
            askHostForSize();
        }
        // Startup churn, never a zoom the user chose -- and one resize arrives as several
        // events (WM_SIZE, WM_WINDOWPOSCHANGED and WM_SHOWWINDOW all configure the view), so
        // this has to hold until the window is up. Taking any of them for the zoom would
        // shrink the editor and persist the wrong size at close.
        if (opening_ != Opening::open) return;
        const double base = EditorView::kWidth * getScaleFactor();
        const int z = std::max(sq8l::Settings::kMinZoom,
                               std::min(sq8l::Settings::kMaxZoom, static_cast<int>(ev.size.getWidth() * 100.0 / base + 0.5)));
        if (z == zoom_) return;
        zoom_ = z;
        if (controller_) {
            Engine lock(*this);
            controller_->post(sq8l::gui::kMsgNotify, 0, 0x30);  // the OPTIONS -> Zoom checks
        }
    }

    bool onMouse(const MouseEvent& ev) override {
        int x, y;
        toForm(ev.pos.getX(), ev.pos.getY(), x, y);
        const MouseButton b = ev.button == kMouseButtonRight ? MouseButton::Right
                              : ev.button == kMouseButtonMiddle ? MouseButton::Middle
                                                                : MouseButton::Left;
        const uint32_t keys = keysOf(ev.mod);
        if (std::getenv("SQ8L_UI_DEBUG"))
            std::fprintf(stderr, "[sq8l-ui] mouse %s button %u at form %d,%d\n", ev.press ? "down" : "up", ev.button, x, y);
        // Windows double click: same button within 500 ms and a few pixels.
        const bool dbl = ev.press && b == lastButton_ && ev.time - lastTime_ <= 500 && std::abs(x - lastX_) <= 2 &&
                         std::abs(y - lastY_) <= 2;
        if (drawn_ && drawn_->modal()) {  // an open drawn menu or dialog takes the input
            if (ev.press) {
                drawn_->mouseDown(b, x, y, dbl);
                lastTime_ = dbl ? 0 : ev.time;
                lastButton_ = b, lastX_ = x, lastY_ = y;
            } else {
                drawn_->mouseUp(b, x, y);
            }
            repaint();
            return true;
        }
        {
            Engine lock(*this);
            if (ev.press) {
                buttons_ |= bit(b);
                // The program name box is an edit control: a left click focuses it.
                const sq8l::gui::NameEdit& edit = view_->progNameEdit();
                const auto r = edit.bounds();
                const bool inEdit = x >= r.left && x < r.right && y >= r.top && y < r.bottom;
                if (b == MouseButton::Left && inEdit) {
                    if (!nameEditing()) beginNameEdit(r.left, r.top, r.width(), r.height(), edit.text);
                    else if (drawn_) drawn_->mouseDown(b, x, y, false);  // move the caret
                    repaint();
                    return true;
                }
                if (nameEditing()) focusName();
                const int loops = modalLoops_;
                if (dbl) {
                    view_->mouseDoubleClick(b, x, y, keys | buttons_);
                    lastTime_ = 0;
                } else {
                    view_->mouseDown(b, x, y, keys | buttons_);
                    lastTime_ = ev.time;
                }
                lastButton_ = b;
                lastX_ = x;
                lastY_ = y;
                if (modalLoops_ != loops) {  // a menu/dialog opened by this press took the mouse
                    view_->cancelMouseMode();
                    buttons_ &= ~bit(b);
                }
            } else {
                buttons_ &= ~bit(b);
                view_->mouseUp(b, x, y, keys | buttons_);
            }
            deliverNotifications();
            controller_->pump();
        }
        repaint();
        return true;
    }

    bool onMotion(const MotionEvent& ev) override {
        int x, y;
        toForm(ev.pos.getX(), ev.pos.getY(), x, y);
        if (drawn_) {
            drawn_->mouseMove(x, y);
            if (drawn_->modal()) return true;
        }
        {
            Engine lock(*this);
            view_->mouseMove(x, y, keysOf(ev.mod) | buttons_);
            controller_->pump();
        }
        repaint();
        return true;
    }

    bool onScroll(const ScrollEvent& ev) override {
        if (!drawn_ || !drawn_->modal()) return false;
        int x, y;
        toForm(ev.pos.getX(), ev.pos.getY(), x, y);
        drawn_->wheel(x, y, ev.delta.getY() > 0 ? 1 : -1);
        return true;
    }

    // Keyboard for the drawn menus, dialogs and name box (the native ones get it themselves).
    bool onKeyboard(const KeyboardEvent& ev) override {
        if (std::getenv("SQ8L_UI_DEBUG"))
            std::fprintf(stderr, "[sq8l-ui] key %s %#x (keycode %u) modal %d name %d\n", ev.press ? "down" : "up", ev.key,
                         ev.keycode, drawn_ && drawn_->modal(), drawn_ && drawn_->nameEditing());
        if (!drawn_ || !drawn_->wantsKeyboard() || !ev.press) return false;
        int vk = 0;
        switch (ev.key) {
        case kKeyBackspace: vk = sq8l::gui::kVkBack; break;
        case kKeyEnter: vk = sq8l::gui::kVkReturn; break;
        case kKeyEscape: vk = sq8l::gui::kVkEscape; break;
        case kKeyDelete: vk = sq8l::gui::kVkDelete; break;
        case kKeyLeft: vk = sq8l::gui::kVkLeft; break;
        case kKeyRight: vk = sq8l::gui::kVkRight; break;
        case kKeyUp: vk = sq8l::gui::kVkUp; break;
        case kKeyDown: vk = sq8l::gui::kVkDown; break;
        case kKeyPageUp: vk = sq8l::gui::kVkPrior; break;
        case kKeyPageDown: vk = sq8l::gui::kVkNext; break;
        case kKeyHome: vk = sq8l::gui::kVkHome; break;
        case kKeyEnd: vk = sq8l::gui::kVkEnd; break;
        default:
            // printable keys: text for the name box (onCharacterInput), "any key" for dialogs
            if (!drawn_->modal()) return false;
            vk = ev.key >= 'a' && ev.key <= 'z' ? static_cast<int>(ev.key - 'a' + 'A') : static_cast<int>(ev.key);
            break;
        }
        drawn_->keyDown(vk);
        repaint();
        return true;
    }

    bool onCharacterInput(const CharacterInputEvent& ev) override {
        if (!drawn_ || !drawn_->nameEditing() || drawn_->modal()) return false;
        drawn_->character(ev.character);
        repaint();
        return true;
    }

#if DISTRHO_UI_FILE_BROWSER
    void uiFileBrowserSelected(const char* filename) override {
        fileChosen_ = filename ? filename : "";
        fileDone_ = true;
    }
#endif

private:
    // The constructor's setSize() runs while DPF is still initializing, and there it only
    // resizes our own window -- the host is never told. Hosts settle on a size before the
    // editor exists, from the unzoomed DISTRHO_UI_DEFAULT_* size DPF answers with until
    // then, and we lose the zoom: REAPER's CLAP keeps a window that crops the editor
    // (gui_get_size comes before gui_set_parent), its VST3 pushes the small rect back onto
    // us (on_size before attached, replayed in postInit). So ask again once the request can
    // reach the host: from the first resize the host puts on us, and failing that (CLAP
    // never resizes us, and DPF drops a VST3 request from a host that asked for no size
    // before attaching) from the first idle. Not a moment earlier -- our own setSize above
    // resizes the view, whose events land here before the constructor has even returned,
    // and an ask spent on one of those is an ask the host never hears.
    void askHostForSize() {
        const double scale = getScaleFactor();
        setSize(static_cast<uint>(EditorView::kWidth * scale * zoom_ / 100.0 + 0.5),
                static_cast<uint>(EditorView::kHeight * scale * zoom_ / 100.0 + 0.5));
    }

    // OPTIONS -> Zoom: resize the window (the host may adjust) and remember the choice.
    void setZoom(int percent) {
        percent = std::max(sq8l::Settings::kMinZoom, std::min(sq8l::Settings::kMaxZoom, percent));
        const double scale = getScaleFactor();
        setSize(static_cast<uint>(EditorView::kWidth * scale * percent / 100.0 + 0.5),
                static_cast<uint>(EditorView::kHeight * scale * percent / 100.0 + 0.5));
        zoom_ = percent;
        plugin_.settings().zoom = percent;
        sq8l::SharedLibrary::saveSettings();
    }

    // Nearest-neighbour enlargement of an RGB24 editor frame by an integer factor.
    static void upscale(const std::vector<uint8_t>& src, int k, std::vector<uint8_t>& dst) {
        const int w = EditorView::kWidth, h = EditorView::kHeight, rowBytes = w * k * 3;
        dst.resize(static_cast<size_t>(rowBytes) * h * k);
        for (int y = 0; y < h; y++) {
            uint8_t* row = dst.data() + static_cast<size_t>(y) * k * rowBytes;
            const uint8_t* s = src.data() + static_cast<size_t>(y) * w * 3;
            for (int x = 0; x < w; x++)
                for (int i = 0; i < k; i++) std::memcpy(row + (x * k + i) * 3, s + x * 3, 3);
            for (int i = 1; i < k; i++) std::memcpy(row + static_cast<size_t>(i) * rowBytes, row, rowBytes);
        }
    }

    sq8l::gui::PlatformUi& platform() {
#if SQ8L_NATIVE_UI
        if (!drawn_) return *native_;
#endif
        return *drawn_;
    }
    void beginNameEdit(int l, int t, int w, int h, const std::string& text) {
#if SQ8L_NATIVE_UI
        if (!drawn_) return native_->beginNameEdit(l, t, w, h, text);
#endif
        drawn_->beginNameEdit(l, t, w, h, text);
    }
    bool nameEditing() const {
#if SQ8L_NATIVE_UI
        if (!drawn_) return native_->nameEditing();
#endif
        return drawn_->nameEditing();
    }
    void focusName() { platform().focusForm(); }

    // One step of the nested event loop of the drawn menus and dialogs.
    bool nestedLoopStep() {
#if defined(__APPLE__)
        return sq8l::gui::macRunLoopStep(0.008);
#elif SQ8L_NATIVE_UI
        return false;
#else
        getApp().idle();  // pugl dispatches the window's events (patches/pugl-x11-nested-update.patch)
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        return true;
#endif
    }

#if DISTRHO_UI_FILE_BROWSER
    // DPF's file browser (xdg portal or its X11 dialog), waited for in a nested loop.
    bool fileBrowser(const sq8l::gui::FileDialogRequest& req, std::string& path) {
        std::string name = req.fileName;
        const size_t slash = name.find_last_of("\\/");
        if (slash != std::string::npos) name = name.substr(slash + 1);
        if (req.save && name.empty()) name = "untitled." + req.defaultExt;
        // (the X11 dialog wants the start folder with its trailing slash)
        const std::string dir = req.initialDir.empty() || req.initialDir.back() == '/' ? req.initialDir
                                                                                        : req.initialDir + "/";
        FileBrowserOptions o;
        o.saving = req.save;
        o.defaultName = name.c_str();
        o.startDir = dir.empty() ? nullptr : dir.c_str();
        o.title = req.save ? "SQ8L - Save" : "SQ8L - Open";
        fileDone_ = false;
        fileChosen_.clear();
        if (!openFileBrowser(o)) return false;
        releaseEngine();
        while (!fileDone_ && nestedLoopStep()) {}
        acquireEngine();
        if (fileChosen_.empty()) return false;
        path = fileChosen_;
        // the default extension, like TSaveDialog.DefaultExt
        const size_t base = path.find_last_of('/');
        if (req.save && !req.defaultExt.empty() &&
            path.find('.', base == std::string::npos ? 0 : base + 1) == std::string::npos)
            path += "." + req.defaultExt;
        return true;
    }
#endif

    // Post the master's queued notifications to the editor (engine lock held).
    void deliverNotifications() {
        for (const auto& m : plugin_.takeEditorMessages()) {
            if (std::getenv("SQ8L_UI_DEBUG")) std::fprintf(stderr, "[sq8l-ui] notify %#x %d\n", m.wParam, m.lParam);
            controller_->post(sq8l::gui::kMsgNotify, m.wParam, m.lParam);
        }
    }

    // Engine lock held while the editor logic runs; released around native modal loops.
    struct Engine {
        SQ8LUI& ui;
        explicit Engine(SQ8LUI& u) : ui(u) { ui.acquireEngine(); }
        ~Engine() { ui.releaseEngine(); }
    };
    void acquireEngine() { plugin_.engineMutex().lock(); ++depth_; }
    void releaseEngine() { --depth_; plugin_.engineMutex().unlock(); }

    void toForm(double px, double py, int& x, int& y) const {
        x = static_cast<int>(px * EditorView::kWidth / static_cast<double>(getWidth()));
        y = static_cast<int>(py * EditorView::kHeight / static_cast<double>(getHeight()));
    }

    static uint32_t bit(MouseButton b) {
        return b == MouseButton::Left ? sq8l::gui::MK_LBUTTON_
               : b == MouseButton::Right ? sq8l::gui::MK_RBUTTON_ : sq8l::gui::MK_MBUTTON_;
    }

    static uint32_t keysOf(uint mod) {
        uint32_t k = 0;
        if (mod & kModifierShift) k |= sq8l::gui::MK_SHIFT_;
        if (mod & kModifierControl) k |= sq8l::gui::MK_CONTROL_;
        return k;
    }

    SQ8LPlugin& plugin_;
    PluginEditorHost host_;
    sq8l::gui::StbTextRenderer text_{true};  // antialiased text
    std::unique_ptr<EditorView> view_;
#if SQ8L_NATIVE_UI
    std::unique_ptr<NativePlatform> native_;
#endif
    std::unique_ptr<sq8l::gui::PlatformUiDrawn> drawn_;
    std::unique_ptr<sq8l::gui::EditorController> controller_;
    sq8l::gui::Bitmap frame_;
    sq8l::gui::Bitmap overlay_;
    std::vector<uint8_t> shown_;   // the frame in the texture (skip uploads when unchanged)
    std::vector<uint8_t> scaled_;  // the frame enlarged k times (sharp scaling)
    int textureK_ = 0;
    int zoom_ = 100;               // window size in percent (OPTIONS -> Zoom)
    // How far the window has got towards showing at the stored zoom, see askHostForSize.
    enum class Opening {
        constructing,  // our own setSize is still echoing back as resize events
        hostsTurn,     // whatever the host does to the window now, ask for our size back
        asked,         // asked, waiting for the window to be up
        open           // settled: a resize is the user's, and sets the zoom
    };
    Opening opening_ = Opening::constructing;
    sq8l::gui::HdRenderer hdRenderer_{text_};  // the editor drawn at the window's resolution
    bool hdValid_ = false;           // the texture holds hdOut_
    sq8l::gui::Bitmap hdOut_;        // the editor at the window's size
    sq8l::gui::Bitmap hdClassic_;    // the classic frame hdOut_ was drawn from
    std::vector<uint8_t> hdRgb_;     // upload buffer
    GLint maxTexture_ = 0;
    bool fileDone_ = false;
    std::string fileChosen_;
    std::vector<uint8_t> rgb_;
    GLuint texture_ = 0;
    int depth_ = 0;
    int modalLoops_ = 0;  // modal loops started (menus/dialogs), see onMouse
    uint32_t buttons_ = 0;
    MouseButton lastButton_ = MouseButton::Left;
    uint lastTime_ = 0;
    int lastX_ = 0, lastY_ = 0;
    std::chrono::steady_clock::time_point lastTick_ = std::chrono::steady_clock::now();

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SQ8LUI)
};

UI* createUI() { return new SQ8LUI(); }

END_NAMESPACE_DISTRHO
