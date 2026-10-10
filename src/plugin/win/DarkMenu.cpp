#include "DarkMenu.h"

#include <vssym32.h>

#include <algorithm>

namespace sq8l::gui {

namespace {

// The colour a theme part fills with, for the places where we need a brush rather than a
// drawing call (the menu background; MENU_POPUPBACKGROUND has no TMT_FILLCOLOR).
COLORREF themeFillColor(HTHEME theme, HDC reference, int part, int state) {
    HDC dc = CreateCompatibleDC(reference);
    if (!dc) return CLR_INVALID;
    COLORREF colour = CLR_INVALID;
    if (HBITMAP bmp = CreateCompatibleBitmap(reference, 8, 8)) {
        HGDIOBJ old = SelectObject(dc, bmp);
        RECT rc{0, 0, 8, 8};
        if (SUCCEEDED(DrawThemeBackground(theme, dc, part, state, &rc, nullptr)))
            colour = GetPixel(dc, 4, 4);
        SelectObject(dc, old);
        DeleteObject(bmp);
    }
    DeleteDC(dc);
    return colour;
}

// The colour of the line a theme part draws, for parts we have to draw by hand (the vertical
// divider between the columns). Rendered over `over` so that a transparent part reads back as
// the background rather than as black.
COLORREF themeLineColor(HTHEME theme, HDC reference, int part, int state, SIZE size, COLORREF over) {
    if (size.cx <= 0 || size.cy <= 0) return CLR_INVALID;
    HDC dc = CreateCompatibleDC(reference);
    if (!dc) return CLR_INVALID;
    COLORREF colour = CLR_INVALID;
    if (HBITMAP bmp = CreateCompatibleBitmap(reference, 8, size.cy)) {
        HGDIOBJ old = SelectObject(dc, bmp);
        RECT rc{0, 0, 8, size.cy};
        if (HBRUSH brush = CreateSolidBrush(over)) {
            FillRect(dc, &rc, brush);
            DeleteObject(brush);
        }
        if (SUCCEEDED(DrawThemeBackground(theme, dc, part, state, &rc, nullptr))) {
            // The line sits somewhere inside the part's height; take the row that differs
            // most from the background.
            long best = -1;
            for (int y = 0; y < size.cy; y++) {
                const COLORREF c = GetPixel(dc, 4, y);
                const long distance = labs(long(GetRValue(c)) - GetRValue(over)) +
                                      labs(long(GetGValue(c)) - GetGValue(over)) +
                                      labs(long(GetBValue(c)) - GetBValue(over));
                if (distance > best) {
                    best = distance;
                    colour = c;
                }
            }
        }
        SelectObject(dc, old);
        DeleteObject(bmp);
    }
    DeleteDC(dc);
    return colour;
}

bool hasColumnBreak(HMENU menu) {
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; i++) {
        MENUITEMINFOW mii{};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_FTYPE | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, UINT(i), TRUE, &mii)) continue;
        if (mii.fType & (MFT_MENUBREAK | MFT_MENUBARBREAK)) return true;
        if (mii.hSubMenu && hasColumnBreak(mii.hSubMenu)) return true;
    }
    return false;
}

std::wstring itemText(HMENU menu, int index) {
    MENUITEMINFOW mii{};
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_STRING;
    if (!GetMenuItemInfoW(menu, UINT(index), TRUE, &mii) || mii.cch == 0) return {};
    std::wstring text(mii.cch + 1, L'\0');
    mii.dwTypeData = text.data();
    mii.cch++;
    if (!GetMenuItemInfoW(menu, UINT(index), TRUE, &mii)) return {};
    text.resize(wcslen(text.c_str()));
    return text;
}

}  // namespace

