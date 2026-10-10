// Windows implementation of the editor's platform services (sq8l::gui::PlatformUi), with
// the same native Win32 building blocks the original used: TrackPopupMenu, MessageBox,
// GetOpen/SaveFileName, a modal window with LISTBOX/BUTTON controls laid out like the
// original forms, SetCursorPos, and an EDIT control for the program name box.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "logic/PlatformUi.h"

namespace sq8l::gui {

class PlatformUiWin final : public PlatformUi {
public:
    struct Hooks {
        std::function<void()> releaseEngine;
        std::function<void()> acquireEngine;
        std::function<void()> pump;
        std::function<void(bool focused, const std::string& text)> nameFocus;
        std::function<void(int key)> nameKey;
    };

    // window: the plugin's HWND (DPF getNativeWindowHandle()).
    PlatformUiWin(void* window, Hooks hooks);
    ~PlatformUiWin() override;

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

    void beginNameEdit(int left, int top, int width, int height, const std::string& text);
    bool nameEditing() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace sq8l::gui
