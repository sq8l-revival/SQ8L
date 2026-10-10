#include "PlatformUiDrawn.h"

#include <algorithm>
#include <fstream>
#include <iterator>

#include "Canvas.h"
#include "logic/Dialogs.h"

namespace sq8l::gui {

namespace {

constexpr int kFormW = 626, kFormH = 430;

// A dark theme that sits well on the editor's panel.
constexpr Color kBg = rgb(40, 40, 43);
constexpr Color kBorder = rgb(120, 120, 126);
constexpr Color kText = rgb(228, 228, 228);
constexpr Color kTextOff = rgb(118, 118, 122);
constexpr Color kHighlight = rgb(62, 98, 158);
constexpr Color kLine = rgb(84, 84, 90);
constexpr Color kCaption = rgb(64, 64, 70);
constexpr Color kList = rgb(20, 20, 22);
constexpr Color kButton = rgb(70, 70, 76);
constexpr Color kButtonDown = rgb(50, 50, 55);
constexpr Color kNameText = rgb(0x78, 0x79, 0x8b);  // the program name box colours
constexpr int kCaptionH = 20;

Font uiFont(Color c = kText) {
    Font f;
    f.face = "Arial";
    f.height = -11;
    f.color = c;
    return f;
}

// VCL captions: '&' marks the hotkey, "&&" is a literal ampersand.
std::string stripAmp(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '&') {
            if (i + 1 < s.size() && s[i + 1] == '&') out += '&', i++;
            continue;
        }
        out += s[i];
    }
    return out;
}

void fill(Bitmap& b, const Rect& r, Color c) {
    const Rect k = r.intersect(Rect{0, 0, b.width(), b.height()});
    for (int y = k.top; y < k.bottom; y++) {
        Color* row = b.row(y);
        for (int x = k.left; x < k.right; x++) row[x] = c;
    }
}

void frame(Bitmap& b, const Rect& r, Color c) {
    fill(b, Rect{r.left, r.top, r.right, r.top + 1}, c);
    fill(b, Rect{r.left, r.bottom - 1, r.right, r.bottom}, c);
    fill(b, Rect{r.left, r.top, r.left + 1, r.bottom}, c);
    fill(b, Rect{r.right - 1, r.top, r.right, r.bottom}, c);
}

// Darken the editor behind a dialog.
void dim(Bitmap& b) {
    for (int y = 0; y < b.height(); y++) {
        Color* row = b.row(y);
        for (int x = 0; x < b.width(); x++) {
            const Color c = row[x];
            row[x] = rgb(((c >> 16) & 0xff) * 9 / 20, ((c >> 8) & 0xff) * 9 / 20, (c & 0xff) * 9 / 20);
        }
    }
}

void text(Bitmap& b, TextRenderer& tr, int x, int y, const std::string& s, Color c, const Rect& clip,
          bool bold = false) {
    const Font f = uiFont(c);
    tr.drawText(b, x, y, clip, f, s);
    if (bold) tr.drawText(b, x + 1, y, clip, f, s);
}

int textW(TextRenderer& tr, const std::string& s) { return tr.textWidth(uiFont(), s); }
int textH(TextRenderer& tr) { return tr.textHeight(uiFont()); }

void checkMark(Bitmap& b, int x, int y, Color c) {  // 7x7 tick, top-left at (x, y)
    static const int pts[][2] = {{0, 3}, {1, 4}, {2, 5}, {3, 4}, {4, 3}, {5, 2}, {6, 1}};
    for (auto& p : pts)
        for (int t = 0; t < 2; t++) fill(b, boundsRect(x + p[0], y + p[1] + t, 1, 1), c);
}

void radioDot(Bitmap& b, int cx, int cy, Color c) {
    for (int dy = -3; dy <= 3; dy++)
        for (int dx = -3; dx <= 3; dx++)
            if (dx * dx + dy * dy <= 9) fill(b, boundsRect(cx + dx, cy + dy, 1, 1), c);
}

void arrowRight(Bitmap& b, int x, int cy, Color c) {
    for (int i = 0; i < 4; i++) fill(b, Rect{x + i, cy - 3 + i, x + i + 1, cy + 4 - i}, c);
}

void button(Bitmap& b, TextRenderer& tr, const Rect& r, const std::string& caption, bool pressed, bool isDefault) {
    fill(b, r, pressed ? kButtonDown : kButton);
    frame(b, r, isDefault ? kText : kBorder);
    const std::string t = stripAmp(caption);
    const int tw = textW(tr, t), th = textH(tr);
    text(b, tr, r.left + (r.width() - tw) / 2 + (pressed ? 1 : 0), r.top + (r.height() - th) / 2 + (pressed ? 1 : 0),
         t, kText, r);
}

