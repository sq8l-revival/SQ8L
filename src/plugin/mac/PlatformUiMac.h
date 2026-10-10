// macOS implementation of the editor's platform services (sq8l::gui::PlatformUi):
// native NSMenu popups, NSAlert message boxes, NSOpen/SavePanel, modal NSPanels for the
// WRITE and MIDI port dialogs (laid out like the original forms), cursor warping, and an
// NSTextField overlay for the program name box.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "logic/PlatformUi.h"

namespace sq8l::gui {

class PlatformUiMac final : public PlatformUi {
public:
    struct Hooks {
        std::function<void()> releaseEngine;   // before a native modal loop
        std::function<void()> acquireEngine;   // after it
        std::function<void()> pump;            // EditorController::pump(), called under the engine lock
        std::function<void(bool focused, const std::string& text)> nameFocus;  // name box focus changes
        std::function<void(int key)> nameKey;  // VK code (Enter) in the name box
    };

    // view: the plugin's NSView (DPF getNativeWindowHandle()).
    PlatformUiMac(void* view, Hooks hooks);
    ~PlatformUiMac() override;

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

    // Program name box: show the native text field over the form rectangle (form coordinates).
    void beginNameEdit(int left, int top, int width, int height, const std::string& text);
    bool nameEditing() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// One step of a nested event loop (the drawn menus and dialogs, SQ8L_DRAWN_UI=1 for
// testing them on macOS): dispatch the next window event, waiting at most `seconds`.
bool macRunLoopStep(double seconds);

}  // namespace sq8l::gui
