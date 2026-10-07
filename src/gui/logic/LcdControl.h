// The VFD page controller: port of unit lcdControl (0x4587d8..0x45b86c) -- ClcdCtr,
// ClcdCtr_page / _pageSL / _pageML and ClcdCtr_param -- plus the page set up of FUN_0047e340
// (unit_47e330), driven by the data in data/GuiLogicData.cpp.
//
// Object structure of the original: ClcdCtr owns 18 pages (ClcdCtr_pageML, depth 0); each has
// one depth 1 child that splits the page definition into sub-pages of two lines ("\&" separated
// lines), each sub-page is a depth 2 ClcdCtr_pageML wrapping one ClcdCtr_pageSL that parses its
// two lines into parameters. Depth 0 and 2 just forward every call to their only child (their
// "auto select sub-page by parameter value" feature, ML +0x18, is never enabled), so LcdPage
// below represents depth 0+1 and LcdSubPage depth 2+SL.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Formatters.h"
#include "LcdData.h"
#include "PlatformUi.h"
#include "controls/LcdDisplay.h"

namespace sq8l {
class EditBuffer;
}

namespace sq8l::gui {

struct LcdParam {                // ClcdCtr_param (140 bytes)
    int knob = -1;               // +0x04 knob bound to this parameter
    int index = 0;               // +0x08 index inside the sub-page
    int ordinal = -1;            // +0x0c running number of the value fields of the sub-page
    std::string name;            // +0x10
    std::string hint;            // +0x14
    std::string label;           // +0x18 static text before the value
    std::string valueText;       // +0x1c
    int width = 0;               // +0x20 value field width
    int x = 0, y = 0;            // +0x24 / +0x28 column / row
    int min = 0, max = 0;        // +0x2c / +0x30
    int value = 0;               // +0x34
    Fmt fmt = Fmt::None;         // +0x38 formatter
    PopText pop = PopText::None; // +0x40 popup item text
    bool customFmt = false;      // +0x48
    Change change = Change::None;// +0x58
    bool showNumber = true;      // +0x64
    int perColumn = 0;           // +0x68
    int paramIndex = -1;         // +0x74 edit buffer parameter override
    bool highlight = false;      // +0x78 selected (drawn with attribute 1)

    void reset();                        // FUN_00458c1c
    void setValue(int v, int pageNo);    // FUN_00458e1c (formats the value text)
    void setWidth(int w);                // FUN_00458d74
    void setRange(int which, int v);     // FUN_00458dd4
    void setFormatter(Fmt f);            // FUN_00458da4 (f == None: back to the default)
    void selectDefaultFormatter();       // FUN_00458fd0 / FUN_00458f98
    std::string fullText() const { return label + valueText; }   // FUN_00458ef0
    std::string paddedText() const;      // FUN_00458f08
    int textLength() const { return int(label.size()) + width; } // FUN_00458f88
};

struct LcdSubPage {              // ClcdCtr_pageML depth 2 + ClcdCtr_pageSL
    std::string title;           // SL +0x14
    int c = 0;                   // SL +0x0c
    int group = 0;               // SL +0x10
    int editor = 0;              // SL +0x20 parameter editor (the edit buffer's; 1 = set)
    int base = 0;                // SL +0x24
    int offset = 0;              // SL +0x28
    int flags[2] = {0, 0};       // SL +0x2c[2] page menu indent / hidden
    std::vector<LcdParam> params;// SL +0x18
    std::vector<int> knobMap;    // SL +0x1c knob -> parameter index (-1 none)

    void reset();                                   // SL vmt+0x70
    void setParamCount(int n);                      // SL vmt+0x08
    void setKnobCount(int n);                       // SL vmt+0x1c
    LcdParam* param(int i);                         // SL vmt+0x0c
    LcdParam* paramForKnob(int k);                  // SL vmt+0x14
    int knobParam(int k) const;                     // SL vmt+0x20
    void setKnobParam(int k, int p);                // SL vmt+0x24
    bool knobFree(int k) const;                     // SL vmt+0x28
    void assignKnob(int p, int k);                  // SL vmt+0x2c
    void bindKnob(LcdParam& p, int k);              // FUN_00458cb8
    bool setText(const std::string& s);             // SL vmt+0x30 (FUN_004595a8 + FUN_00459b0c)
    LcdParam* findParam(const std::string& name);   // SL vmt+0x10
    int editBufferIndex(const LcdParam& p) const;   // FUN_00458ea4
};

struct LcdPage {                 // ClcdCtr_pageML depth 0 + depth 1
    std::vector<LcdSubPage> subs;// ML1 +0x0c
    int cur = 0;                 // ML1 +0x10
    void setText(const std::string& s);   // ML1 vmt+0x30: split into sub-pages
    void select(int n);                   // FUN_0045a170
    LcdSubPage& sub() { return subs[size_t(cur)]; }
    const LcdSubPage& sub() const { return subs[size_t(cur)]; }
};

// Callbacks of ClcdCtr (+0x28..+0x6c) implemented by the editor form.
class LcdListener {
public:
    virtual ~LcdListener() = default;
    virtual void lcdKnobValue(int knob, int value) = 0;          // +0x28 (LAB_004846c4)
    virtual void lcdKnobRange(int knob, int min, int max) = 0;   // +0x30 (FUN_00484700)
    virtual void lcdDrawn() = 0;                                 // +0x38 (FUN_004847a4)
    virtual void lcdParamMouseDown(int param) = 0;               // +0x40 (SUB_00484800)
    virtual void lcdHint(int param) = 0;                         // +0x50 (LAB_00484874)
    virtual void lcdSaveMouse() = 0;                             // +0x58 (FUN_00483a5c)
    virtual void lcdRestoreMouse() = 0;                          // +0x60 (FUN_00483a84)
    virtual void lcdScrollArrows(bool down, bool up) = 0;        // +0x68 (LAB_00484908)
    virtual void lcdParamChanged(int page, int param) = 0;       // param +0x58 (FUN_0048489c)
    virtual void lcdShowPopup(const struct LcdPopup& popup) = 0; // TPopupMenu(+0x80).Popup
};

// A value popup menu built by FUN_0045aca0 (the TPopupMenu at ClcdCtr +0x80).
struct LcdPopup {
    int param = -1;              // TPopupMenu +0x0c: the parameter (index in the sub-page)
    std::vector<MenuItem> items; // ids: value + kPopupIdBase
    int x = 0, y = 0;            // form position (lcd client + lcd position)
};

class LcdController {            // ClcdCtr (152 bytes)
public:
    static constexpr int kPopupIdBase = 0x10000;