// Word wrap to `width` pixels (lines split at '\n' first).
std::vector<std::string> wrap(TextRenderer& tr, const std::string& s, int width) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        std::string para = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!para.empty() && para.back() == '\r') para.pop_back();
        std::string line;
        size_t p = 0;
        while (p < para.size()) {
            size_t sp = para.find(' ', p);
            const std::string word = para.substr(p, sp == std::string::npos ? std::string::npos : sp - p);
            const std::string cand = line.empty() ? word : line + " " + word;
            if (!line.empty() && textW(tr, cand) > width) {
                out.push_back(line);
                line = word;
            } else {
                line = cand;
            }
            if (sp == std::string::npos) break;
            p = sp + 1;
        }
        out.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

}  // namespace

// ==================================================================== layers

class PlatformUiDrawn::Layer {
public:
    virtual ~Layer() = default;
    virtual bool dims() const { return false; }  // darken the editor behind it
    virtual void render(Bitmap& b, TextRenderer& tr) = 0;
    virtual void mouseDown(MouseButton, int, int, bool) {}
    virtual void mouseUp(MouseButton, int, int) {}
    virtual void mouseMove(int, int) {}
    virtual void wheel(int, int, int) {}
    virtual void keyDown(int) {}
    bool done = false;
};

namespace {

using Layer = PlatformUiDrawn::Layer;

// ---------------------------------------------------------------- popup menu
class MenuLayer final : public Layer {
public:
    MenuLayer(TextRenderer& tr, const std::vector<MenuItem>& items, int x, int y) : tr_(tr), openX_(x), openY_(y) {
        // Rows as tall as Windows menus where they fit, shorter for long menus (the program
        // list: 4 columns of 33 rows in a 430 pixel window).
        for (rowH_ = 18; rowH_ > 11; rowH_--)
            if (measure(items).second <= kFormH) break;
        open(items, x, y, -1);
    }

    int result = 0;

    void render(Bitmap& b, TextRenderer& tr) override {
        for (const Level& l : levels_) {
            fill(b, l.box, kBg);
            frame(b, l.box, kBorder);
            for (int cx : l.columnLines) fill(b, Rect{cx, l.box.top + 3, cx + 1, l.box.bottom - 3}, kLine);
            for (size_t i = 0; i < l.items->size(); i++) {
                const MenuItem& it = (*l.items)[i];
                const Rect& r = l.rects[i];
                if (it.separator) {
                    const int my = (r.top + r.bottom) / 2;
                    fill(b, Rect{r.left + 4, my, r.right - 4, my + 1}, kLine);
                    continue;
                }
                const bool hot = static_cast<int>(i) == l.hover && it.enabled;
                if (hot) fill(b, r, kHighlight);
                const Color c = it.enabled ? kText : kTextOff;
                const int cy = (r.top + r.bottom) / 2;
                if (it.checked) {
                    if (it.radio) radioDot(b, r.left + 9, cy, c);
                    else checkMark(b, r.left + 5, cy - 4, c);
                }
                text(b, tr, r.left + 18, r.top + (r.height() - textH(tr)) / 2, stripAmp(it.text), c, r, it.isDefault);
                if (!it.sub.empty()) arrowRight(b, r.right - 10, cy, c);
            }
        }
    }

    void mouseMove(int x, int y) override {
        if (std::abs(x - openX_) > 2 || std::abs(y - openY_) > 2) armed_ = true;
        int li, ii;
        if (!hit(x, y, li, ii)) return;
        hover(li, ii);
    }

    void mouseDown(MouseButton, int x, int y, bool) override {
        int li, ii;
        if (!hit(x, y, li, ii)) {
            if (inside(x, y)) return;  // a border or a gap
            result = 0;                // a click outside closes the menu
            done = true;
            return;
        }
        armed_ = true;
        hover(li, ii);
    }

    void mouseUp(MouseButton, int x, int y) override {
        int li, ii;
        if (!armed_ || !hit(x, y, li, ii)) return;
        choose(li, ii);
    }

    void keyDown(int vk) override {
        Level& l = levels_.back();
        switch (vk) {
        case kVkUp: step(l, -1); break;
        case kVkDown: step(l, 1); break;
        case kVkRight:
            if (l.hover >= 0 && !(*l.items)[static_cast<size_t>(l.hover)].sub.empty()) openSub(levels_.size() - 1);
            break;
        case kVkLeft:
            if (levels_.size() > 1) levels_.pop_back();
            break;
        case kVkReturn:
            if (l.hover >= 0) choose(static_cast<int>(levels_.size()) - 1, l.hover);
            break;
        case kVkEscape:
            if (levels_.size() > 1) {
                levels_.pop_back();
            } else {
                result = 0;
                done = true;
            }
            break;
        default: break;
        }
    }

private:
    struct Level {
        const std::vector<MenuItem>* items = nullptr;
        Rect box;
        std::vector<Rect> rects;
        std::vector<int> columnLines;
        int hover = -1;
    };