bool DarkMenu::menusAreDark() {
    // With a null window OpenThemeData resolves the class the way USER32 does for its menus:
    // it follows the preferred app mode the host set (SetPreferredAppMode), not just the
    // system setting, so this is what the plugin's other menus are actually drawn with.
    HTHEME theme = OpenThemeData(nullptr, L"Menu");
    if (!theme) return false;
    COLORREF text = 0;
    const bool ok = SUCCEEDED(GetThemeColor(theme, MENU_POPUPITEM, MPI_NORMAL, TMT_TEXTCOLOR, &text));
    CloseThemeData(theme);
    if (!ok) return false;
    // Light text means a dark menu.
    return (GetRValue(text) + GetGValue(text) + GetBValue(text)) / 3 > 128;
}

bool DarkMenu::prepare(HMENU menu, HWND owner) {
    reset();
    if (!menu || !owner || !hasColumnBreak(menu) || !menusAreDark()) return false;

    theme_ = OpenThemeData(nullptr, L"Menu");
    if (!theme_) return false;
    owner_ = owner;

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        font_ = CreateFontIndirectW(&ncm.lfMenuFont);
        ncm.lfMenuFont.lfWeight = FW_BOLD;
        boldFont_ = CreateFontIndirectW(&ncm.lfMenuFont);
    }

    HDC dc = GetDC(owner);
    const COLORREF background = themeFillColor(theme_, dc, MENU_POPUPBACKGROUND, 0);
    GetThemePartSize(theme_, dc, MENU_POPUPCHECK, MC_CHECKMARKNORMAL, nullptr, TS_TRUE, &metrics_.check);
    GetThemePartSize(theme_, dc, MENU_POPUPSEPARATOR, 0, nullptr, TS_TRUE, &metrics_.separator);
    GetThemeMargins(theme_, dc, MENU_POPUPITEM, MPI_NORMAL, TMT_CONTENTMARGINS, nullptr, &metrics_.itemMargins);
    // The layout is the classic one -- the check column, the text, then the accelerator and
    // submenu arrow columns USER32 reserves whether an item uses them or not -- because that
    // is what Windows gives this menu when it is not dark, and the list should not change
    // size with the host's theme. Only the colours and the glyphs come from the theme. The
    // gap before the accelerator column is 10 pixels at 96 dpi; tying it to the check mark
    // scales it with the display, as the system metrics scale themselves.
    const int gap = metrics_.check.cx * 5 / 8;
    metrics_.gutter = GetSystemMetrics(SM_CXMENUSIZE);
    if (metrics_.gutter < metrics_.check.cx) metrics_.gutter = metrics_.check.cx;
    metrics_.trailing = GetSystemMetrics(SM_CXMENUCHECK) + gap;
    // Measured: USER32 adds this much to the width reported for an owner-drawn item, so it
    // comes off what we report. Both metrics scale with the display.
    metrics_.systemPad = GetSystemMetrics(SM_CXMENUSIZE) - GetSystemMetrics(SM_CXBORDER);
    metrics_.row = GetSystemMetrics(SM_CYMENUSIZE);
    metrics_.separatorHeight = metrics_.row / 2;
    // A column divider is a separator's line with a little air on either side (1 + 1 + 1 at
    // 96 dpi), so that it reads like the horizontal separators rather than like a border.
    if (metrics_.separator.cx <= 0) metrics_.separator.cx = 1;
    const int air = metrics_.check.cx / 16 > 0 ? metrics_.check.cx / 16 : 1;
    metrics_.divider = metrics_.separator.cx + 2 * air;
    const COLORREF line =
        background == CLR_INVALID
            ? CLR_INVALID
            : themeLineColor(theme_, dc, MENU_POPUPSEPARATOR, 0, metrics_.separator, background);
    ReleaseDC(owner, dc);

    if (background == CLR_INVALID || line == CLR_INVALID) {
        reset();
        return false;
    }
    background_ = CreateSolidBrush(background);
    divider_ = CreateSolidBrush(line);

    convert(menu);
    if (items_.empty()) {
        reset();
        return false;
    }

    // Everything outside the item rectangles -- the padding around the columns and the strip
    // the column break leaves -- is the menu's own background, which the system would paint
    // light here too.
    MENUINFO mi{};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
    mi.hbrBack = background_;
    SetMenuInfo(menu, &mi);
    return true;
}