    LcdController(LcdDisplay& lcd, EditBuffer* editBuffer, LcdListener* listener);

    // set up (FUN_0047e340): pages from the definition strings + the recorded set up
    // portExtensions: build the pages with the port's additions (the VOICES control on the
    // EMU page); false builds exactly the original's pages.
    void buildPages(bool portExtensions = true);

    // pages
    int pageCount() const { return int(pages_.size()); }
    LcdPage* page(int i) { return (i >= 0 && i < pageCount()) ? &pages_[size_t(i)] : nullptr; }
    const std::vector<LcdPage>& pages() const { return pages_; }
    int pageIndex() const { return pageIndex_; }                 // +0x14
    LcdPage* currentPage() { return page_; }                     // +0x18
    LcdSubPage* currentSub() { return page_ ? &page_->sub() : nullptr; }
    void selectPage(int n);                                      // FUN_0045a84c
    int subPageCount() const;                                    // FUN_0045a8b8
    int subPage() const;                                         // FUN_0045a8cc
    void selectSubPage(int n);                                   // FUN_0045a8e0
    void nextSubPage() { selectSubPage(subPage() + 1); }         // FUN_0045a970
    void prevSubPage() { selectSubPage(subPage() - 1); }         // 0x45a988
    void selectPageSub(int page, int sub);                       // FUN_0045a9a0

    // drawing / refresh
    void drawAll();                                              // FUN_0045a9d0 (= FUN_0045ab44)
    void drawParam(LcdParam& p);                                 // FUN_0045aab4
    void refresh(bool draw);                                     // FUN_0045b448
    void updateKnobRanges();                                     // FUN_0045b6e4
    void lockDisplay() { displayLock_++; }                       // FUN_0045ab60
    void unlockDisplay(bool redraw);                             // FUN_0045ab64

    // editing
    int readParam(const LcdParam& p) const;                      // FUN_0045b378
    void writeParam(const LcdParam& p, int v);                   // FUN_0045b3a4
    int currentValue(int param) const;                           // FUN_0045b538
    void knobChanged(int knob, int value);                       // 0x45b63c
    void setParam(int param, int value);                         // FUN_0045b574
    LcdParam* paramAt(int col, int row);                         // FUN_0045b3d0

    // VFD mouse events (cell coordinates, from TLCD3 +0x648/+0x650/+0x658/+0x668)
    void cellMouseDown(int button, int col, int row);            // FUN_0045b1d4
    void cellMouseUp(int col, int row);                          // FUN_0045b260
    void cellMouseMove(int col, int row);                        // FUN_0045b280
    void cellDoubleClick(int col, int row);                      // FUN_0045b2b0
    // value popup (FUN_0045aca0) and the click on one of its items (LAB_0045b18c)
    void popupValues(int col, LcdParam& p);
    const LcdPopup& popup() const { return popup_; }
    void popupClicked(int itemId);

    // messages over the display (FUN_0045aba4 / FUN_0045ab88) and timer tick (FUN_0045b334)
    void showMessage(int x1, int x2, const std::string& line1, const std::string& line2, int timeout);
    void endMessage();
    void tick();

    // ClcdCtr fields
    int refreshDiv = 0;          // +0x0c (FUN_0045a730)
    int refreshCount = 0;        // +0x10
    int numKnobs = 10;           // +0x1c
    int updateLock = 0;          // +0x20
    bool mouseIsDown = false;    // +0x70
    int downParam = -1;          // +0x74 (index of the parameter, -1 = none)
    int downCol = 0, downRow = 0;// +0x78 / +0x7c
    bool restoreMouse = true;    // +0x8c
    int msgTimeout = 0;          // +0x90
    int msgActive = 0;           // +0x94
    int displayLock() const { return displayLock_; }             // +0x24
    void setRefreshDiv(int n) { refreshDiv = n < 1 ? 0 : n; }    // FUN_0045a730

private:
    void scrollArrows();

    LcdDisplay& lcd_;            // +0x08
    EditBuffer* eb_;             // parameter editor of the pages (+0x20 of every page)
    LcdListener* listener_;
    std::vector<LcdPage> pages_; // +0x04
    int pageIndex_ = 0;          // +0x14
    LcdPage* page_ = nullptr;    // +0x18
    int displayLock_ = 0;        // +0x24
    LcdPopup popup_;             // +0x80
};

}  // namespace sq8l::gui