    int sepH() const { return std::max(5, rowH_ / 2); }

    // Width of the column of items [from, to): check/radio margin, text, then room for the
    // sub-menu arrow where the column has one.
    int columnWidth(const std::vector<MenuItem>& items, size_t from, size_t to) const {
        int w = 0;
        bool sub = false;
        for (size_t i = from; i < to; i++) {
            const MenuItem& it = items[i];
            if (it.separator) continue;
            w = std::max(w, textW(tr_, stripAmp(it.text)));
            sub = sub || !it.sub.empty();
        }
        return 18 + w + (sub ? 22 : 10);
    }

    // (width, height) of a menu with `items`
    std::pair<int, int> measure(const std::vector<MenuItem>& items) const {
        int w = 0, h = 0, colH = 0;
        size_t start = 0;
        for (size_t i = 0; i < items.size(); i++) {
            if (items[i].barBreak && i > 0) {
                w += columnWidth(items, start, i) + 1;
                h = std::max(h, colH);
                colH = 0;
                start = i;
            }
            colH += items[i].separator ? sepH() : rowH_;
        }
        w += columnWidth(items, start, items.size());
        h = std::max(h, colH);
        return {w + 4, h + 4};
    }

    void open(const std::vector<MenuItem>& items, int x, int y, int parentLevel) {
        Level l;
        l.items = &items;
        const auto [w, h] = measure(items);
        if (parentLevel >= 0) {  // to the right of the parent item, else to its left
            const Level& p = levels_[static_cast<size_t>(parentLevel)];
            if (x + w > kFormW) x = p.box.left - w + 2;
        }
        x = std::clamp(x, 0, std::max(0, kFormW - w));
        y = std::clamp(y, 0, std::max(0, kFormH - h));
        l.box = boundsRect(x, y, w, h);
        // item rectangles, column by column
        int cx = x + 2, cy = y + 2, colStart = 0;
        auto closeColumn = [&](int end) {
            const int cw = columnWidth(items, static_cast<size_t>(colStart), static_cast<size_t>(end));
            for (int i = colStart; i < end; i++) {
                l.rects[static_cast<size_t>(i)].left = cx;
                l.rects[static_cast<size_t>(i)].right = cx + cw;
            }
            cx += cw + 1;
        };
        l.rects.resize(items.size());
        for (size_t i = 0; i < items.size(); i++) {
            const MenuItem& it = items[i];
            if (it.barBreak && i > 0) {
                closeColumn(static_cast<int>(i));
                l.columnLines.push_back(cx - 1);
                colStart = static_cast<int>(i);
                cy = y + 2;
            }
            const int rh = it.separator ? sepH() : rowH_;
            l.rects[i].top = cy;
            l.rects[i].bottom = cy + rh;
            cy += rh;
        }
        closeColumn(static_cast<int>(items.size()));
        levels_.push_back(std::move(l));
    }

    void openSub(size_t level) {
        levels_.resize(level + 1);
        const Level& l = levels_[level];
        const Rect r = l.rects[static_cast<size_t>(l.hover)];
        open((*l.items)[static_cast<size_t>(l.hover)].sub, l.box.right - 2, r.top - 2, static_cast<int>(level));
    }

    void hover(int li, int ii) {
        levels_.resize(static_cast<size_t>(li) + 1);
        Level& l = levels_[static_cast<size_t>(li)];
        const MenuItem& it = (*l.items)[static_cast<size_t>(ii)];
        l.hover = it.separator ? -1 : ii;
        if (l.hover >= 0 && it.enabled && !it.sub.empty()) openSub(static_cast<size_t>(li));
    }

    void choose(int li, int ii) {
        const Level& l = levels_[static_cast<size_t>(li)];
        const MenuItem& it = (*l.items)[static_cast<size_t>(ii)];
        if (it.separator || !it.enabled) return;
        if (!it.sub.empty()) {
            hover(li, ii);
            return;
        }
        result = it.id;
        done = true;
    }

    void step(Level& l, int dir) {
        const int n = static_cast<int>(l.items->size());
        int i = l.hover;
        for (int k = 0; k < n; k++) {
            i = (i + dir + n) % n;
            const MenuItem& it = (*l.items)[static_cast<size_t>(i)];
            if (!it.separator && it.enabled) {
                l.hover = i;
                levels_.resize(static_cast<size_t>(&l - levels_.data()) + 1);
                return;
            }
        }
    }

    bool inside(int x, int y) const {
        for (const Level& l : levels_)
            if (l.box.contains(x, y)) return true;
        return false;
    }