void DarkMenu::convert(HMENU menu) {
    const int count = GetMenuItemCount(menu);
    bool column = false;  // true once a break has started a second column
    for (int i = 0; i < count; i++) {
        MENUITEMINFOW read{};
        read.cbSize = sizeof(read);
        read.fMask = MIIM_FTYPE | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, UINT(i), TRUE, &read)) continue;
        if (read.hSubMenu) convert(read.hSubMenu);

        if (read.fType & (MFT_MENUBREAK | MFT_MENUBARBREAK)) column = true;

        auto item = std::make_unique<Item>();
        item->separator = (read.fType & MFT_SEPARATOR) != 0;
        item->radio = (read.fType & MFT_RADIOCHECK) != 0;
        item->submenu = read.hSubMenu != nullptr;
        item->column = column;
        if (!item->separator) item->text = itemText(menu, i);

        MENUITEMINFOW write{};
        write.cbSize = sizeof(write);
        write.fMask = MIIM_FTYPE | MIIM_DATA;
        // MFT_SEPARATOR and the break must survive -- the system still lays the columns out,
        // it only stops drawing the items. MFT_MENUBARBREAK becomes MFT_MENUBREAK: the bar it
        // asks for is the raised classic one, light grey and out of place in a dark menu, and
        // it is drawn outside the item rectangles where we cannot paint over it. We leave the
        // columns bare and draw the divider with the items instead, as a line in the colour
        // the horizontal separators use.
        write.fType = ((read.fType & ~MFT_MENUBARBREAK) | MFT_OWNERDRAW) |
                      ((read.fType & (MFT_MENUBREAK | MFT_MENUBARBREAK)) ? MFT_MENUBREAK : 0);
        write.dwItemData = reinterpret_cast<ULONG_PTR>(item.get());
        if (!SetMenuItemInfoW(menu, UINT(i), TRUE, &write)) continue;
        items_.push_back(std::move(item));
    }
}

bool DarkMenu::owns(const Item* it) const {
    if (!it) return false;
    return std::any_of(items_.begin(), items_.end(), [it](const std::unique_ptr<Item>& p) { return p.get() == it; });
}

bool DarkMenu::measure(MEASUREITEMSTRUCT* mis) const {
    if (!mis || mis->CtlType != ODT_MENU || !theme_) return false;
    const Item* item = reinterpret_cast<const Item*>(mis->itemData);
    if (!owns(item)) return false;

    const MARGINS& im = metrics_.itemMargins;
    const int indent = item->column ? metrics_.divider : 0;
    if (item->separator) {
        mis->itemWidth = UINT(indent + metrics_.gutter);
        mis->itemHeight = UINT(metrics_.separatorHeight);
        return true;
    }

    HDC dc = GetDC(owner_);
    HGDIOBJ old = SelectObject(dc, font_);
    SIZE text{};
    GetTextExtentPoint32W(dc, item->text.c_str(), int(item->text.size()), &text);
    SelectObject(dc, old);
    ReleaseDC(owner_, dc);

    const int width = metrics_.gutter + text.cx + metrics_.trailing;
    mis->itemWidth = UINT(indent + (width > metrics_.systemPad ? width - metrics_.systemPad : 0));
    // The classic row height, but never less than what the text or the check mark needs.
    int height = metrics_.row;
    const int textHeight = text.cy + im.cyTopHeight + im.cyBottomHeight;
    if (textHeight > height) height = textHeight;
    if (metrics_.check.cy > height) height = metrics_.check.cy;
    mis->itemHeight = UINT(height);
    return true;
}

