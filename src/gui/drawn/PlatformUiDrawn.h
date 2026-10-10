// Platform services drawn inside the editor window (sq8l::gui::PlatformUi), for systems
// without usable native popup menus and dialogs from a plugin (Linux: every toolkit is the
// host's business). Popup menus, message boxes, the WRITE and MIDI port dialogs, the info
// windows and the program name box are drawn over the editor frame and get the window's
// mouse and keyboard input while they are open.
//
// The editor logic expects modal calls that return the user's answer (TrackPopupMenu,
// MessageBox, ShowModal): while a menu or dialog is open, the platform runs a nested event
// loop through Hooks::runLoopStep (the window keeps receiving and painting events) with
// the engine lock released, like the native modal loops on macOS and Windows.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Bitmap.h"
#include "TextRenderer.h"
#include "controls/Control.h"
#include "logic/PlatformUi.h"

namespace sq8l::gui {

// Virtual-key codes used by the drawn controls (Windows values, like the editor logic).
enum : int {
    kVkBack = 0x08, kVkReturn = 0x0d, kVkEscape = 0x1b, kVkPrior = 0x21, kVkNext = 0x22,
    kVkEnd = 0x23, kVkHome = 0x24, kVkLeft = 0x25, kVkUp = 0x26, kVkRight = 0x27, kVkDown = 0x28,
    kVkDelete = 0x2e,
};

class PlatformUiDrawn final : public PlatformUi {
public:
    struct Hooks {
        std::function<void()> releaseEngine;  // around the nested loops
        std::function<void()> acquireEngine;
        std::function<void()> pump;           // EditorController::pump(), under the engine lock
        std::function<void(bool focused, const std::string& text)> nameFocus;  // program name box
        std::function<void(int key)> nameKey;  // VK_RETURN in the name box
        // Process the window's pending events once (they come back through the input
        // functions below) and wait a little; false = the loop can't run (window closing).
        std::function<bool()> runLoopStep;
        std::function<void()> repaint;        // ask the window to repaint
        // Optional platform services; without them: no file dialog, the cursor position is
        // the last one seen and can't be moved.
        std::function<bool(const FileDialogRequest&, std::string&)> fileDialog;
        std::function<bool(Point&)> cursorPos;
        std::function<void(Point)> setCursorPos;
        std::function<void(bool)> setCursorVisible;  // without it the cursor stays visible
    };

    PlatformUiDrawn(TextRenderer& text, Hooks hooks);
    ~PlatformUiDrawn() override;

    // PlatformUi
    int popupMenu(const std::vector<MenuItem>& items, int x, int y) override;
    int messageBox(const std::string& text, const std::string& caption, int flags) override;
    bool fileDialog(const FileDialogRequest& request, std::string& path) override;
    bool readFile(const std::string& path, std::vector<uint8_t>& data) override;
    bool writeFile(const std::string& path, const std::vector<uint8_t>& data) override;
    Point cursorPos() override;
    void setCursorPos(Point p) override;
    void setCursorVisible(bool visible) override;
    void focusForm() override;
    void runModal(ModalDialog& dialog) override;
    void showModInfo(const std::vector<std::string>& lines) override;
    void showAbout(const std::string& text) override;

    // Program name box: edit the text in place over the form rectangle.
    void beginNameEdit(int left, int top, int width, int height, const std::string& text);
    bool nameEditing() const;

    // ---- window side (form coordinates)
    bool modal() const;          // a menu or dialog is open: it takes all mouse and keyboard input
    bool wantsKeyboard() const;  // modal, or the name box is being edited
    bool hasOverlay() const;     // something to draw over the editor
    void render(Bitmap& frame);  // draw it over the editor frame
    void mouseDown(MouseButton b, int x, int y, bool doubleClick);
    void mouseUp(MouseButton b, int x, int y);
    void mouseMove(int x, int y);
    void wheel(int x, int y, int delta);  // delta > 0: up
    void keyDown(int vk);
    void character(uint32_t codepoint);

    // An open menu or dialog (tests and the window use it through the functions above).
    class Layer;

private:
    void runLoop(Layer& layer);
    TextRenderer& text_;
    Hooks hooks_;
    std::vector<Layer*> layers_;  // open menus/dialogs, the top one gets the input
    struct NameBox;
    std::unique_ptr<NameBox> name_;
    Point lastMouse_;
};

}  // namespace sq8l::gui