    bool hit(int x, int y, int& li, int& ii) const {
        for (int L = static_cast<int>(levels_.size()) - 1; L >= 0; L--) {
            const Level& l = levels_[static_cast<size_t>(L)];
            if (!l.box.contains(x, y)) continue;
            for (size_t i = 0; i < l.rects.size(); i++)
                if (l.rects[i].contains(x, y)) {
                    li = L;
                    ii = static_cast<int>(i);
                    return true;
                }
            return false;
        }
        return false;
    }

    TextRenderer& tr_;
    std::vector<Level> levels_;
    int rowH_ = 18;
    int openX_, openY_;
    bool armed_ = false;  // the release of the click that opened the menu chooses nothing
};

// ---------------------------------------------------------------- message box
class MessageLayer final : public Layer {
public:
    MessageLayer(TextRenderer& tr, const std::string& msg, const std::string& caption, int flags) : caption_(caption) {
        const int kind = flags & 0xf;
        if (kind == kMbYesNo) buttons_ = {{"Yes", kIdYes, {}}, {"No", kIdNo, {}}};
        else if (kind == kMbOkCancel) buttons_ = {{"OK", kIdOk, {}}, {"Cancel", kIdCancel, {}}};
        else buttons_ = {{"OK", kIdOk, {}}};
        lines_ = wrap(tr, msg, 380);
        int tw = textW(tr, caption) + 24;
        for (const std::string& l : lines_) tw = std::max(tw, textW(tr, l));
        const int bw = static_cast<int>(buttons_.size()) * 83 + 8;
        const int w = std::max({tw + 32, bw + 16, 220});
        const int lineH = textH(tr) + 3;
        const int h = kCaptionH + 16 + static_cast<int>(lines_.size()) * lineH + 16 + 23 + 12;
        box_ = boundsRect((kFormW - w) / 2, (kFormH - h) / 2, w, h);
        int bx = box_.right - 8 - static_cast<int>(buttons_.size()) * 83 + 8;
        for (Button& b : buttons_) {
            b.r = boundsRect(bx, box_.bottom - 12 - 23, 75, 23);
            bx += 83;
        }
    }

    int result = kIdOk;
    bool dims() const override { return true; }

    void render(Bitmap& b, TextRenderer& tr) override {
        fill(b, box_, kBg);
        frame(b, box_, kBorder);
        const Rect cap{box_.left + 1, box_.top + 1, box_.right - 1, box_.top + kCaptionH};
        fill(b, cap, kCaption);
        text(b, tr, cap.left + 8, cap.top + (kCaptionH - textH(tr)) / 2, caption_, kText, cap);
        const int lineH = textH(tr) + 3;
        int y = box_.top + kCaptionH + 16;
        for (const std::string& l : lines_) {
            text(b, tr, box_.left + 16, y, l, kText, box_);
            y += lineH;
        }
        for (size_t i = 0; i < buttons_.size(); i++)
            button(b, tr, buttons_[i].r, buttons_[i].caption, static_cast<int>(i) == pressed_, i == 0);
    }

    void mouseDown(MouseButton, int x, int y, bool) override {
        pressed_ = -1;
        for (size_t i = 0; i < buttons_.size(); i++)
            if (buttons_[i].r.contains(x, y)) pressed_ = static_cast<int>(i);
    }
    void mouseUp(MouseButton, int x, int y) override {
        if (pressed_ >= 0 && buttons_[static_cast<size_t>(pressed_)].r.contains(x, y)) finish(pressed_);
        pressed_ = -1;
    }
    void keyDown(int vk) override {
        if (vk == kVkReturn) finish(0);
        else if (vk == kVkEscape) finish(static_cast<int>(buttons_.size()) - 1);
    }

private:
    struct Button {
        std::string caption;
        int id;
        Rect r;
    };
    void finish(int i) {
        result = buttons_[static_cast<size_t>(i)].id;
        done = true;
    }
    std::string caption_;
    std::vector<std::string> lines_;
    std::vector<Button> buttons_;
    Rect box_;
    int pressed_ = -1;
};

// ---------------------------------------------------------------- info windows
class InfoLayer final : public Layer {
public:
    InfoLayer(TextRenderer& tr, std::string caption, std::vector<std::string> lines, bool centred, bool keyCloses)
        : caption_(std::move(caption)), lines_(std::move(lines)), centred_(centred), keyCloses_(keyCloses) {
        int w = caption_.empty() ? 0 : textW(tr, caption_) + 24;
        for (const std::string& l : lines_) w = std::max(w, textW(tr, l));
        w = std::min(w + 32, kFormW - 16);
        lineH_ = textH(tr) + 2;
        const int capH = caption_.empty() ? 0 : kCaptionH;
        visible_ = std::min(static_cast<int>(lines_.size()), (kFormH - 16 - capH - 24) / lineH_);
        const int h = capH + 12 + visible_ * lineH_ + 12;
        box_ = boundsRect((kFormW - w) / 2, (kFormH - h) / 2, w, h);
    }

