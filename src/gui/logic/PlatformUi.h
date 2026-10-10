// Platform services needed by the editor logic (layer 4 of docs/GUI_ARCHITECTURE.md): popup
// menus, message boxes, file dialogs and file access, the modal dialogs, the mouse cursor and the
// MIDI ports used by SEND/REQ. The DPF UI implements it; tests implement it with scripted answers.
//
// Coordinates are form client coordinates of the editor window (626 x 430); the implementation
// converts them to screen coordinates where needed (the original uses screen coordinates for the
// cursor and for TPopupMenu.Popup, with the form at its own screen position).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sq8l::gui {

// One entry of a popup menu, in the shape the VCL creates for TrackPopupMenu (and the oracle
// captures in oracle/gui.py menu_tree): caption (with '&' hotkey markers where the VCL adds them),
// flags and sub-menu.
struct MenuItem {
    std::string text;
    int id = 0;               // logic-defined command id (returned by popupMenu when chosen)
    bool checked = false;     // MFS_CHECKED
    bool enabled = true;      // !MFS_DISABLED/MFS_GRAYED
    bool separator = false;   // MFT_SEPARATOR ("-")
    bool radio = false;       // MFT_RADIOCHECK (TMenuItem.RadioItem)
    bool barBreak = false;    // MFT_MENUBARBREAK (TMenuItem.Break = mbBarBreak: new column)
    bool isDefault = false;   // MFS_DEFAULT (TMenuItem.Default, drawn bold)
    std::vector<MenuItem> sub;
};

// Application.MessageBox flags / results (Windows values).
enum : int {
    kMbOk = 0, kMbOkCancel = 1, kMbYesNo = 4,
    kIdOk = 1, kIdCancel = 2, kIdYes = 6, kIdNo = 7,
};

struct FileDialogRequest {
    bool save = false;        // TSaveDialog (form +0x300) / TOpenDialog (+0x2fc)
    std::string initialDir;   // InitialDir: the plugin folder (CplugConfig +0x1c) without its trailing backslash (TOpenDialog.SetInitialDir)
    std::string filter;       // e.g. "Library / Backup data (*.8XL; *.dat)|*.8XL;*.dat"
    std::string defaultExt;   // DefaultExt ("8XL", "SYX")
    // FileName shown in the dialog: the dialog object keeps the last chosen file; the program
    // export sets it to InitialDir + '\\' + the sanitized program name (FUN_00417798).
    std::string fileName;
};

struct Point {
    int x = 0, y = 0;
};

class PlatformUi {
public:
    virtual ~PlatformUi() = default;

    // TPopupMenu.Popup at form position (x, y). Returns the id of the chosen item, 0 if the menu
    // was dismissed. The logic dispatches the choice like the VCL (on the next message pump).
    virtual int popupMenu(const std::vector<MenuItem>& items, int x, int y) = 0;

    // Application.MessageBox(text, caption, flags) -> kIdOk / kIdCancel / kIdYes / kIdNo.
    virtual int messageBox(const std::string& text, const std::string& caption, int flags) = 0;

    // TOpenDialog / TSaveDialog.Execute: false if cancelled; `path` is the chosen file.
    virtual bool fileDialog(const FileDialogRequest& request, std::string& path) = 0;
    // Whole file read (FUN_004173d0) / write (FUN_004174c8). read: false if the file can't be
    // read or is empty; write: false on error.
    virtual bool readFile(const std::string& path, std::vector<uint8_t>& data) = 0;
    virtual bool writeFile(const std::string& path, const std::vector<uint8_t>& data) = 0;

    // Mouse cursor in form coordinates (Mouse.CursorPos), used by the "jumping mouse".
    virtual Point cursorPos() = 0;
    virtual void setCursorPos(Point p) = 0;
    // (port) Hide / show the cursor over the editor while a knob is being turned, the modern
    // form of the original's "mouse position restored after knob turning" (issue #25). Calls
    // are balanced by the caller, never nested. Platforms without it simply keep the cursor.
    virtual void setCursorVisible(bool /*visible*/) {}

    // Keyboard focus moved to the form (FUN_00483a3c: ends program name editing).
    virtual void focusForm() {}

    // Modal dialogs. The dialog logic lives in Dialogs.h; the platform shows it and forwards
    // the user's actions to it until modalResult != 0 (see SelSingleDialog / MidiSelDialog).
    // While the dialog is open the original keeps dispatching the editor's posted messages
    // (Application.HandleMessage): call EditorController::pump() between the user actions.
    // Closing the window otherwise (title bar, Alt+F4) = clickCancel().
    // All dialogs are centred on form point (kDialogCenterX, kDialogCenterY) (Dialogs.h).
    virtual void runModal(class ModalDialog& dialog) = 0;

    // TModInfoForm / TAboutForm: also shown modally (ShowModal) and closed by a click (both)
    // or a key (ModInfo); nothing is returned to the editor.
    //   showModInfo: the lines of the TMemo (FUN_0047cb80);
    //   showAbout:   the TMemo text (DFM lines, each ending with CR LF), centred, no border.
    virtual void showModInfo(const std::vector<std::string>& lines) = 0;
    virtual void showAbout(const std::string& text) = 0;
};

}  // namespace sq8l::gui
