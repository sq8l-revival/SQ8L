#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>

#include "PlatformUiWin.h"

#include "DarkMenu.h"

#include <fstream>
#include <iterator>
#include <vector>

#include "logic/Dialogs.h"

namespace sq8l::gui {

namespace {

constexpr int kFormW = 626, kFormH = 430;
constexpr int kCtlList = 100, kCtlOk = 101, kCtlCancel = 102, kCtlCompare = 103, kCtlBank = 104;
const char* const kDialogClass = "SQ8LPortV1Dialog";

void appendMenu(HMENU menu, const std::vector<MenuItem>& items) {
    for (const MenuItem& it : items) {
        if (it.separator) {
            AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        MENUITEMINFOA mii{};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_STRING | MIIM_ID | MIIM_STATE | MIIM_FTYPE;
        mii.fType = MFT_STRING;
        if (it.radio) mii.fType |= MFT_RADIOCHECK;
        if (it.barBreak) mii.fType |= MFT_MENUBARBREAK;
        mii.fState = (it.checked ? MFS_CHECKED : 0) | (it.enabled ? 0 : MFS_GRAYED) | (it.isDefault ? MFS_DEFAULT : 0);
        mii.wID = static_cast<UINT>(it.id);
        mii.dwTypeData = const_cast<char*>(it.text.c_str());
        if (!it.sub.empty()) {
            HMENU sub = CreatePopupMenu();
            appendMenu(sub, it.sub);
            mii.fMask |= MIIM_SUBMENU;
            mii.hSubMenu = sub;
        }
        InsertMenuItemA(menu, GetMenuItemCount(menu), TRUE, &mii);
    }
}

// This DLL's module handle (window classes must belong to the DLL, not the host).
HINSTANCE dllInstance() {
    HMODULE h = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&dllInstance), &h);
    return h;
}

// VCL filter "Desc|*.a;*.b|..." -> "Desc\0*.a;*.b\0...\0\0"
std::string winFilter(const std::string& f) {
    std::string out = f;
    for (char& c : out)
        if (c == '|') c = '\0';
    out.push_back('\0');
    out.push_back('\0');
    return out;
}

}  // namespace

struct PlatformUiWin::Impl {
    HWND hwnd = nullptr;
    Hooks hooks;
    HWND edit = nullptr;
    WNDPROC editProc = nullptr;
    WNDPROC parentProc = nullptr;
    HBRUSH editBrush = nullptr;
    HFONT editFont = nullptr;
    HFONT dialogFont = nullptr;
    DarkMenu darkMenu;

    double scale() const {
        RECT r;
        GetClientRect(hwnd, &r);
        return (r.right - r.left) / static_cast<double>(kFormW);
    }
    POINT screenOfForm(int x, int y) const {
        const double s = scale();
        POINT p{static_cast<LONG>(x * s), static_cast<LONG>(y * s)};
        ClientToScreen(hwnd, &p);
        return p;
    }
    struct Released {
        const Hooks& h;
        explicit Released(const Hooks& hk) : h(hk) { if (h.releaseEngine) h.releaseEngine(); }
        ~Released() { if (h.acquireEngine) h.acquireEngine(); }
    };