    bool dims() const override { return true; }

    void render(Bitmap& b, TextRenderer& tr) override {
        fill(b, box_, kBg);
        frame(b, box_, kBorder);
        int y = box_.top + 12;
        if (!caption_.empty()) {
            const Rect cap{box_.left + 1, box_.top + 1, box_.right - 1, box_.top + kCaptionH};
            fill(b, cap, kCaption);
            text(b, tr, cap.left + 8, cap.top + (kCaptionH - textH(tr)) / 2, caption_, kText, cap);
            y += kCaptionH;
        }
        const Rect clip{box_.left + 1, y, box_.right - 1, y + visible_ * lineH_};
        for (int i = 0; i < visible_ && top_ + i < static_cast<int>(lines_.size()); i++) {
            const std::string& l = lines_[static_cast<size_t>(top_ + i)];
            const int x = centred_ ? box_.left + (box_.width() - textW(tr, l)) / 2 : box_.left + 16;
            text(b, tr, x, y + i * lineH_, l, kText, clip);
        }
    }

    void mouseDown(MouseButton, int, int, bool) override { done = true; }  // a click closes it
    void wheel(int, int, int delta) override { scroll(delta > 0 ? -3 : 3); }
    void keyDown(int vk) override {
        if (vk == kVkUp) scroll(-1);
        else if (vk == kVkDown) scroll(1);
        else if (vk == kVkPrior) scroll(-visible_);
        else if (vk == kVkNext) scroll(visible_);
        else if (keyCloses_ || vk == kVkEscape || vk == kVkReturn) done = true;
    }

private:
    void scroll(int d) {
        top_ = std::clamp(top_ + d, 0, std::max(0, static_cast<int>(lines_.size()) - visible_));
    }
    std::string caption_;
    std::vector<std::string> lines_;
    bool centred_, keyCloses_;
    Rect box_;
    int lineH_ = 14, visible_ = 0, top_ = 0;
};

// ---------------------------------------------------------------- WRITE / MIDI port dialogs
class DialogLayer final : public Layer {
public:
    using Act = std::function<void(const std::function<void()>&)>;

    DialogLayer(ModalDialog& d, Act act) : d_(d), act_(std::move(act)) {
        sel_ = d.kind() == ModalDialog::Kind::SelectProgram;
        // TSelSingleForm 196x352 / TmidiSelForm 194x314 client areas (the original forms)
        const int W = sel_ ? 196 : 194, H = sel_ ? 352 : 314;
        const int left = std::clamp(kDialogCenterX - W / 2, 0, kFormW - W);
        const int top = std::clamp(kDialogCenterY - (H + kCaptionH) / 2, 0, std::max(0, kFormH - H - kCaptionH));
        box_ = boundsRect(left, top, W, H + kCaptionH);
        const int cx = left, cy = top + kCaptionH;  // client origin
        list_ = boundsRect(cx + 8, cy + (sel_ ? 45 : 8), 179, 265);
        const int btnTop = cy + (sel_ ? 319 : 280);
        ok_ = boundsRect(cx + 8, btnTop, 82, 25);
        cancel_ = boundsRect(cx + 104, btnTop, 82, 25);
        if (sel_) {
            compare_ = boundsRect(cx + 8, cy + 8 + 4, 90, 17);
            bank_ = boundsRect(cx + 112, cy + 8, 75, 25);
        }
        close_ = boundsRect(box_.right - 18, box_.top + 3, 14, 14);
        ensureVisible();
    }

    bool dims() const override { return true; }