bool DarkMenu::draw(const DRAWITEMSTRUCT* dis) const {
    if (!dis || dis->CtlType != ODT_MENU || !theme_) return false;
    const Item* item = reinterpret_cast<const Item*>(dis->itemData);
    if (!owns(item)) return false;

    HDC dc = dis->hDC;
    const bool disabled = (dis->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    const bool hot = (dis->itemState & (ODS_SELECTED | ODS_HOTLIGHT)) != 0;
    const int state = disabled ? (hot ? MPI_DISABLEDHOT : MPI_DISABLED) : (hot ? MPI_HOT : MPI_NORMAL);

    FillRect(dc, &dis->rcItem, background_);

    // The divider between two columns: one line per item, which together make the continuous
    // line the whole column long. It stays outside the item, so the highlight does not cover it.
    RECT rc = dis->rcItem;
    if (item->column) {
        const int air = (metrics_.divider - metrics_.separator.cx) / 2;
        RECT line{rc.left + air, rc.top, rc.left + air + metrics_.separator.cx, rc.bottom};
        FillRect(dc, &line, divider_);
        rc.left += metrics_.divider;
    }
    DrawThemeBackground(theme_, dc, MENU_POPUPITEM, state, &rc, nullptr);

    if (item->separator) {
        // The whole width of the item, check column included: that is where USER32 puts the
        // line in a menu it draws itself, and a line that starts after the gutter reads as
        // right-aligned next to one that does not.
        DrawThemeBackground(theme_, dc, MENU_POPUPSEPARATOR, 0, &rc, nullptr);
        return true;
    }

    if (dis->itemState & ODS_CHECKED) {
        RECT box{rc.left, rc.top, rc.left + metrics_.gutter, rc.bottom};
        DrawThemeBackground(theme_, dc, MENU_POPUPCHECKBACKGROUND, disabled ? MCB_DISABLED : MCB_NORMAL, &box,
                            nullptr);
        RECT mark{box.left + (metrics_.gutter - metrics_.check.cx) / 2,
                  rc.top + (rc.bottom - rc.top - metrics_.check.cy) / 2, 0, 0};
        mark.right = mark.left + metrics_.check.cx;
        mark.bottom = mark.top + metrics_.check.cy;
        const int part = item->radio ? (disabled ? MC_BULLETDISABLED : MC_BULLETNORMAL)
                                     : (disabled ? MC_CHECKMARKDISABLED : MC_CHECKMARKNORMAL);
        DrawThemeBackground(theme_, dc, MENU_POPUPCHECK, part, &mark, nullptr);
    }

    if (item->submenu) {
        SIZE arrow{};
        const int part = disabled ? MSM_DISABLED : MSM_NORMAL;
        if (SUCCEEDED(GetThemePartSize(theme_, dc, MENU_POPUPSUBMENU, part, nullptr, TS_TRUE, &arrow))) {
            RECT a{rc.right - arrow.cx - metrics_.itemMargins.cxRightWidth,
                   rc.top + (rc.bottom - rc.top - arrow.cy) / 2, 0, 0};
            a.right = a.left + arrow.cx;
            a.bottom = a.top + arrow.cy;
            DrawThemeBackground(theme_, dc, MENU_POPUPSUBMENU, part, &a, nullptr);
        }
    }

    RECT text = rc;
    text.left += metrics_.gutter;
    text.right -= metrics_.itemMargins.cxRightWidth;
    DWORD flags = DT_SINGLELINE | DT_LEFT | DT_VCENTER;
    if (dis->itemState & ODS_NOACCEL) flags |= DT_HIDEPREFIX;
    HGDIOBJ old = SelectObject(dc, (dis->itemState & ODS_DEFAULT) && boldFont_ ? boldFont_ : font_);
    DrawThemeText(theme_, dc, MENU_POPUPITEM, state, item->text.c_str(), -1, flags, 0, &text);
    SelectObject(dc, old);
    return true;
}

void DarkMenu::reset() {
    items_.clear();
    if (theme_) CloseThemeData(theme_);
    if (background_) DeleteObject(background_);
    if (divider_) DeleteObject(divider_);
    if (font_) DeleteObject(font_);
    if (boldFont_) DeleteObject(boldFont_);
    theme_ = nullptr;
    background_ = nullptr;
    divider_ = nullptr;
    font_ = nullptr;
    boldFont_ = nullptr;
    owner_ = nullptr;
    metrics_ = Metrics{};
}

}  // namespace sq8l::gui
