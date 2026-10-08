#include "LcdControl.h"

#include "EditBuffer.h"
#include "VoiceSlots.h"  // (port) kOriginalPlayableVoices

namespace sq8l::gui {

namespace {

// Delphi Val(s, v, code) for integers (System._ValLong): leading blanks, optional sign,
// decimal digits; code = 0 on success.
int valLong(const std::string& s, int& code) {
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') i++;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
    long long v = 0;
    const size_t first = i;
    for (; i < s.size(); i++) {
        if (s[i] < '0' || s[i] > '9') {
            code = int(i) + 1;
            return int(neg ? -v : v);
        }
        v = v * 10 + (s[i] - '0');
    }
    code = i == first ? int(i) + 1 : 0;
    return int(neg ? -v : v);
}

// Delphi Copy(s, index, count) with 1-based index.
std::string copy(const std::string& s, int index, int count) {
    if (index < 1) index = 1;
    if (count <= 0 || index > int(s.size())) return {};
    return s.substr(size_t(index - 1), size_t(count));
}

bool isDigit(char c) { return uint8_t(c - '0') < 10; }

}  // namespace

// ==================================================================== ClcdCtr_param

void LcdParam::reset() {  // FUN_00458c1c
    knob = -1;
    ordinal = -1;
    name.clear();
    hint.clear();
    label.clear();
    valueText.clear();
    width = 0;
    x = 0;
    y = 0;
    fmt = Fmt::None;
    pop = PopText::None;
    customFmt = false;
    perColumn = 0;
    min = 0;
    max = 0;
    value = 0;
    paramIndex = -1;
    highlight = false;
}

void LcdParam::setValue(int v, int pageNo) {  // FUN_00458e1c
    (void)pageNo;  // passed to the formatters, unused by all of them
    if (fmt != Fmt::None) {
        std::string s;
        formatValue(fmt, width, v, s);
        valueText = s;  // FUN_00458cf8(1)
    }
    value = v;
}

void LcdParam::setWidth(int w) {  // FUN_00458d74
    if (w < 1) w = 0;
    if (w != width) {
        width = w;
        if (w < int(valueText.size())) valueText.resize(size_t(w));
    }
}

void LcdParam::setRange(int which, int v) {  // FUN_00458dd4
    if (which == 0) {
        if (v != min) {
            min = v;
            if (max < v) max = v;
        }
    } else if (which == 1) {
        if (v != max) {
            max = v;
            if (v < min) min = v;
        }
    }
    selectDefaultFormatter();
}

void LcdParam::selectDefaultFormatter() {  // FUN_00458fd0 / FUN_00458f98
    if (customFmt) return;
    if (min == 0 && max == 1)
        fmt = Fmt::OffOn;
    else if (min < 0 || max < 0)
        fmt = Fmt::Signed;
    else
        fmt = Fmt::Unsigned;
}

void LcdParam::setFormatter(Fmt f) {  // FUN_00458da4
    if (f == Fmt::None) {
        selectDefaultFormatter();
        customFmt = false;
    } else {
        fmt = f;
        customFmt = true;
    }
}

std::string LcdParam::paddedText() const {  // FUN_00458f08
    const int pad = width - int(valueText.size());
    if (pad > 0) return label + valueText + delphi::stringOfChar(' ', pad);
    return label + valueText;
}

// ==================================================================== ClcdCtr_pageSL

void LcdSubPage::reset() {  // v028
    params.clear();         // v000
    knobMap.clear();
    title.clear();          // vmt+0x3c(0)
    editor = 0;
    base = 0;
    offset = 0;
}

void LcdSubPage::setParamCount(int n) {  // v002
    if (n < 1) {
        params.clear();
        return;
    }
    const int old = int(params.size());
    params.resize(size_t(n));
    for (int i = old; i < n; i++) {  // FUN_00458ba0: +0x64 = 1, +0x08 = index, reset
        params[size_t(i)] = LcdParam{};
        params[size_t(i)].showNumber = true;
        params[size_t(i)].index = i;
        params[size_t(i)].reset();
    }
}

void LcdSubPage::setKnobCount(int n) {  // v007
    if (n < 1) {
        knobMap.clear();
        return;
    }
    knobMap.resize(size_t(n), -1);
}

LcdParam* LcdSubPage::param(int i) {  // v003
    if (i >= 0 && i < int(params.size())) return &params[size_t(i)];
    return nullptr;
}

int LcdSubPage::knobParam(int k) const {  // v008
    if (k >= 0 && k < int(knobMap.size())) return knobMap[size_t(k)];
    return -1;
}

LcdParam* LcdSubPage::paramForKnob(int k) { return param(knobParam(k)); }  // v005

void LcdSubPage::setKnobParam(int k, int p) {  // v009
    if (k >= 0 && k < int(knobMap.size()) && p < int(params.size())) knobMap[size_t(k)] = p;
}

bool LcdSubPage::knobFree(int k) const {  // v010
    for (const LcdParam& p : params)
        if (p.knob == k) return false;
    return true;
}

void LcdSubPage::assignKnob(int p, int k) {  // v011
    for (size_t i = 0; i < knobMap.size(); i++)
        if (knobMap[i] == p) knobMap[i] = -1;
    setKnobParam(k, p);
}

void LcdSubPage::bindKnob(LcdParam& p, int k) {  // FUN_00458cb8
    if (k == p.knob) return;
    p.knob = knobFree(k) ? k : -1;
    assignKnob(p.index, p.knob);
}

LcdParam* LcdSubPage::findParam(const std::string& name) {  // v004
    for (LcdParam& p : params)
        if (p.name == name) return &p;
    return nullptr;
}

int LcdSubPage::editBufferIndex(const LcdParam& p) const {  // FUN_00458ea4
    if (p.paramIndex >= 0) return offset + p.paramIndex;
    if (p.ordinal < 0) return -1;
    return base + offset + p.ordinal;
}

bool LcdSubPage::setText(const std::string& s) {  // v012
    // ---- FUN_004595a8: count parameters ('[') and the highest knob number ("{n,")
    int count = 0, maxKnob = -1, depth = 0;
    const int len = int(s.size());
    for (int i = 1; i <= len; i++) {
        const char c = s[size_t(i - 1)];
        if (c == '[') {
            if (depth > 0) return false;
            depth++;
            count++;
        } else if (c == ']') {
            if (depth < 1) return false;
            depth--;
        } else if (c == '{') {
            if (depth < 1 || len < i + 1) return false;
            for (int j = i + 1; j <= len; j++) {
                if (s[size_t(j - 1)] == ',') {
                    int code;
                    const int n = valLong(copy(s, i + 1, j - i - 1), code);
                    if (code == 0 && maxKnob < n) maxKnob = n;
                    break;
                }
            }
        } else if (c == '}' && depth < 1) {
            return false;
        }
    }
    const int knobs = maxKnob + 1;
    if (count <= 0) return false;
    reset();
    setParamCount(count);
    setKnobCount(knobs);

    // ---- FUN_00459b0c: parse
    int x = 0, ord = 0, y = 0, pidx = 0;
    depth = 0;
    bool inNum = false;
    int start = 0;
    // FUN_00459a34: decimal number starting at `from`
    auto number = [&](int from) {
        if (from < 1 && from > len) return 0;
        int end = from;
        for (int k = from; k <= len; k++) {
            if (!isDigit(s[size_t(k - 1)])) {
                end = k - 1;
                break;
            }
        }
        if (from > end) return 0;
        int code;
        return valLong(copy(s, from, end - from + 1), code);
    };
    // FUN_00459700: "{knob,name,width,min,max}"
    auto field = [&](int from, int to, LcdParam& p) {
        std::string t = copy(s, from, to - from + 1);
        int code;
        size_t c = t.find(',');
        if (c == std::string::npos) return;
        int v = valLong(t.substr(0, c), code);
        bindKnob(p, code == 0 ? v : -1);
        t.erase(0, c + 1);
        if ((c = t.find(',')) == std::string::npos) return;
        p.name = t.substr(0, c);
        t.erase(0, c + 1);
        if ((c = t.find(',')) == std::string::npos) return;
        v = valLong(t.substr(0, c), code);
        p.setWidth(code == 0 ? v : 0);
        t.erase(0, c + 1);
        if ((c = t.find(',')) == std::string::npos) return;
        v = valLong(t.substr(0, c), code);
        p.setRange(0, code == 0 ? v : 0);
        t.erase(0, c + 1);
        v = valLong(t, code);
        p.setRange(1, code == 0 ? v : 0);
        x += p.width;
        p.ordinal = ord++;
    };
    // FUN_00459924: "[label{...}]"
    auto item = [&](int from, int to, LcdParam& p) {
        p.reset();
        p.x = x;
        p.y = y;
        int labelLen = to - from + 1;
        int vstart = 0;
        for (int k = from; k <= to; k++) {
            const char c = s[size_t(k - 1)];
            if (c == '{') {
                labelLen = k - from;
                vstart = k + 1;
            } else if (c == '}') {
                field(vstart, k - 1, p);
                break;
            }
        }
        x += labelLen + 1;
        p.label = labelLen < 1 ? std::string() : copy(s, from, labelLen);
        pidx++;
    };
    for (int i = 1; i <= len; i++) {
        if (inNum && !isDigit(s[size_t(i - 1)])) {
            inNum = false;
            x += number(start);
        }
        const char c = s[size_t(i - 1)];
        if (isDigit(c)) {
            if (depth <= 0 && !inNum) {
                inNum = true;
                start = i;
            }
        } else if (c == '[') {
            start = i + 1;
            depth++;
        } else if (c == '\\') {
            if (i < len && depth <= 0) {
                if (s[size_t(i)] == '&') {
                    x = 0;
                    y++;
                }
                i++;
            }
        } else if (c == ']') {
            depth--;
            LcdParam* p = param(pidx);
            if (!p) return false;
            item(start, i - 1, *p);
        }
    }
    return true;
}

// ==================================================================== ClcdCtr_pageML

void LcdPage::setText(const std::string& s) {  // ML depth 1: v012
    const int len = int(s.size());
    int seps[33] = {0};
    int n = 0;
    for (int i = 1; i <= len; i++) {
        if (s[size_t(i - 1)] == '\\' && i < len && s[size_t(i)] == '&') {
            if (n + 1 < 33) seps[n + 1] = i + 1;
            n++;
            i++;
        }
    }
    int segs = n + 1;
    if (segs < 3) {
        subs.resize(1);  // FUN_0045a074(self, 1)
        subs[0].setText(s);
        return;
    }
    if (segs > 32) segs = 32;
    const int half = segs >> 1;
    const int count = half + (segs & 1);
    seps[0] = 0;
    seps[segs] = len;
    subs.resize(size_t(count));
    for (int i = 0; i < half; i++) {
        const int from = seps[2 * i] + 1, to = seps[2 * i + 2];
        subs[size_t(i)].setText(copy(s, from, to - from + 1));
    }
    if (segs & 1) {
        const int from = seps[2 * (count - 1)] + 1, to = seps[2 * (count - 1) + 1];
        subs[size_t(count - 1)].setText(copy(s, from, to - from + 1) + ",");
    }
}

void LcdPage::select(int n) {  // FUN_0045a170
    const int last = int(subs.size()) - 1;
    if (n < 1)
        cur = 0;
    else
        cur = last < n ? last : n;
}

// ==================================================================== ClcdCtr

LcdController::LcdController(LcdDisplay& lcd, EditBuffer* editBuffer, LcdListener* listener)
    : lcd_(lcd), eb_(editBuffer), listener_(listener) {}

void LcdController::buildPages(bool portExtensions) {  // FUN_0045a790(0x12) + FUN_0047e340
    pages_.assign(size_t(kNumPages), LcdPage{});
    for (int i = 0; i < kNumPages; i++) {
        const PageSetup& ps = data::pageSetup(i, portExtensions);
        LcdPage& pg = pages_[size_t(i)];
        pg.subs.resize(1);
        pg.subs[0].c = i;
        pg.setText(ps.definition);
        for (int j = 0; j < ps.numSubPages && j < int(pg.subs.size()); j++) {
            const SubPageSetup& ss = ps.subPages[j];
            LcdSubPage& sp = pg.subs[size_t(j)];
            sp.c = i;  // created with the page number (FUN_00459e90)
            sp.title = ss.title;
            sp.group = ss.group;
            sp.editor = 1;
            sp.base = ss.base;
            sp.offset = ss.offset;
            sp.flags[0] = ss.indent;
            sp.flags[1] = ss.hidden;
            for (int k = 0; k < ss.numParams; k++) {
                const ParamSetup& ps2 = ss.params[k];
                LcdParam* p = sp.param(ps2.index);
                if (!p) continue;
                p->hint = ps2.hint ? ps2.hint : "";
                p->paramIndex = ps2.paramIndex;
                if (ps2.fmt != Fmt::None) p->setFormatter(ps2.fmt);
                p->pop = ps2.pop;
                p->showNumber = ps2.showNumber;
                p->perColumn = ps2.perColumn;
                p->change = ps2.change;
            }
        }
        pg.select(0);
    }
}

// -------------------------------------------------------------------- pages

void LcdController::selectPage(int n) {  // FUN_0045a84c
    LcdPage* p = page(n);
    if (!p) return;
    page_ = p;
    pageIndex_ = n;
    if (displayLock_ < 1) lcd_.clearCells();
    updateKnobRanges();
    refresh(false);
    drawAll();
    scrollArrows();
}

void LcdController::scrollArrows() {  // FUN_0045a924 + callback +0x68
    bool down = false, up = false;
    if (page_) {
        const int n = int(page_->subs.size());
        if (n > 1) {
            if (page_->cur < n - 1) down = true;
            if (page_->cur > 0) up = true;
        }
    }
    if (listener_) listener_->lcdScrollArrows(down, up);
}

int LcdController::subPageCount() const { return page_ ? int(page_->subs.size()) : 0; }  // FUN_0045a8b8

int LcdController::subPage() const { return page_ ? page_->cur : 0; }  // FUN_0045a8cc

void LcdController::selectSubPage(int n) {  // FUN_0045a8e0
    if (!page_) return;
    const int count = int(page_->subs.size());
    if (count > 0) {
        if (n < 0) n += count;
        n %= count;
    }
    page_->select(n);
    selectPage(pageIndex_);
}

void LcdController::selectPageSub(int pg, int sub) {  // FUN_0045a9a0
    lockDisplay();
    selectPage(pg);
    selectSubPage(sub);
    unlockDisplay(true);
}

void LcdController::unlockDisplay(bool redraw) {  // FUN_0045ab64
    if (displayLock_ > 0) displayLock_--;
    if (redraw) drawAll();
}

// -------------------------------------------------------------------- drawing

void LcdController::drawAll() {  // FUN_0045a9d0
    if (displayLock_ >= 1) return;
    lcd_.clearCells();
    if (!page_) return;
    LcdSubPage& sp = page_->sub();
    for (LcdParam& p : sp.params) lcd_.writeText(p.x, p.y, p.fullText(), p.highlight ? 1 : 0);
    if (listener_) listener_->lcdDrawn();
    lcd_.invalidate();
}

void LcdController::drawParam(LcdParam& p) {  // FUN_0045aab4
    if (displayLock_ >= 1) return;
    lcd_.writeText(p.x, p.y, p.paddedText(), p.highlight ? 1 : 0);
    if (listener_) listener_->lcdDrawn();
}

void LcdController::refresh(bool draw) {  // FUN_0045b448
    if (!page_) return;
    LcdSubPage& sp = page_->sub();
    if (draw) updateLock++;
    for (int k = 0; k < int(sp.knobMap.size()); k++) {
        LcdParam* p = sp.paramForKnob(k);
        if (!p) continue;
        p->setValue(readParam(*p), sp.c);
        if (draw) drawParam(*p);
        if (listener_) listener_->lcdKnobValue(k, p->value);   // FUN_0045b76c(.., 0)
    }
    if (draw && updateLock > 0) updateLock--;
}

void LcdController::updateKnobRanges() {  // FUN_0045b6e4
    if (!page_ || !listener_) return;
    LcdSubPage& sp = page_->sub();
    const int n = int(sp.knobMap.size());
    for (int k = 0; k < n; k++) {
        LcdParam* p = sp.paramForKnob(k);
        if (p)
            listener_->lcdKnobRange(k, p->min, p->max);
        else
            listener_->lcdKnobRange(k, 0, 0);
    }
    for (int k = n; k <= numKnobs - 1; k++) listener_->lcdKnobRange(k, 0, 0);
}

// -------------------------------------------------------------------- editing

int LcdController::readParam(const LcdParam& p) const {  // FUN_0045b378
    if (!page_ || !eb_) return 0;
    const LcdSubPage& sp = page_->sub();
    if (!sp.editor) return 0;
    const int idx = sp.editBufferIndex(p);
    if (uint32_t(idx) >= uint32_t(kNumParams)) return 0;
    const int v = eb_->param(idx);
    // (port) EMU -> VOICES: a program of the original has 0 there, which means its 8 voices.
    // Reading it as 8 puts the knob and the value popup where the display already says they
    // are, so turning the knob goes to 7 or 9 instead of jumping to the bottom of the range.
    // The program itself keeps the 0 until the parameter is really edited.
    if (p.fmt == Fmt::Polyphony && v == 0) return kOriginalPlayableVoices;
    return v;
}

void LcdController::writeParam(const LcdParam& p, int v) {  // FUN_0045b3a4
    if (!page_ || !eb_) return;
    const LcdSubPage& sp = page_->sub();
    if (!sp.editor) return;
    const int idx = sp.editBufferIndex(p);
    if (uint32_t(idx) < uint32_t(kNumParams)) eb_->setParamValue(idx, v);
}

int LcdController::currentValue(int param) const {  // FUN_0045b538
    if (!page_) return 0;
    const LcdSubPage& sp = page_->sub();
    if (param < 0 || param >= int(sp.params.size())) return 0;
    return readParam(sp.params[size_t(param)]);
}

void LcdController::knobChanged(int knob, int value) {  // 0x45b63c
    endMessage();
    if (!page_) return;
    LcdSubPage& sp = page_->sub();
    LcdParam* p = sp.paramForKnob(knob);
    if (!p) return;
    int v = value;
    if (v < p->min)
        v = p->min;
    else if (v > p->max)
        v = p->max;
    writeParam(*p, v);
    p->setValue(v, sp.c);
    drawParam(*p);  // page vmt+0x74 (auto sub-page selection) is never active: no relayout
    if (p->change == Change::Form && listener_) listener_->lcdParamChanged(sp.c, p->index);
}

void LcdController::setParam(int param, int value) {  // FUN_0045b574
    if (!page_) return;
    LcdSubPage& sp = page_->sub();
    LcdParam* p = sp.param(param);
    if (!p) return;
    int v = value;
    if (v < p->min)
        v = p->min;
    else if (v > p->max)
        v = p->max;
    writeParam(*p, v);  // (pre-set filter +0x50: never installed)
    p->setValue(v, sp.c);
    drawParam(*p);
    drawParam(*p);
    if (p->change == Change::Form && listener_) listener_->lcdParamChanged(sp.c, p->index);
}

LcdParam* LcdController::paramAt(int col, int row) {  // FUN_0045b3d0
    if (!page_) return nullptr;
    for (LcdParam& p : page_->sub().params)
        if (p.x <= col && col < p.textLength() + p.x && row == p.y) return &p;
    return nullptr;
}

// -------------------------------------------------------------------- VFD mouse

void LcdController::cellMouseDown(int button, int col, int row) {  // FUN_0045b1d4
    if (msgActive == 1) endMessage();
    if (button != 0) return;
    mouseIsDown = true;
    downCol = col;
    downRow = row;
    LcdParam* p = paramAt(col, row);
    if (!p) return;
    downParam = p->index;
    if (listener_) listener_->lcdParamMouseDown(p->index);
}

void LcdController::cellMouseUp(int, int) {  // FUN_0045b260
    downParam = -1;
    mouseIsDown = false;
}

void LcdController::cellMouseMove(int col, int row) {  // FUN_0045b280
    LcdParam* p = paramAt(col, row);
    if (p && listener_) listener_->lcdHint(p->index);
}

void LcdController::cellDoubleClick(int col, int row) {  // FUN_0045b2b0
    LcdParam* p = paramAt(col, row);
    if (!p || p->knob < 0) return;
    if (p->max - p->min < 2) {
        int v = currentValue(p->index) + 1;
        if (v > p->max) v = p->min;
        const int idx = p->index, knob = p->knob;
        setParam(idx, v);
        lcd_.invalidate();  // FUN_0045b76c(.., 1)
        if (listener_) listener_->lcdKnobValue(knob, v);
    } else {
        popupValues(col, *p);
    }
}

void LcdController::popupValues(int col, LcdParam& p) {  // FUN_0045aca0
    if (listener_) listener_->lcdSaveMouse();  // FUN_0045b7a8
    const bool usePop = p.pop != PopText::None;
    const bool custom = usePop ? true : p.customFmt;
    if (!usePop && p.fmt == Fmt::None) return;
    auto text = [&](int v, std::string& s) {
        if (usePop)
            formatPopupText(p.pop, v, s);
        else
            formatValue(p.fmt, p.width, v, s);
    };
    popup_ = LcdPopup{};
    popup_.param = p.index;
    int lo = p.min, hi = p.max;
    if (hi < lo) std::swap(lo, hi);
    const int off = lo == -1 ? -1 : 0;
    const int cur = p.value;
    if (off == -1) lo++;
    const int perCol = p.perColumn > 0 ? p.perColumn : 16;
    const int digits = delphi::numDigits(hi - lo + 1);
    std::string s;  // the popup's string variable (kept across items like the original)
    std::vector<MenuItem>& items = popup_.items;
    if (off != lo) {
        text(off, s);
        MenuItem it;
        it.text = s;
        it.id = off + kPopupIdBase;
        it.radio = true;
        items.push_back(it);
        MenuItem sep;
        sep.separator = true;
        items.push_back(sep);
    }
    int col2 = 0;
    for (int v = lo; v <= hi; v++) {
        if (custom) {
            if (p.showNumber) {
                std::string val;
                formatValue(p.fmt, p.width, v, val);
                s = delphi::intToStrZ(v, digits) + "   " + val;
            } else {
                text(v, s);
            }
        } else {
            text(v, s);
        }
        MenuItem it;
        it.text = s;
        it.id = v + kPopupIdBase;
        it.radio = true;
        it.checked = v == cur;
        if (col2 >= perCol) {
            if (off != lo) {
                MenuItem empty;  // NewItem('', .., Enabled = False) with Break = mbBarBreak
                empty.enabled = false;
                empty.barBreak = true;
                items.push_back(empty);
                MenuItem sep;
                sep.separator = true;
                items.push_back(sep);
            } else {
                it.barBreak = true;
            }
            col2 = 0;
        }
        items.push_back(it);
        col2++;
    }
    popup_.x = lcd_.left() + lcd_.charX(col);
    popup_.y = lcd_.top() + lcd_.charY(p.y) + lcd_.charHeight();
    if (listener_) listener_->lcdShowPopup(popup_);
}

void LcdController::popupClicked(int itemId) {  // LAB_0045b18c
    if (!page_ || popup_.param < 0) return;
    LcdParam* p = page_->sub().param(popup_.param);
    if (!p) return;
    const int v = itemId - kPopupIdBase;
    const int knob = p->knob;
    setParam(popup_.param, v);
    lcd_.invalidate();
    if (listener_) listener_->lcdKnobValue(knob, v);
    if (restoreMouse && listener_) listener_->lcdRestoreMouse();
}

// -------------------------------------------------------------------- messages / timer

void LcdController::showMessage(int x1, int x2, const std::string& line1, const std::string& line2,
                                int timeout) {  // FUN_0045aba4
    if (displayLock_ < 1) lockDisplay();
    lcd_.clearCells();
    if (x1 < 0) x1 = int((uint32_t(lcd_.cols()) >> 1) - (uint32_t(line1.size()) >> 1));
    if (x2 < 0) x2 = int((uint32_t(lcd_.cols()) >> 1) - (uint32_t(line2.size()) >> 1));
    lcd_.writeText(x1, 0, line1, 0);
    lcd_.writeText(x2, 1, line2, 0);
    lcd_.invalidate();
    if (timeout > 0) {
        msgTimeout = timeout;
        msgActive = 1;
    }
}

void LcdController::endMessage() {  // FUN_0045ab88
    if (msgActive > 0) {
        msgActive = 0;
        unlockDisplay(true);
    }
}

void LcdController::tick() {  // FUN_0045b334
    if (refreshDiv > 0) {
        if (refreshCount < 1) {
            refreshCount = refreshDiv;
            lcd_.update();
        } else {
            refreshCount--;
        }
    }
    if (msgActive > 0) {
        if (msgTimeout < 1)
            endMessage();
        else
            msgTimeout--;
    }
}

}  // namespace sq8l::gui