    void render(Bitmap& b, TextRenderer& tr) override {
        if (d_.modalResult != 0) done = true;
        fill(b, box_, kBg);
        frame(b, box_, kBorder);
        const Rect cap{box_.left + 1, box_.top + 1, box_.right - 1, box_.top + kCaptionH};
        fill(b, cap, kCaption);
        text(b, tr, cap.left + 8, cap.top + (kCaptionH - textH(tr)) / 2, d_.caption, kText,
             Rect{cap.left, cap.top, close_.left - 2, cap.bottom});
        // close box
        for (int i = 3; i < 11; i++) {
            fill(b, boundsRect(close_.left + i, close_.top + i, 1, 1), kText);
            fill(b, boundsRect(close_.left + 13 - i, close_.top + i, 1, 1), kText);
        }
        // list box
        fill(b, list_, kList);
        frame(b, list_, kBorder);
        const Rect inner = rows();
        const int n = static_cast<int>(d_.items.size());
        for (int i = 0; i < visibleRows() && top_ + i < n; i++) {
            const int row = top_ + i;
            const Rect r = boundsRect(inner.left, inner.top + i * kRowH, inner.width(), kRowH);
            if (row == d_.itemIndex) fill(b, r, kHighlight);
            text(b, tr, r.left + 2, r.top + (kRowH - textH(tr)) / 2, d_.items[static_cast<size_t>(row)], kText, r);
        }
        if (n > visibleRows()) {  // scroll bar
            const Rect sb = scrollBar();
            fill(b, sb, kBg);
            const Rect th = thumb();
            fill(b, th, kButton);
            frame(b, th, kBorder);
        }
        button(b, tr, ok_, d_.okCaption, pressed_ == kOk, true);
        button(b, tr, cancel_, d_.cancelCaption, pressed_ == kCancel, false);
        if (sel_) {
            auto* s = static_cast<SelSingleDialog*>(&d_);
            const Rect box = boundsRect(compare_.left, compare_.top + 2, 13, 13);
            fill(b, box, kList);
            frame(b, box, kBorder);
            if (s->compareChecked) checkMark(b, box.left + 3, box.top + 2, kText);
            text(b, tr, box.right + 5, compare_.top + (compare_.height() - textH(tr)) / 2, "COMPARE", kText, box_);
            button(b, tr, bank_, "Bank", pressed_ == kBank, false);
        }
    }

    void mouseDown(MouseButton btn, int x, int y, bool dbl) override {
        pressed_ = -1;
        if (btn != MouseButton::Left) return;
        if (close_.contains(x, y)) {
            act([this] { d_.clickCancel(); });
            return;
        }
        if (ok_.contains(x, y)) pressed_ = kOk;
        else if (cancel_.contains(x, y)) pressed_ = kCancel;
        else if (sel_ && bank_.contains(x, y)) pressed_ = kBank;
        else if (sel_ && compare_.contains(x, y)) act([this] { static_cast<SelSingleDialog&>(d_).clickCompare(); });
        else if (static_cast<int>(d_.items.size()) > visibleRows() && scrollBar().contains(x, y)) {
            const Rect th = thumb();
            if (y < th.top) scroll(-visibleRows());
            else if (y >= th.bottom) scroll(visibleRows());
            else dragFrom_ = y, dragTop_ = top_;
        } else if (rows().contains(x, y)) {
            const int row = top_ + (y - rows().top) / kRowH;
            if (row < static_cast<int>(d_.items.size())) {
                if (dbl) act([this, row] { d_.dblClickItem(row); });
                else act([this, row] { d_.clickItem(row); });
            }
        }
    }

    void mouseMove(int, int y) override {
        if (dragFrom_ < 0) return;
        const int n = static_cast<int>(d_.items.size());
        const int track = scrollBar().height() - thumb().height();
        if (track > 0) top_ = std::clamp(dragTop_ + (y - dragFrom_) * (n - visibleRows()) / track, 0, n - visibleRows());
    }

    void mouseUp(MouseButton, int x, int y) override {
        dragFrom_ = -1;
        const int p = pressed_;
        pressed_ = -1;
        if (p == kOk && ok_.contains(x, y)) act([this] { d_.clickOk(); });
        else if (p == kCancel && cancel_.contains(x, y)) act([this] { d_.clickCancel(); });
        else if (p == kBank && bank_.contains(x, y)) act([this] { static_cast<SelSingleDialog&>(d_).clickBank(); });
    }

    void wheel(int, int, int delta) override { scroll(delta > 0 ? -3 : 3); }

    void keyDown(int vk) override {
        const int n = static_cast<int>(d_.items.size());
        int to = -1;
        switch (vk) {
        case kVkUp: to = d_.itemIndex - 1; break;
        case kVkDown: to = d_.itemIndex + 1; break;
        case kVkPrior: to = d_.itemIndex - visibleRows(); break;
        case kVkNext: to = d_.itemIndex + visibleRows(); break;
        case kVkHome: to = 0; break;
        case kVkEnd: to = n - 1; break;
        case kVkReturn:
        case kVkEscape: act([this, vk] { d_.keyDown(vk); }); return;
        default: return;
        }
        if (n == 0) return;
        to = std::clamp(to, 0, n - 1);
        if (to != d_.itemIndex) act([this, to] { d_.clickItem(to); });
    }

private:
    enum { kOk, kCancel, kBank };
    static constexpr int kRowH = 13;  // like the original list box

