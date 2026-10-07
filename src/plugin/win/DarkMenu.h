// Windows draws popup menus in the host's dark theme on its own -- except menus that are
// split into columns (MFT_MENUBARBREAK), which fall back to the classic light colours. The
// program list and the longer LCD value lists are the only menus of that kind in SQ8L, so in
// a dark host (REAPER's dark mode) they came out white while every other menu was dark.
//
// For those menus, and only those, we take the drawing over: the items become owner-drawn and
// we paint them with the very theme parts USER32 would have used (MENU_POPUPITEM and friends),
// so the result follows the host instead of a palette of our own.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <uxtheme.h>

#include <memory>
#include <string>
#include <vector>

namespace sq8l::gui {

class DarkMenu {
public:
    DarkMenu() = default;
    ~DarkMenu() { reset(); }
    DarkMenu(const DarkMenu&) = delete;
    DarkMenu& operator=(const DarkMenu&) = delete;

    // Converts `menu` to owner-drawn items if it needs it: true if the caller must now route
    // WM_MEASUREITEM / WM_DRAWITEM here (and call reset() once the menu is gone). False leaves
    // the menu untouched -- it has no column break, or the host's menus are not dark, or the
    // theme is unavailable; the stock drawing is right in all of those cases.
    bool prepare(HMENU menu, HWND owner);

    bool active() const { return !items_.empty(); }
    bool measure(MEASUREITEMSTRUCT* mis) const;
    bool draw(const DRAWITEMSTRUCT* dis) const;
    void reset();

    // True if the popup menus of this process are drawn dark (what the host asked Windows for).
    static bool menusAreDark();

private:
    struct Item {
        std::wstring text;
        bool separator = false;
        bool radio = false;     // MFT_RADIOCHECK: a bullet instead of a check mark
        bool submenu = false;   // owner-drawn items get no arrow from the system
        bool column = false;    // in a column after a break: draws the divider on its left
    };
    // The parts of the menu layout USER32 would have measured with, read from the theme once.
    struct Metrics {
        SIZE check{};           // MENU_POPUPCHECK
        SIZE separator{};       // MENU_POPUPSEPARATOR
        MARGINS checkMargins{}; // around the check mark
        MARGINS itemMargins{};  // around the item text
        int gutter = 0;         // check column; the text starts right after it
        int trailing = 0;       // accelerator and arrow columns, reserved whether used or not
        int systemPad = 0;      // what USER32 adds to an owner-drawn item's reported width
        int row = 0;            // height of an item
        int separatorHeight = 0;
        int divider = 0;        // width taken by a column divider, line included
    };

    void convert(HMENU menu);
    bool owns(const Item* it) const;

    std::vector<std::unique_ptr<Item>> items_;
    Metrics metrics_;
    HTHEME theme_ = nullptr;
    HBRUSH background_ = nullptr;
    HBRUSH divider_ = nullptr;
    HFONT font_ = nullptr;
    HFONT boldFont_ = nullptr;
    HWND owner_ = nullptr;
};

}  // namespace sq8l::gui