    // ------------------------------------------------------------ program name EDIT
    static LRESULT CALLBACK editSubclass(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrA(w, GWLP_USERDATA));
        if (msg == WM_KEYDOWN && wp == VK_RETURN) {
            if (self->hooks.nameKey) self->hooks.nameKey(0x0d);
            SetFocus(self->hwnd);
            return 0;
        }
        if (msg == WM_CHAR && wp == '\r') return 0;
        if (msg == WM_KILLFOCUS) {
            char buf[64] = {};
            GetWindowTextA(w, buf, sizeof buf);
            ShowWindow(w, SW_HIDE);
            if (self->hooks.nameFocus) self->hooks.nameFocus(false, buf);
        }
        return CallWindowProcA(self->editProc, w, msg, wp, lp);
    }
    // The plugin window gets WM_CTLCOLOREDIT for the EDIT (black background like the original)
    // and, while a column menu is up in a dark host, its owner-draw messages.
    static LRESULT CALLBACK parentSubclass(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetPropA(w, "SQ8LPortV1Impl"));
        if (self && msg == WM_MEASUREITEM && self->darkMenu.measure(reinterpret_cast<MEASUREITEMSTRUCT*>(lp)))
            return TRUE;
        if (self && msg == WM_DRAWITEM && self->darkMenu.draw(reinterpret_cast<DRAWITEMSTRUCT*>(lp))) return TRUE;
        if (self && msg == WM_CTLCOLOREDIT && reinterpret_cast<HWND>(lp) == self->edit) {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, RGB(0x78, 0x79, 0x8b));
            SetBkColor(dc, RGB(0, 0, 0));
            return reinterpret_cast<LRESULT>(self->editBrush);
        }
        return CallWindowProcA(self ? self->parentProc : DefWindowProcA, w, msg, wp, lp);
    }

    // ------------------------------------------------------------ modal dialog
    struct DialogState {
        Impl* impl;
        ModalDialog* dialog;
        HWND window = nullptr, list = nullptr, compare = nullptr;
        bool syncing = false;
    };

    static void refresh(DialogState& d) {
        d.syncing = true;
        SendMessageA(d.list, WM_SETREDRAW, FALSE, 0);
        SendMessageA(d.list, LB_RESETCONTENT, 0, 0);
        for (const std::string& s : d.dialog->items) SendMessageA(d.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str()));
        SendMessageA(d.list, LB_SETCURSEL, static_cast<WPARAM>(d.dialog->itemIndex), 0);
        SendMessageA(d.list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(d.list, nullptr, TRUE);
        if (d.compare) {
            auto* sel = dynamic_cast<SelSingleDialog*>(d.dialog);
            SendMessageA(d.compare, BM_SETCHECK, sel && sel->compareChecked ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        d.syncing = false;
    }

    static void act(DialogState& d, const std::function<void()>& f) {
        const Hooks& h = d.impl->hooks;
        if (h.acquireEngine) h.acquireEngine();
        f();
        if (h.pump) h.pump();
        if (h.releaseEngine) h.releaseEngine();
        refresh(d);
    }

    static LRESULT CALLBACK dialogProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
        auto* d = reinterpret_cast<DialogState*>(GetWindowLongPtrA(w, GWLP_USERDATA));
        if (!d) return DefWindowProcA(w, msg, wp, lp);
        ModalDialog* dlg = d->dialog;
        switch (msg) {
        case WM_COMMAND: {
            const int id = LOWORD(wp), code = HIWORD(wp);
            if (id == kCtlList && code == LBN_SELCHANGE && !d->syncing) {
                const int row = static_cast<int>(SendMessageA(d->list, LB_GETCURSEL, 0, 0));
                if (row >= 0) act(*d, [dlg, row] { dlg->clickItem(row); });
            } else if (id == kCtlList && code == LBN_DBLCLK) {
                const int row = static_cast<int>(SendMessageA(d->list, LB_GETCURSEL, 0, 0));
                if (row >= 0) act(*d, [dlg, row] { dlg->dblClickItem(row); });
            } else if (id == kCtlOk || id == IDOK) {
                act(*d, [dlg] { dlg->clickOk(); });
            } else if (id == kCtlCancel || id == IDCANCEL) {
                act(*d, [dlg] { dlg->clickCancel(); });
            } else if (id == kCtlCompare) {
                if (auto* s = dynamic_cast<SelSingleDialog*>(dlg)) act(*d, [s] { s->clickCompare(); });
            } else if (id == kCtlBank) {
                if (auto* s = dynamic_cast<SelSingleDialog*>(dlg)) act(*d, [s] { s->clickBank(); });
            }
            return 0;
        }
        case WM_CLOSE:
            act(*d, [dlg] { dlg->clickCancel(); });
            return 0;
        default:
            return DefWindowProcA(w, msg, wp, lp);
        }
    }

    HWND child(HWND parent, const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int id) {
        HWND c = CreateWindowExA(cls == std::string("LISTBOX") ? WS_EX_CLIENTEDGE : 0, cls, text,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, x, y, w, h, parent,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), dllInstance(), nullptr);
        SendMessageA(c, WM_SETFONT, reinterpret_cast<WPARAM>(dialogFont), TRUE);
        return c;
    }
};