    void act(const std::function<void()>& f) {
        act_(f);
        ensureVisible();
        if (d_.modalResult != 0) done = true;
    }
    Rect rows() const {
        Rect r{list_.left + 2, list_.top + 2, list_.right - 2, list_.bottom - 2};
        if (static_cast<int>(d_.items.size()) > visibleRows()) r.right -= 12;
        return r;
    }
    int visibleRows() const { return (list_.height() - 4) / kRowH; }
    Rect scrollBar() const { return Rect{list_.right - 14, list_.top + 2, list_.right - 2, list_.bottom - 2}; }
    Rect thumb() const {
        const Rect sb = scrollBar();
        const int n = std::max(1, static_cast<int>(d_.items.size()));
        const int h = std::max(16, sb.height() * visibleRows() / n);
        const int range = std::max(1, n - visibleRows());
        const int y = sb.top + (sb.height() - h) * top_ / range;
        return Rect{sb.left, y, sb.right, y + h};
    }
    void scroll(int d) {
        top_ = std::clamp(top_ + d, 0, std::max(0, static_cast<int>(d_.items.size()) - visibleRows()));
    }
    void ensureVisible() {  // like scrollRowToVisible
        const int i = d_.itemIndex;
        if (i < 0) return;
        if (i < top_) top_ = i;
        else if (i >= top_ + visibleRows()) top_ = i - visibleRows() + 1;
        scroll(0);
    }

    ModalDialog& d_;
    Act act_;
    bool sel_ = false;
    Rect box_, list_, ok_, cancel_, compare_, bank_, close_;
    int top_ = 0, pressed_ = -1, dragFrom_ = -1, dragTop_ = 0;
};

}  // namespace

// ==================================================================== name box

struct PlatformUiDrawn::NameBox {
    bool active = false;
    Rect r;
    std::string text;
    size_t caret = 0;
};

// ==================================================================== PlatformUiDrawn

PlatformUiDrawn::PlatformUiDrawn(TextRenderer& text, Hooks hooks)
    : text_(text), hooks_(std::move(hooks)), name_(std::make_unique<NameBox>()) {}

PlatformUiDrawn::~PlatformUiDrawn() = default;

void PlatformUiDrawn::runLoop(Layer& layer) {
    layers_.push_back(&layer);
    if (hooks_.repaint) hooks_.repaint();
    if (hooks_.releaseEngine) hooks_.releaseEngine();
    while (!layer.done)
        if (!hooks_.runLoopStep || !hooks_.runLoopStep()) break;
    if (hooks_.acquireEngine) hooks_.acquireEngine();
    layers_.erase(std::find(layers_.begin(), layers_.end(), &layer));
    if (hooks_.repaint) hooks_.repaint();
}

int PlatformUiDrawn::popupMenu(const std::vector<MenuItem>& items, int x, int y) {
    if (items.empty()) return 0;
    MenuLayer m(text_, items, x, y);
    runLoop(m);
    return m.done ? m.result : 0;
}

int PlatformUiDrawn::messageBox(const std::string& text, const std::string& caption, int flags) {
    MessageLayer m(text_, text, caption, flags);
    runLoop(m);
    if (m.done) return m.result;
    const int kind = flags & 0xf;  // the loop could not run: the "cancel" answer
    return kind == kMbYesNo ? kIdNo : kind == kMbOkCancel ? kIdCancel : kIdOk;
}

bool PlatformUiDrawn::fileDialog(const FileDialogRequest& request, std::string& path) {
    if (!hooks_.fileDialog) return false;
    return hooks_.fileDialog(request, path);
}

bool PlatformUiDrawn::readFile(const std::string& path, std::vector<uint8_t>& data) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    data.assign(std::istreambuf_iterator<char>(f), {});
    return !data.empty();
}

bool PlatformUiDrawn::writeFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

Point PlatformUiDrawn::cursorPos() {
    Point p;
    if (hooks_.cursorPos && hooks_.cursorPos(p)) return p;
    return lastMouse_;
}

void PlatformUiDrawn::setCursorPos(Point p) {
    lastMouse_ = p;
    if (hooks_.setCursorPos) hooks_.setCursorPos(p);
}

void PlatformUiDrawn::setCursorVisible(bool visible) {
    if (hooks_.setCursorVisible) hooks_.setCursorVisible(visible);
}

void PlatformUiDrawn::focusForm() {
    if (!name_->active) return;
    name_->active = false;
    if (hooks_.nameFocus) hooks_.nameFocus(false, name_->text);
    if (hooks_.repaint) hooks_.repaint();
}

void PlatformUiDrawn::runModal(ModalDialog& dialog) {
    // ShowModal: OnShow ran in the dialog logic before the loop (showModal).
    if (dialog.modalResult != 0) return;
    const Hooks& h = hooks_;
    DialogLayer d(dialog, [&h](const std::function<void()>& f) {  // like the native dialogs
        if (h.acquireEngine) h.acquireEngine();
        f();
        if (h.pump) h.pump();
        if (h.releaseEngine) h.releaseEngine();
    });
    runLoop(d);
    if (dialog.modalResult == 0) dialog.clickCancel();  // the loop could not run
}

