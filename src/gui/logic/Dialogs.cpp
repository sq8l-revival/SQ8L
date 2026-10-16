#include "Dialogs.h"

#include <cstdint>

#include "EditorHost.h"
#include "Formatters.h"
#include "LcdData.h"
#include "Program.h"

namespace sq8l::gui {

// ==================================================================== ModalDialog

int ModalDialog::showModal(PlatformUi& ui) {
    modalResult = kMrNone;
    onShow();
    ui.runModal(*this);
    if (modalResult == kMrNone) modalResult = kMrCancel;  // closed by the window manager
    onClose();
    return modalResult;
}

void ModalDialog::clickItem(int index) {
    if (index >= -1 && index < int(items.size())) itemIndex = index;
}

void ModalDialog::dblClickItem(int index) {
    clickItem(index);
    clickOk();
}

void ModalDialog::keyDown(int key) {
    if (key == 0x0d)
        clickOk();
    else if (key == 0x1b)
        modalResult = kMrCancel;
}

// ==================================================================== TSelSingleForm

SelSingleDialog::SelSingleDialog(EditorHost& host, const std::string& cap, const std::string& ok,
                                 const std::string& cancel, int b, int p)
    : bank(b), prog(p), host_(host) {  // 0x47dd28
    caption = cap;
    if (!ok.empty()) okCaption = ok;
    if (!cancel.empty()) cancelCaption = cancel;
    // compCheck.Checked := compareOnWrite; its OnClick runs before the edit buffer is attached
    compareChecked = host.settings().compareOnWrite();
    attached_ = true;  // +0x2f4 := edit buffer
}

void SelSingleDialog::onShow() {  // FormShow
    if (first_) {
        first_ = false;
        fill();
    }
}

void SelSingleDialog::fill() {  // FUN_0047e010
    items.clear();
    const char letter = (bank >= 0 && bank < 4) ? char('A' + bank) : '?';
    for (int i = 0; i < 0x80; i++)
        items.push_back(std::string(1, letter) + delphi::intToStrZ(i, 3) + "   " +
                        host_.library().programName(SoundLibrary::index(i, bank)));
    if (prog < 0)
        prog = 0;
    else if (prog >= 0x80)
        prog = 0x7f;
    itemIndex = prog;
    clickItem(itemIndex);  // SingleList.OnClick
}

void SelSingleDialog::clickItem(int index) {  // SingleListClick
    ModalDialog::clickItem(index);
    if (compareChecked) compareOn();
}

void SelSingleDialog::clickOk() {  // okButtonClick
    prog = itemIndex;
    modalResult = kMrOk;
}

void SelSingleDialog::clickCompare() {  // compCheckClick
    compareChecked = !compareChecked;
    if (compareChecked)
        compareOn();
    else
        compareOff();
}

void SelSingleDialog::clickBank() {  // BankButtonClick
    prog = itemIndex;
    bank = (bank + 1) % 4;
    fill();
}

void SelSingleDialog::onClose() {  // FormClose
    compareOff();
    host_.setGuiSetting(3, compareChecked);
}

void SelSingleDialog::compareOn() {  // FUN_0047e1e0
    if (attached_) host_.editBuffer().compare(itemIndex, bank);
}

void SelSingleDialog::compareOff() {  // FUN_0047e20c
    if (attached_) host_.editBuffer().compareOff();
}

// ==================================================================== TmidiSelForm

MidiSelDialog::MidiSelDialog(const std::string& cap, bool out, int cur, std::vector<std::string> ports)
    : output(out), current(cur), ports_(std::move(ports)) {
    caption = cap;
}

void MidiSelDialog::onShow() {  // FormShow
    if (!first_) return;
    first_ = false;
    items.clear();
    for (size_t i = 0; i < ports_.size(); i++) items.push_back(delphi::intToStrZ(int(i), 3) + "   " + ports_[i]);
    const int n = int(ports_.size());
    if (current < 0)
        current = 0;
    else if (current >= n)
        current = n - 1;
    // TListBox.ItemIndex := current (LB_SETCURSEL: no selection when out of range, e.g. no ports)
    itemIndex = current >= 0 && current < int(items.size()) ? current : -1;
}

void MidiSelDialog::clickOk() { modalResult = itemIndex + 9; }  // okButtonClick

// ==================================================================== TModInfoForm

namespace {
const char* const kDestNames[47] = {  // DAT_004c2ef8
    "OSC1",    "OSC2",    "OSC3",    "DCA1",    "DCA2",   "DCA3",    "FILT",    "DCA4",    "PAN",     "LFO1",
    "LFO1.F",  "LFO1.PH", "LFO1.SM", "LFO2",    "LFO2.F", "LFO2.PH", "LFO2.SM", "LFO3",    "LFO3.F",  "LFO3.PH",
    "LFO3.SM", "LFO4",    "LFO4.F",  "LFO4.PH", "LFO4.SM","MAT1",    "MAT1.A",  "MAT2",    "MAT2.A",  "MAT3",
    "MAT3.A",  "ENV1.L",  "ENV1.T",  "ENV1.T1", "ENV1.SM","ENV2.L",  "ENV2.T",  "ENV2.T1", "ENV2.SM", "ENV3.L",
    "ENV3.T",  "ENV3.T1", "ENV3.SM", "ENV4.L",  "ENV4.T", "ENV4.T1", "ENV4.SM"};
}  // namespace

std::vector<std::string> modulationUsage(const Program& p) {  // FUN_0047cb80
    uint64_t bits[0x92] = {};
    // FUN_0047cb18: a used slot (amount != 0, source 0..145) sets bit `dest` of its source
    // (VEL-X counts as VEL, KEYB2 as KEYB)
    auto use = [&](int16_t src, int8_t amount, int dest) {
        if (amount == 0 || src < 0 || src > 0x91) return;
        if (src == 9) src = 8;
        else if (src == 11) src = 10;
        bits[src] |= uint64_t(1) << dest;
    };
    auto slot = [&](size_t off, int dest) { use(p.s16(off), p.s8(off + 2), dest); };
    const size_t body = ofs::Body;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 2; j++) slot(body + size_t(i) * 0x18 + 5 + size_t(j) * 3, i);
        for (int j = 0; j < 2; j++) slot(body + size_t(i) * 0x18 + 0x10 + size_t(j) * 3, i + 3);
    }
    for (int j = 0; j < 2; j++) {
        slot(body + 0xe8 + size_t(j) * 3, 6);
        const int8_t keybd = p.s8(body + 0xe6);
        if (keybd > 0) use(10, keybd, 6);
    }
    slot(body + 0x112, 7);
    slot(body + 0x115, 8);
    for (int i = 0; i < 4; i++) {
        const uint8_t modes = p.u8(body + size_t(i) * 0x10 + 0x8d);
        int am = modes & 3;
        if (am <= 1) am = 0;
        slot(body + size_t(i) * 0x10 + 0x87, 9 + 4 * i + am);
        int fm = (modes >> 2) & 3;
        if (fm <= 1) fm = 1;
        slot(body + size_t(i) * 0x10 + 0x8a, 9 + 4 * i + fm);
    }
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) slot(body + size_t(i) * 0xc + 0xc0 + size_t(j) * 3, 0x19 + 2 * i);
        slot(body + size_t(i) * 0xc + 0xc9, 0x19 + 2 * i + 1);
    }
    for (int i = 0; i < 4; i++) {
        const size_t e = body + size_t(i) * 14;
        const int8_t lv = p.s8(e + 0x4d);
        if (lv > 0) use(lv < 0x40 ? 8 : 9, lv, 0x1f + 4 * i);
        const int8_t t1v = p.s8(e + 0x4e);
        if (t1v > 0) use(8, t1v, 0x1f + 4 * i + 2 + ((p.u8(e + 0x54) >> 4) & 1));
        const int8_t tk = p.s8(e + 0x53);
        if (tk > 0) use(10, tk, 0x1f + 4 * i + 1);
    }
    std::vector<std::string> lines;
    bool sep1 = false, sep2 = false;
    for (int src = 0; src < 0x92; src++) {
        if (!bits[src]) continue;
        std::string s = data::kModSourceNames[src + 1];
        size_t k = 1;
        while (s[k - 1] == ' ' && !(int(s.size()) <= int(k))) k++;
        if (k > 1) s = s.substr(k - 1);
        s += "   ->   ";
        for (int d = 0; d < 0x2f; d++) {
            if (bits[src] & (uint64_t(1) << d)) {
                s += kDestNames[d];
                if (d < 0x2e) s += "   ";
            }
        }
        if (!sep1 && src >= 8) {
            lines.emplace_back();
            sep1 = true;
        }
        if (!sep2 && src >= 0x10) {
            lines.emplace_back();
            sep2 = true;
        }
        lines.push_back(s);
    }
    if (lines.empty()) lines.push_back("* NO MODULATIONS *");
    return lines;
}

// ==================================================================== TAboutForm

std::string aboutText() {
  static const char *const kLines[] =
  {
    "SQ8L",
    "v0.91 beta",
    "",
    "(c) 2006-2008 Siegfried Kullmann",
    "(c) 2026-20xx Claudio Tassitano, Mario Kruselj & other GitHub contributors",
    "",
    "Thanks to Rainer Buchty for SQ-80 resources, measurements and expertise!",
    "",
    "Factory sounds:",
    "Ole Jeppesen & Siegfried Kullmann",
    "Ensoniq (SQ-80 sounds in Bank D)",
    "",
    "VST plugin technology by Steinberg",
    "Audio Units technology by Apple"
  };
  std::string s;
  for (const char *l : kLines) {
    s += l;
    s += "\r\n";
  }
    return s;
}

}  // namespace sq8l::gui