PlatformUiWin::PlatformUiWin(void* window, Hooks hooks) : impl_(std::make_unique<Impl>()) {
    impl_->hwnd = static_cast<HWND>(window);
    impl_->hooks = std::move(hooks);
    impl_->editBrush = CreateSolidBrush(RGB(0, 0, 0));
    impl_->dialogFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    // The window is the owner of the popup menus and of the program name EDIT, so it is where
    // their messages arrive.
    if (impl_->hwnd) {
        SetPropA(impl_->hwnd, "SQ8LPortV1Impl", impl_.get());
        impl_->parentProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrA(impl_->hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Impl::parentSubclass)));
    }
}

PlatformUiWin::~PlatformUiWin() {
    Impl& I = *impl_;
    if (I.parentProc) {
        SetWindowLongPtrA(I.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(I.parentProc));
        RemovePropA(I.hwnd, "SQ8LPortV1Impl");
    }
    if (I.edit) DestroyWindow(I.edit);
    if (I.editFont) DeleteObject(I.editFont);
    if (I.editBrush) DeleteObject(I.editBrush);
}

int PlatformUiWin::popupMenu(const std::vector<MenuItem>& items, int x, int y) {
    HMENU menu = CreatePopupMenu();
    appendMenu(menu, items);
    // Windows does not draw a menu split into columns in the host's dark theme; for those we
    // draw the items ourselves (DarkMenu), which needs our window procedure in place.
    if (impl_->parentProc) impl_->darkMenu.prepare(menu, impl_->hwnd);
    const POINT p = impl_->screenOfForm(x, y);
    int cmd;
    {
        Impl::Released unlocked(impl_->hooks);
        cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, p.x, p.y, 0,
                             impl_->hwnd, nullptr);
    }
    DestroyMenu(menu);
    impl_->darkMenu.reset();
    return cmd;
}

int PlatformUiWin::messageBox(const std::string& text, const std::string& caption, int flags) {
    Impl::Released unlocked(impl_->hooks);
    return MessageBoxA(impl_->hwnd, text.c_str(), caption.c_str(), static_cast<UINT>(flags));
}

bool PlatformUiWin::fileDialog(const FileDialogRequest& req, std::string& path) {
    char file[MAX_PATH] = {};
    if (!req.fileName.empty()) lstrcpynA(file, req.fileName.c_str(), MAX_PATH);
    const std::string filter = winFilter(req.filter);
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = impl_->hwnd;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = req.initialDir.empty() ? nullptr : req.initialDir.c_str();
    ofn.lpstrDefExt = req.defaultExt.empty() ? nullptr : req.defaultExt.c_str();
    ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | (req.save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    BOOL ok;
    {
        Impl::Released unlocked(impl_->hooks);
        ok = req.save ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn);
    }
    if (!ok) return false;
    path = file;
    return true;
}

bool PlatformUiWin::readFile(const std::string& path, std::vector<uint8_t>& data) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    data.assign(std::istreambuf_iterator<char>(f), {});
    return !data.empty();
}

bool PlatformUiWin::writeFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

Point PlatformUiWin::cursorPos() {
    POINT p;
    GetCursorPos(&p);
    ScreenToClient(impl_->hwnd, &p);
    const double s = impl_->scale();
    return {static_cast<int>(p.x / s), static_cast<int>(p.y / s)};
}

void PlatformUiWin::setCursorPos(Point p) {
    const POINT s = impl_->screenOfForm(p.x, p.y);
    SetCursorPos(s.x, s.y);
}

void PlatformUiWin::setCursorVisible(bool visible) {
    // ShowCursor keeps a counter per thread, so the calls have to balance; the editor only
    // ever hides once at a time (EditorController::hideCursor).
    ShowCursor(visible ? TRUE : FALSE);
}

void PlatformUiWin::focusForm() {
    if (impl_->edit && IsWindowVisible(impl_->edit)) SetFocus(impl_->hwnd);
}