void PlatformUiDrawn::showModInfo(const std::vector<std::string>& lines) {
    InfoLayer l(text_, "Modulation sources", lines, false, true);
    runLoop(l);
}

void PlatformUiDrawn::showAbout(const std::string& text) {
    std::vector<std::string> lines;
    size_t s = 0;
    while (s <= text.size()) {
        const size_t e = text.find('\n', s);
        std::string l = text.substr(s, e == std::string::npos ? std::string::npos : e - s);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        if (e == std::string::npos) break;
        s = e + 1;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    InfoLayer l(text_, "", lines, true, false);
    runLoop(l);
}

void PlatformUiDrawn::beginNameEdit(int left, int top, int width, int height, const std::string& text) {
    NameBox& n = *name_;
    n.active = true;
    n.r = boundsRect(left, top, width, height);
    n.text = text;
    n.caret = text.size();
    if (hooks_.nameFocus) hooks_.nameFocus(true, text);
    if (hooks_.repaint) hooks_.repaint();
}

bool PlatformUiDrawn::nameEditing() const { return name_->active; }

bool PlatformUiDrawn::modal() const { return !layers_.empty(); }
bool PlatformUiDrawn::wantsKeyboard() const { return modal() || name_->active; }
bool PlatformUiDrawn::hasOverlay() const { return modal() || name_->active; }

void PlatformUiDrawn::render(Bitmap& frame) {
    if (name_->active) {
        const NameBox& n = *name_;
        fill(frame, n.r, 0);
        const int th = textH(text_);
        const int y = n.r.top + (n.r.height() - th) / 2;
        text(frame, text_, n.r.left + 2, y, n.text, kNameText, n.r);
        const int cx = n.r.left + 2 + textW(text_, n.text.substr(0, n.caret));
        fill(frame, Rect{cx, y, cx + 1, y + th}, kText);
    }
    for (Layer* l : layers_) {
        if (l->dims()) dim(frame);
        l->render(frame, text_);
    }
}

void PlatformUiDrawn::mouseDown(MouseButton b, int x, int y, bool doubleClick) {
    lastMouse_ = {x, y};
    if (!layers_.empty()) {
        layers_.back()->mouseDown(b, x, y, doubleClick);
    } else if (name_->active && name_->r.contains(x, y)) {  // move the caret
        NameBox& n = *name_;
        size_t best = 0;
        int bestD = 1 << 30;
        for (size_t i = 0; i <= n.text.size(); i++) {
            const int d = std::abs(n.r.left + 2 + textW(text_, n.text.substr(0, i)) - x);
            if (d < bestD) bestD = d, best = i;
        }
        n.caret = best;
    }
    if (hooks_.repaint) hooks_.repaint();
}

void PlatformUiDrawn::mouseUp(MouseButton b, int x, int y) {
    lastMouse_ = {x, y};
    if (!layers_.empty()) layers_.back()->mouseUp(b, x, y);
    if (hooks_.repaint) hooks_.repaint();
}

void PlatformUiDrawn::mouseMove(int x, int y) {
    lastMouse_ = {x, y};
    if (!layers_.empty()) {
        layers_.back()->mouseMove(x, y);
        if (hooks_.repaint) hooks_.repaint();
    }
}

void PlatformUiDrawn::wheel(int x, int y, int delta) {
    if (!layers_.empty()) layers_.back()->wheel(x, y, delta);
    if (hooks_.repaint) hooks_.repaint();
}

void PlatformUiDrawn::keyDown(int vk) {
    if (!layers_.empty()) {
        layers_.back()->keyDown(vk);
    } else if (name_->active) {
        NameBox& n = *name_;
        switch (vk) {
        case kVkBack:
            if (n.caret > 0) n.text.erase(--n.caret, 1);
            break;
        case kVkDelete:
            if (n.caret < n.text.size()) n.text.erase(n.caret, 1);
            break;
        case kVkLeft:
            if (n.caret > 0) n.caret--;
            break;
        case kVkRight:
            if (n.caret < n.text.size()) n.caret++;
            break;
        case kVkHome: n.caret = 0; break;
        case kVkEnd: n.caret = n.text.size(); break;
        case kVkReturn:
            if (hooks_.nameKey) hooks_.nameKey(kVkReturn);
            focusForm();
            break;
        case kVkEscape: focusForm(); break;
        default: break;
        }
    }
    if (hooks_.repaint) hooks_.repaint();
}

void PlatformUiDrawn::character(uint32_t c) {
    if (!layers_.empty() || !name_->active) return;
    NameBox& n = *name_;
    if (c < 0x20 || c > 0x7e || n.text.size() >= 15) return;  // like the macOS box: 15, upper case
    if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    n.text.insert(n.caret++, 1, static_cast<char>(c));
    if (hooks_.repaint) hooks_.repaint();
}

}  // namespace sq8l::gui