void PlatformUiWin::beginNameEdit(int left, int top, int width, int height, const std::string& text) {
    Impl& I = *impl_;
    const double s = I.scale();
    if (!I.edit) {
        I.edit = CreateWindowExA(0, "EDIT", "", WS_CHILD | ES_UPPERCASE | ES_AUTOHSCROLL, 0, 0, 10, 10, I.hwnd, nullptr,
                                 dllInstance(), nullptr);
        SetWindowLongPtrA(I.edit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&I));
        I.editProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrA(I.edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Impl::editSubclass)));
        SendMessageA(I.edit, EM_LIMITTEXT, 15, 0);
        I.editFont = CreateFontA(-static_cast<int>(11 * s), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Arial");
        SendMessageA(I.edit, WM_SETFONT, reinterpret_cast<WPARAM>(I.editFont), TRUE);
    }
    SetWindowTextA(I.edit, text.c_str());
    MoveWindow(I.edit, static_cast<int>(left * s), static_cast<int>(top * s), static_cast<int>(width * s),
               static_cast<int>(height * s), TRUE);
    ShowWindow(I.edit, SW_SHOW);
    SetFocus(I.edit);
    SendMessageA(I.edit, EM_SETSEL, 0, -1);
    if (I.hooks.nameFocus) I.hooks.nameFocus(true, text);
}

bool PlatformUiWin::nameEditing() const { return impl_->edit && IsWindowVisible(impl_->edit); }

void PlatformUiWin::runModal(ModalDialog& dialog) {
    Impl& I = *impl_;
    const bool sel = dialog.kind() == ModalDialog::Kind::SelectProgram;
    // TSelSingleForm 196x352 / TmidiSelForm 194x314 client areas (original layout)
    const int W = sel ? 196 : 194, H = sel ? 352 : 314;
    RECT r{0, 0, W, H};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_DLGMODALFRAME);
    const POINT c = I.screenOfForm(kDialogCenterX, kDialogCenterY);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    Impl::DialogState d{&I, &dialog};
    HWND owner = GetAncestor(I.hwnd, GA_ROOT);
    // Registered for the dialog's lifetime only, with this DLL's instance (safe if the host
    // unloads and reloads the plugin).
    WNDCLASSA wc{};
    wc.lpfnWndProc = &Impl::dialogProc;
    wc.hInstance = dllInstance();
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kDialogClass;
    RegisterClassA(&wc);  // may already exist while another instance shows its dialog
    d.window = CreateWindowExA(WS_EX_DLGMODALFRAME, kDialogClass, dialog.caption.c_str(), style, c.x - ww / 2,
                               c.y - wh / 2, ww, wh, owner, nullptr, dllInstance(), nullptr);
    SetWindowLongPtrA(d.window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&d));
    const int listTop = sel ? 45 : 8, btnTop = sel ? 319 : 280;
    d.list = I.child(d.window, "LISTBOX", "", LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT, 8, listTop, 179, 265, kCtlList);
    I.child(d.window, "BUTTON", dialog.okCaption.c_str(), BS_DEFPUSHBUTTON, 8, btnTop, 82, 25, kCtlOk);
    I.child(d.window, "BUTTON", dialog.cancelCaption.c_str(), BS_PUSHBUTTON, 104, btnTop, 82, 25, kCtlCancel);
    if (sel) {
        d.compare = I.child(d.window, "BUTTON", "COMPARE", BS_CHECKBOX, 8, 8, 73, 17, kCtlCompare);
        I.child(d.window, "BUTTON", "Bank", BS_PUSHBUTTON, 112, 8, 75, 25, kCtlBank);
    }
    Impl::refresh(d);
    ShowWindow(d.window, SW_SHOW);
    EnableWindow(owner, FALSE);
    {
        Impl::Released unlocked(I.hooks);
        MSG m;
        while (dialog.modalResult == 0 && GetMessageA(&m, nullptr, 0, 0) > 0) {
            if (IsDialogMessageA(d.window, &m)) continue;
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
    }
    EnableWindow(owner, TRUE);
    SetWindowLongPtrA(d.window, GWLP_USERDATA, 0);
    DestroyWindow(d.window);
    UnregisterClassA(kDialogClass, dllInstance());  // fails harmlessly if still in use
    SetActiveWindow(owner);
}

void PlatformUiWin::showModInfo(const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& l : lines) text += l + "\r\n";
    Impl::Released unlocked(impl_->hooks);
    MessageBoxA(impl_->hwnd, text.c_str(), "Modulation sources", MB_OK);
}

void PlatformUiWin::showAbout(const std::string& text) {
    Impl::Released unlocked(impl_->hooks);
    MessageBoxA(impl_->hwnd, text.c_str(), "SQ8L", MB_OK);
}

}  // namespace sq8l::gui
