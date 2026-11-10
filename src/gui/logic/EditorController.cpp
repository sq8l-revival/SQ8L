#include "EditorController.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <utility>

#include "Dialogs.h"
#include "GuiFpu.h"
#include "Program.h"
#include "SQ8LVersion.h"
#include "SysEx.h"

namespace sq8l::gui {

namespace {

// FUN_004602f4: bank letter
char bankLetter(int bank) { return (bank >= 0 && bank < 4) ? char('A' + bank) : '?'; }

// (port, issue #45) The page buttons that form a group: four LFOs, four envelopes, three
// matrix modulators. Tags as wireControls installs them (page << 4 | sub).
const int kLfoTags[] = {111, 127, 143, 159};  // buttLfo1..4
const int kEnvTags[] = {175, 191, 207, 223};  // buttEnv1..4
const int kMatTags[] = {224, 240, 256};       // buttMat1..3

// (port, issue #45) Which group a page belongs to, or -1. The LFOs share one view between
// them and the envelopes another (`groupSub_`): LFO2's second view answers the same question
// as LFO1's, so moving to the next LFO should not also change the subject. The matrix pages
// have a single view each and so need no entry here.
int pageGroup(int page) {
    for (int t : kLfoTags)
        if ((t >> 4) == page) return 0;
    for (int t : kEnvTags)
        if ((t >> 4) == page) return 1;
    return -1;
}

// FUN_00460304: "A   (user)" ...
std::string bankName(int bank) {
    if (bank < 0 || bank > 3) return "?";
    std::string s(1, char('A' + bank));
    if (bank == 2) return s + "   (factory)";
    if (bank == 3) return s + "   (SQ-80 factory)";
    return s + "   (user)";
}

// TMenuItem.RethinkHotkeys (maAutomatic) on the visible items of one menu level: every item
// without '&' gets one before its first letter/digit not used by a previous item.
void rethinkHotkeys(std::vector<MenuItem>& items) {
    bool used[256] = {false};
    auto valid = [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'); };
    for (MenuItem& it : items) {
        if (it.separator) continue;
        const size_t amp = it.text.find('&');
        if (amp != std::string::npos && amp + 1 < it.text.size())
            used[uint8_t(std::toupper(uint8_t(it.text[amp + 1])))] = true;
    }
    for (MenuItem& it : items) {
        if (it.separator || it.text.find('&') != std::string::npos) continue;
        for (size_t i = 0; i < it.text.size(); i++) {
            const unsigned char c = uint8_t(std::toupper(uint8_t(it.text[i])));
            if (valid(c) && !used[c]) {
                used[c] = true;
                it.text.insert(i, 1, '&');
                break;
            }
        }
    }
}

}  // namespace

// ==================================================================== TMenuItem

void MenuNode::setChecked(bool c) {
    if (checked == c) return;
    checked = c;
    if (c && radio && parent) {  // TurnSiblingsOff (same GroupIndex)
        for (MenuNode& s : parent->items)
            if (&s != this && s.radio && s.groupIndex == groupIndex) s.checked = false;
    }
}

// ==================================================================== construction

EditorController::EditorController(EditorView& view, EditorHost& host, PlatformUi& ui, bool firstInstance)
    : view_(view), host_(host), ui_(ui), firstInstance_(firstInstance) {
    buildMenus();
}

EditorController::~EditorController()
{
    if (hideCursorEnabled())
    {
      showCursor();
    }
}

void EditorController::buildMenus() {
    // TPLUGEDITFORM menus (DFM): FILE, INFO, OPTIONS. Captions, tags, visibility, RadioItem.
    auto item = [](const char* caption, int tag, std::function<void(MenuNode&)> fn, bool visible = true,
                   bool radio = false) {
        MenuNode n;
        n.caption = caption;
        n.tag = tag;
        n.onClick = std::move(fn);
        n.visible = visible;
        n.radio = radio;
        return n;
    };
    auto line = [](bool visible = true, bool radio = false) {
        MenuNode n;
        n.caption = "-";
        n.visible = visible;
        n.radio = radio;
        return n;
    };
    auto& f = fileMenu_.items;
    f.push_back(item("Load library...", 0, [this](MenuNode&) { loadLibrary(); }));
    f.push_back(item("Save library...", 0, [this](MenuNode&) { saveLibrary(); }));
    f.push_back(item("Init library", 0, [this](MenuNode&) { initLibrary(); }));
    f.push_back(line());
    f.push_back(item("Load bank...", 0, [this](MenuNode&) { loadBank(); }));
    f.push_back(item("Save bank...", 0, [this](MenuNode&) { saveBank(); }));
    f.push_back(item("Init bank", 0, [this](MenuNode&) { initBank(); }));
    f.push_back(line());
    f.push_back(item("Load program...", 0, nullptr, false));
    f.back().enabled = false;
    f.push_back(item("Save program...", 0, nullptr, false));
    f.back().enabled = false;
    f.push_back(line(false));
    f.push_back(item("Request program (SQ-80/ESQ-1 SysEx)...", 0, [this](MenuNode&) { requestProgram(); }));
    f.push_back(item("Receive program (SQ-80/ESQ-1 SysEx)...", 0, [this](MenuNode&) { receiveProgram(); }));
    f.push_back(item("Send program (SQ-80/ESQ-1 SysEx)...", 0, [this](MenuNode&) { sendProgram(); }));
    f.push_back(item("Import program file (SQ-80/ESQ-1 SysEx)...", 0, [this](MenuNode&) { importProgram(); }));
    f.push_back(item("Export program file (SQ-80/ESQ-1 SysEx)...", 0, [this](MenuNode&) { exportProgram(); }));
    f.push_back(line());
    f.push_back(item("Receive bank (SQ-80/ESQ-1 SysEx)...", 0, nullptr, false));
    f.back().enabled = false;
    f.push_back(item("Send bank (SQ-80/ESQ-1 SysEx)...", 0, nullptr, false));
    f.back().enabled = false;
    f.push_back(item("Import bank file (SQ-80/ESQ-1 SysEx)...", 0, [this](MenuNode&) { importBank(); }));
    f.push_back(item("Export bank file (SQ-80/ESQ-1 SysEx)...", 0, [this](MenuNode&) { exportBank(); }));

    auto& in = infoMenu_.items;
    auto info = [this](MenuNode& n) {  // menuInfo_Click
        if (n.tag == 0)
            showModInfo();
        else if (n.tag == 1)
            showAbout();
    };
    in.push_back(item("Modulation usage...", 0, info));
    in.push_back(line());
    in.push_back(item("About...", 1, info));

    auto emu = [this](MenuNode& n) { emuModeClick(n); };
    auto& o = optionsMenu_.items;
    // (port) The OPTIONS items are indexed as they go in, so the cached MenuNode pointers at
    // the end of this function survive reordering: hard-coded positions silently rewired the
    // wrong setting whenever the layout changed.
    size_t iGain = 0, iPoly = 0, iSteal = 0, iEmu = 0, iMouse = 0, iKeyboard = 0, iSwap = 0, iConfirm = 0,
           iProgChg = 0, iMts = 0, iZoom = 0;
    // (port, issue #52) Output gain leads the menu: most soundbanks play far too loud, so a
    // cut is the first thing to reach for. Of this instance, like the polyphony below, but
    // the value last chosen is also where a newly created instance starts.
    MenuNode gain = item("Output gain", 0, nullptr);
    auto gainClick = [this](MenuNode& n) { outputCutClick(n); };
    gain.items.push_back(item("0 dB", 0, gainClick, true, true));
    gain.items.push_back(item("-6 dB", 6, gainClick, true, true));
    gain.items.push_back(item("-12 dB", 12, gainClick, true, true));
    gain.items.push_back(item("-18 dB", 18, gainClick, true, true));
    gain.items.push_back(item("-24 dB", 24, gainClick, true, true));
    iGain = o.size();
    o.push_back(gain);
    o.push_back(line());
    // (port) Polyphony opens the emulation group: how many voices there are, then what
    // happens when they run out, then the rest of the emulation. Like the emulation
    // overrides below, but of this instance only (the voices are a parameter of the
    // program), so the plugin saves it with its own state and not in SQ8L.ini.
    MenuNode poly = item("Polyphony", 0, nullptr);
    auto polyClick = [this](MenuNode& n) { polyphonyClick(n); };
    poly.items.push_back(item("Set by program   (EMU->VOICES parameter)", 0, polyClick, true, true));
    poly.items.push_back(line());
    poly.items.push_back(item("1 voice", 1, polyClick, true, true));
    poly.items.push_back(item("2 voices", 2, polyClick, true, true));
    poly.items.push_back(item("4 voices", 4, polyClick, true, true));
    poly.items.push_back(item("8 voices", 8, polyClick, true, true));
    poly.items.push_back(item("12 voices", 12, polyClick, true, true));
    poly.items.push_back(item("16 voices", 16, polyClick, true, true));
    poly.items.push_back(item("24 voices", 24, polyClick, true, true));
    poly.items.push_back(item("32 voices", 32, polyClick, true, true));
    poly.items.push_back(item("48 voices", 48, polyClick, true, true));
    poly.items.push_back(item("64 voices", 64, polyClick, true, true));
    iPoly = o.size();
    o.push_back(poly);
    iSteal = o.size();
    MenuNode steal = item("Voice stealing mode", 0, nullptr);
    steal.items.push_back(item("Set by program   (EMU->VSTEAL parameter)", 0, emu, true, true));
    steal.items.push_back(line(true, true));
    steal.items.push_back(item("HARD", 1, emu, true, true));
    steal.items.push_back(item("SOFT", 2, emu, true, true));
    o.push_back(steal);
    MenuNode emulation = item("Emulation preferences", 0, nullptr);
    MenuNode dca13 = item("DCA1-3 smoothing", 0, nullptr);
    dca13.items.push_back(item("Set by program   (EMU->DCA1-3 parameter)", 16, emu, true, true));
    dca13.items.push_back(line());
    dca13.items.push_back(item("EMU   (emulate SQ-80: smooth, less responsive)", 17, emu, true, true));
    dca13.items.push_back(item("FAST   (less accurate, more responsive)", 18, emu, true, true));
    emulation.items.push_back(dca13);
    MenuNode dca4 = item("DCA4 smoothing", 0, nullptr);
    dca4.items.push_back(item("Set by program   (EMU->DCA4 parameter)", 32, emu, true, true));
    dca4.items.push_back(line());
    dca4.items.push_back(item("EMU   (softer attack, more like SQ-80)", 33, emu, true, true));   // menu_dca4soft
    dca4.items.push_back(item("HARD   (hard attack, most responsive)", 34, emu, true, true));  // menu_dca4Hard
    emulation.items.push_back(dca4);
    MenuNode muff = item("Muffle mode", 0, nullptr);
    muff.items.push_back(item("Set by program   (EMU->MUFFLE parameter)", 48, emu, true, true));
    muff.items.push_back(line());
    muff.items.push_back(item("OFF   (crisp sound)", 49, emu, true, true));
    muff.items.push_back(item("ON  (slightly muffled sound, more like SQ-80)", 50, emu, true, true));
    emulation.items.push_back(muff);
    MenuNode dcb = item("DC blocking filter", 0, nullptr);
    dcb.items.push_back(item("Set by program   (EMU->DC-BLOCK parameter)", 64, emu, true, true));
    dcb.items.push_back(line());
    dcb.items.push_back(item("SMART   (on when needed)", 65, emu, true, true));
    dcb.items.push_back(item("ON", 66, emu, true, true));
    dcb.items.push_back(item("OFF", 67, emu, true, true));
    emulation.items.push_back(dcb);
    iEmu = o.size();
    o.push_back(emulation);
    o.push_back(line());
    // (port) One "Mouse" submenu holding the two restore options and the display scroll, which
    // the original had as a submenu plus a separate top-level item. The children are reworded
    // because the original captions only read correctly under "...is restored after...".
    MenuNode rest = item("Mouse", 0, nullptr);
    // Recorded as they go in, like the top level below, so that the cached MenuNode pointers
    // survive an item being added or moved rather than silently naming its neighbour.
    size_t iRestMouse = 0, iHideCur = 0, iNatural = 0, iRmbScr = 0;
    iRestMouse = rest.items.size();
    rest.items.push_back(item("Restore position after popup menus", 0,
                              [this](MenuNode& n) { restMouseMenuClick(n); }));
    iHideCur = rest.items.size();
    rest.items.push_back(item("Hide cursor when editing", 0,
                              [this](MenuNode& n) { hideCursorClick(n); }));
    // (port, issue #35) Inverts the wheel over a knob or a display field, for a touchpad.
    rest.items.push_back(line());
    iNatural = rest.items.size();
    rest.items.push_back(item("Natural scrolling", 0,
                              [this](MenuNode& n) { naturalScrollClick(n); }));
    rest.items.push_back(line());
    iRmbScr = rest.items.size();
    rest.items.push_back(item("Right click on display scroll pages", 0,
                              [this](MenuNode& n) { rmbScrDispClick(n); }));
    iMouse = o.size();
    o.push_back(rest);
    // (port, issue #45) The keyboard sits with the mouse: the other way of working the
    // editor's controls. Off by default - holding the keyboard takes the host's shortcuts
    // away from the user, so it is asked for rather than assumed.
    MenuNode keyb = item("Keyboard", 0, nullptr);
    size_t iKeyEnable = keyb.items.size();
    keyb.items.push_back(item("Enable keyboard control", 0,
                              [this](MenuNode& n) { keyboardControlClick(n); }));
    // (port, issue #45) Per instance, unlike the switch above: a project should open each
    // instance where it was left, and a global could not say that. Shown greyed rather than
    // hidden while keyboard control is off - a submenu should show what it holds.
    size_t iKeyMemory = keyb.items.size();
    keyb.items.push_back(item("Page memory", 0, [this](MenuNode& n) { pageMemoryClick(n); }));
    iKeyboard = o.size();
    o.push_back(keyb);
    // Port additions: the original's hidden swapProgUpDn ini key, the load prompts, MTS-ESP
    // and the window size. Polyphony leads the first group, above.
    iSwap = o.size();
    o.push_back(item("Down arrow loads next program", 0, [this](MenuNode& n) { swapProgUpDnClick(n); }));
    o.push_back(line());
    iConfirm = o.size();
    o.push_back(item("Ask before loading banks/libraries", 0, [this](MenuNode& n) { confirmLoadClick(n); }));
    // (port, issue #40) The original ignores program changes outright. Grouped with the load
    // prompt above: both decide what may replace the edit buffer behind the user's back.
    iProgChg = o.size();
    o.push_back(item("Support MIDI Program Change", 0,
                     [this](MenuNode& n) { programChangeClick(n); }));
    o.push_back(line());
    // (port) Two independent options rather than one tri-state: the parent carries the
    // checkmark of "Enable", so the state shows without opening the submenu.
    MenuNode mts = item("MTS-ESP", 0, nullptr);
    mts.items.push_back(item("Enable", 0, [this](MenuNode& n) { mtsEnableClick(n); }));
    mts.items.push_back(line());
    mts.items.push_back(item("Correct SQ-80 per-key pitch offsets", 0,
                             [this](MenuNode& n) { mtsCorrectPitchClick(n); }));
    iMts = o.size();
    o.push_back(mts);
    o.push_back(line());
    MenuNode zoom = item("Zoom", 0, nullptr);
    auto zoomClick = [this](MenuNode& n) { this->zoomClick(n); };
    for (const auto& z : {std::pair<const char*, int>{"100%", 100}, {"125%", 125}, {"150%", 150},
                          {"175%", 175}, {"200%", 200}, {"250%", 250}, {"300%", 300}})
        zoom.items.push_back(item(z.first, z.second, zoomClick, true, true));
    iZoom = o.size();
    o.push_back(zoom);

    // parents (for radio items) and the items the form references
    std::function<void(MenuNode&)> link = [&](MenuNode& n) {
        for (MenuNode& c : n.items) {
            c.parent = &n;
            link(c);
        }
    };
    for (PopupMenuDef* m : {&fileMenu_, &infoMenu_, &optionsMenu_})
        for (MenuNode& n : m->items) link(n);
    MenuNode& s = optionsMenu_.items[iSteal];
    MenuNode& e = optionsMenu_.items[iEmu];
    menuSynth_[0][0] = &s.items[0];  // menu_vocStealProg
    menuSynth_[0][1] = &s.items[2];  // menu_vocStealHard
    menuSynth_[0][2] = &s.items[3];  // menu_vocStealSoft
    menuSynth_[1][0] = &e.items[0].items[0];  // menu_dca13prog
    menuSynth_[1][1] = &e.items[0].items[2];  // menu_dca13emu
    menuSynth_[1][2] = &e.items[0].items[3];  // menu_dca13fast
    menuSynth_[2][0] = &e.items[1].items[0];  // menu_dca4prog
    menuSynth_[2][1] = &e.items[1].items[3];  // menu_dca4Hard (the "HARD" item, Tag 34)
    menuSynth_[2][2] = &e.items[1].items[2];  // menu_dca4soft (the "EMU" item, Tag 33)
    menuSynth_[3][0] = &e.items[2].items[0];  // menu_muffProg
    menuSynth_[3][1] = &e.items[2].items[2];  // menu_muffOff
    menuSynth_[3][2] = &e.items[2].items[3];  // menu_muffOn
    menuSynth_[4][0] = &e.items[3].items[0];  // menu_dcbProg
    menuSynth_[4][1] = &e.items[3].items[2];  // menu_dcbSmart
    menuSynth_[4][2] = &e.items[3].items[3];  // menu_dcbOn
    menuSynth_[4][3] = &e.items[3].items[4];  // menu_dcbOff
    MenuNode& mouse = optionsMenu_.items[iMouse];
    menuRestMouseMenu_ = &mouse.items[iRestMouse];
    menuHideCursor_ = &mouse.items[iHideCur];
    menuNaturalScroll_ = &mouse.items[iNatural];
    menuRmbScrDisp_ = &mouse.items[iRmbScr];
    menuKeyboardControl_ = &optionsMenu_.items[iKeyboard].items[iKeyEnable];
    menuPageMemory_ = &optionsMenu_.items[iKeyboard].items[iKeyMemory];
    menuOutputGain_ = &optionsMenu_.items[iGain];
    menuPolyphony_ = &optionsMenu_.items[iPoly];
    menuSwapProgUpDn_ = &optionsMenu_.items[iSwap];
    menuConfirmLoad_ = &optionsMenu_.items[iConfirm];
    menuProgramChange_ = &optionsMenu_.items[iProgChg];
    menuMts_ = &optionsMenu_.items[iMts];
    menuMtsEnable_ = &menuMts_->items[0];
    menuMtsCorrect_ = &menuMts_->items[2];
    menuZoom_ = &optionsMenu_.items[iZoom];
    pageMenu_.autoHotkeys = false;     // menuPagePopup: AutoHotkeys = maManual
    programMenu_.autoHotkeys = false;  // FUN_0043bb14
}

// ==================================================================== FormShow

void EditorController::show() {  // TplugEditForm_FormShow
    if (!firstShow_) return;
    firstShow_ = false;
    // ---- FUN_00483d9c (the parts that are not view set-up, see EditorView)
    ctr_ = std::make_unique<LcdController>(view_.lcd(), &host_.editBuffer(), this);
    ctr_->buildPages();  // FUN_0047e340 (+ the port's VOICES control)
    ctr_->numKnobs = 10;
    ctr_->setRefreshDiv(1);             // FUN_0045a730(ctr, 1)
    wireControls();
    midiIn_ = -1;
    midiOut_ = -1;
    settingsChanged();                  // FUN_00483b08
    refreshAll();                       // FUN_004842d8
    selectPage(0);                      // FUN_004849e0(form, 0)
    restorePlace();                     // (port, issue #45) ...or where this instance was left
    if (firstInstance_) {
        // (port) This build's version (SQ8LVersion.h). Dotted path: '.' is a cell attribute here,
        // not a column. Uppercase: the VFD folds lowercase. Its '(' and ')' draw as chevrons.
        std::string version = "VERSION " + std::string(kVersion);
        for (char& c : version) c = char(std::toupper(static_cast<unsigned char>(c)));
        ctr_->showMessage(-1, -1, "*** SQ8L CROSS WAVE SYNTHESIZER ***", version, 0x42, true);
        setStatus("Welcome to SQ8L");
        firstInstance_ = false;
    } else {
        setStatus("");
    }
    setVoicesText("");
}

void EditorController::wireControls() {
    // (port) How the view moves the cursor while the pointer is locked, see hideCursor.
    // Reading it back lets the lock notice a platform that refuses to move it.
    view_.warpCursor = [this](int& x, int& y) {
        ui_.setCursorPos(Point{x, y});
        const Point at = ui_.cursorPos();
        x = at.x;
        y = at.y;
    };
    // knobs (+0x2e8 / +0x2f0 / OnDblClick / OnMouseMove; OnChange from the DFM)
    for (int i = 0; i < kNumKnobs; i++) {
        Knob& k = view_.knob(i);
        // (port) The cursor goes away for the length of a turn (issue #25). Not on a knob the
        // page has no parameter for (an empty range): turning it does nothing.
        k.onEditBegin = [this](Knob& kn) {
            if (kn.maxValue() > kn.minValue()) hideCursor();
        };
        k.onEditEnd = [this](Knob&) { showCursor(); };
        // (port, issue #45) A click takes the focus, which the original only moved when a
        // value changed: "each clicked knob should take keyboard focus". Before the drag
        // starts, so a click that turns nothing still selects the parameter.
        k.onMouseDown = [this, i](Control&, MouseButton b, ShiftState, int, int) {
            LcdSubPage* sp = ctr_->currentSub();
            if (b == MouseButton::Left && sp && sp->paramForKnob(i)) highlightParam(sp->knobParam(i));
        };
        // (port, issue #45) The original opened the parameter's value menu here. That menu is
        // on the right click and on a double click on the parameter's field in the display,
        // so the gesture is better spent on the default value the Del key also gives.
        k.onDblClick = [this, i](Control&) { resetKnobParam(i); };  // 0x485148
        k.onMouseMove = [this, i](Control&, ShiftState, int, int) {  // FUN_00485170
            LcdParam* p = ctr_->currentSub() ? ctr_->currentSub()->paramForKnob(i) : nullptr;
            if (p) ctr_->cellMouseMove(p->x, p->y);
        };
        k.onChange = [this](Knob& kn) { lcdKnobChange(kn); };
    }
    // the VFD: OnMouseDown/Move/Up start/continue/end a drag of the knob of the parameter
    LcdDisplay& lcd = view_.lcd();
    lcd.onMouseDown = [this](Control&, MouseButton b, ShiftState s, int x, int y) {  // 0x48452c
        lcdDragKnob_ = -1;
        lcdToggleParam_ = -1;
        if (b != MouseButton::Left) return;
        int col, row;
        view_.lcd().cellAt(x, y, col, row);
        LcdParam* p = ctr_->paramAt(col, row);     // FUN_004844f0
        const int k = p ? p->knob : -1;
        if (k < 0 || k >= kNumKnobs)
          return;
        // (port, issue #43) A field with only two values is flipped by a click,
        // like the soft button standing in for its knob; no drag starts, so the
        // cursor stays put as well.
        if (knobIsTwoState(k)) {
          lcdToggleParam_ = p->index;
          return;
        }
        lcdDragKnob_ = k;
        view_.knob(k).beginDrag(s, x, y);
    };
    lcd.onMouseMove = [this](Control&, ShiftState s, int x, int y) {  // 0x484598
        if (lcdDragKnob_ < 0) return;
        if (view_.mouseCapture() != &view_.lcd()) {
            view_.knob(lcdDragKnob_).cancelDrag();
            lcdDragKnob_ = -1;
        } else {
            view_.knob(lcdDragKnob_).dragMove(s, x, y);
        }
    };
    lcd.onMouseUp = [this](Control &, MouseButton b, ShiftState, int x,
                           int y) { // FUN_00484570
      if (b != MouseButton::Left)
        return;
      if (lcdDragKnob_ >= 0) {
        view_.knob(lcdDragKnob_).endDrag();
        lcdDragKnob_ = -1;
      }
      // (port, issue #43) The flip lands on the button coming up over the field
      // it went down on, the way a push button fires; moving off it first
      // cancels the click.
      if (lcdToggleParam_ >= 0) {
        int col, row;
        view_.lcd().cellAt(x, y, col, row);
        LcdParam *p = ctr_->paramAt(col, row);
        if (p && p->index == lcdToggleParam_)
          toggleKnobParam(p->knob);
        lcdToggleParam_ = -1;
      }
    };
    lcd.onCellMouseDown = [this](LcdDisplay&, MouseButton b, ShiftState, int col, int row) {  // 0x4845fc
        ctr_->cellMouseDown(int(b), col, row);
    };
    lcd.onCellMouseUp = [this](LcdDisplay&, MouseButton, ShiftState, int col, int row) {  // FUN_00484624
        ctr_->cellMouseUp(col, row);
    };
    lcd.onCellMouseMove = [this](LcdDisplay&, ShiftState, int col, int row) {  // FUN_0048464c
        if (lcdDragKnob_ < 0) ctr_->cellMouseMove(col, row);
    };
    lcd.onCellDblClick = [this](LcdDisplay&, int col, int row) {  // FUN_00484680
        if (lcdDragKnob_ >= 0) {
            view_.knob(lcdDragKnob_).cancelDrag();
            lcdDragKnob_ = -1;
        }
        // (port, issue #43) Each of the two clicks has flipped a two-state
        // field already. FUN_0045b2b0 flips a range of two as well instead of
        // showing a menu of it, which would leave a double click the other way
        // round than clicking twice.
        const LcdParam *p = ctr_->paramAt(col, row);
        if (p && p->max - p->min == 1)
          return;
        ctr_->cellDoubleClick(col, row);
    };
    // numLcd / progNameEdit
    auto hint = [this](Control& c, ShiftState, int, int) { mouseMoveHint(c); };
    LcdDisplay& num = view_.numLcd();
    num.onMouseDown = [this](Control&, MouseButton b, ShiftState, int, int) {  // numLcdMouseDown
        // Port addition: the left button opens the program list too (the original: right only).
        if (b == MouseButton::Right || b == MouseButton::Left) programDblClick();
    };
    num.onMouseMove = hint;
    num.onDblClick = [this](Control&) { programDblClick(); };
    NameEdit& edit = view_.progNameEdit();
    edit.onMouseDown = [this](Control&, MouseButton b, ShiftState, int, int) {  // progNameEditMouseDown
        if (b == MouseButton::Right) programDblClick();
    };
    edit.onMouseMove = hint;
    edit.onDblClick = [this](Control&) { programDblClick(); };
    // buttons
    for (GraphButton* b : view_.buttons()) {
        b->onMouseMove = hint;
        const std::string& n = b->name();
        const int tag = [&] {
            static const std::pair<const char*, int> tags[] = {
                {"buttOsc1", 16}, {"buttOsc2", 32}, {"buttOsc3", 48}, {"buttDca1", 17}, {"buttDca2", 33},
                {"buttDca3", 49}, {"buttFilt", 64}, {"buttDca4", 80}, {"buttModes", 287}, {"buttLfo1", 111},
                {"buttLfo2", 127}, {"buttLfo3", 143}, {"buttLfo4", 159}, {"buttEnv1", 175}, {"buttEnv2", 191},
                {"buttEnv3", 207}, {"buttEnv4", 223}, {"buttMat1", 224}, {"buttMat2", 240}, {"buttMat3", 256},
                {"buttWav", 15}};
            for (auto& t : tags)
                if (n == t.first) return t.second;
            return -1;
        }();
        if (tag >= 0)
            b->onClick = [this, tag](Control&) {  // pageButtonClick
                focusForm();
                pageButton(tag, true);
            };
        else if (n == "upButton")
            b->onClick = [this](Control&) { upButtonClick(); };
        else if (n == "downButton")
            b->onClick = [this](Control&) { downButtonClick(); };
        else if (n == "writeButton")
            b->onClick = [this](Control&) { writeButtonClick(); };
        else if (n == "initButton")
            b->onClick = [this](Control&) { initButtonClick(); };
        else if (n == "sysExSendButton")
            b->onClick = [this](Control&) { sysexSendButton(); };
        else if (n == "sysExReqButton")
            b->onClick = [this](Control&) { sysexReqButton(); };
        else if (n == "buttSync")
            b->onClick = [this](Control&) { syncClick(); };
        else if (n == "buttAm")
            b->onClick = [this](Control&) { amClick(0); };
        else if (n == "buttMono")
            b->onClick = [this](Control&) { monoClick(); };
        else if (n == "BankButton")
            b->onClick = [this](Control&) { bankButtonClick(); };
        else if (n == "pscrUpButton")
            b->onClick = [this](Control&) { prevSubPage(); };
        else if (n == "pscrDownButton")
            b->onClick = [this](Control&) { nextSubPage(); };
    }
    // (port, issue #43) the soft buttons of the two-state slots: they flip
    // their parameter and show its hint, like the knob they stand in for (the
    // loop above gave them the static one)
    for (int i = 0; i < kNumKnobs; i++) {
      GraphButton &t = view_.toggle(i);
      t.onClick = [this, i](Control &) { toggleKnobParam(i); };
      t.onMouseMove = [this, i](Control &, ShiftState, int, int) {
        LcdParam *p =
            ctr_->currentSub() ? ctr_->currentSub()->paramForKnob(i) : nullptr;
        if (p)
          ctr_->cellMouseMove(p->x, p->y);
      };
    }
    // menu areas
    view_.image("menuFileImage").onClick = [this](Control& c) {
        menuButtonClick(static_cast<ImageArea&>(c), fileMenu_);
    };
    view_.image("menuOptImage").onClick = [this](Control& c) {
        menuButtonClick(static_cast<ImageArea&>(c), optionsMenu_);
    };
    view_.image("menuInfoImage").onClick = [this](Control& c) {
        menuButtonClick(static_cast<ImageArea&>(c), infoMenu_);
    };
    view_.image("menuPanicImage").onClick = [this](Control&) { panicClick(); };
    for (const char* n : {"menuFileImage", "menuOptImage", "menuInfoImage", "menuPanicImage"})
        view_.image(n).onMouseMove = hint;
    view_.statusLabel2().onMouseMove = hint;
    view_.form().onMouseMove = hint;
}

// ==================================================================== messages

void EditorController::post(uint32_t msg, uint32_t wParam, int32_t lParam) {
    queue_.push_back({msg, wParam, uint32_t(lParam)});
}

void EditorController::timerTick() { post(kMsgTimer); }  // 0x4835d0

int EditorController::pump(int limit) {
    int n = 0;
    while (!queue_.empty() && n < limit) {
        const auto m = queue_.front();
        queue_.pop_front();
        n++;
        switch (m[0]) {
        case kMsgCommand:
            dispatchCommand(int(m[1]));
            break;
        case kMsgSysexReceived:  // 0x4835ec
            showMessage("** SysEx dump received **");
            break;
        case kMsgRefresh:  // 0x483620
            refreshAll();
            break;
        case kMsgTimer:
            onTimer();
            break;
        case kMsgNotify:
            onNotify(m[1], int32_t(m[2]));
            break;
        default:
            break;
        }
    }
    return n;
}

void EditorController::idle(int ms) {
    const int n = ms / 20 > 1 ? ms / 20 : 1;
    for (int i = 0; i < n; i++) {
        timerTick();
        pump();
    }
}

void EditorController::onTimer() {  // FUN_00483628
    if (tickDivider_ < 1) {
        tickDivider_ = 2;
        updateVoices();
        updateMtsScale();  // (port)
    } else {
        tickDivider_--;
    }
    if (ctr_) ctr_->tick();
}

// Port addition: the connected MTS-ESP master's scale name in the top bar, blank when the
// option is off or no master is there. Polled, because a master can change scale at any time.
void EditorController::updateMtsScale() {
    std::string name = host_.mtsEnabled() ? host_.mtsScaleName() : std::string();
    if (name == mtsScale_) return;
    mtsScale_ = std::move(name);
    view_.setMtsText(mtsScale_);
}

void EditorController::updateVoices() {  // 0x483804
    const int total = host_.voicesMax();
    const int digits = delphi::numDigits(total);
    const int used = host_.voicesUsed();
    setVoicesText(delphi::strWidth(used, digits) + "/" + delphi::intToStr(total));
}

void EditorController::onNotify(uint32_t wParam, int32_t) {  // FUN_0048365c
    const uint32_t kind = wParam >> 16;
    if (kind == 0)
        settingsChanged();
    else if (kind == 1)
        refreshAll();
}

void EditorController::settingsChanged() {  // FUN_00483b08
    const Settings& s = host_.settings();
    libDir_ = host_.pluginDirectory();
    sysexDir_ = host_.pluginDirectory();
    bool b = s.restoreMouseAfterMenu();
    if (b != restMouseMenu_) {
        setRestMouseMenu(b);
        menuRestMouseMenu_->setChecked(b);
    }
    b = s.hideCursorWhileEditing();
    if (b != hideCursor_) {
        setHideCursor(b);
        menuHideCursor_->setChecked(b);
    }
    keyCaptMode_ = s.keyCaptureMode();
    swapProgUpDn_ = s.swapProgramUpDown();
    if (menuSwapProgUpDn_) menuSwapProgUpDn_->setChecked(swapProgUpDn_);
    if (menuConfirmLoad_) menuConfirmLoad_->setChecked(s.confirmLoading());
    if (menuProgramChange_) menuProgramChange_->setChecked(host_.programChangeEnabled());
    naturalScroll_ = s.naturalScrolling();
    if (menuNaturalScroll_) menuNaturalScroll_->setChecked(naturalScroll_);
    keyboardControl_ = s.keyboardControl();
    if (menuKeyboardControl_) menuKeyboardControl_->setChecked(keyboardControl_);
    // (port, issue #45) Switching it on has to land the focus somewhere, or the keys would do
    // nothing until the user clicked a knob - which is the friction the feature exists to
    // remove. Switching it off leaves the selection alone: it is the original's underline too,
    // and focusKnob() already reports nothing while the switch is off.
    if (menuPageMemory_) {
        menuPageMemory_->setChecked(host_.pageMemory());
        menuPageMemory_->enabled = keyboardControl_;  // it serves nothing while the keys are off
    }
    if (keyboardControl_ && focusKnob() < 0) focusFirstKnob();
    view_.setFocusKnob(focusKnob());  // switching it off takes the rectangle away
    if (menuMtsEnable_) {
        const bool on = host_.mtsEnabled();
        menuMtsEnable_->setChecked(on);
        menuMts_->setChecked(on);  // the parent shows the state without opening the submenu
        menuMtsCorrect_->setChecked(host_.mtsCorrectPitch());
    }
    if (menuZoom_)  // a size dragged to a value that is not in the list checks nothing
        for (MenuNode& n : menuZoom_->items) n.checked = n.tag == host_.zoom();
    if (menuPolyphony_) {
        const int ovr = host_.polyphonyOverride();
        for (MenuNode& n : menuPolyphony_->items)
            if (n.caption != "-") n.checked = n.tag == ovr;
    }
    if (menuOutputGain_) {
        const int cut = host_.outputCut();
        for (MenuNode& n : menuOutputGain_->items)
            if (n.caption != "-") n.checked = n.tag == cut;
    }
    b = s.rightClickScrollsDisplay();
    if (b != rmbScroll_) {
        rmbScroll_ = b;
        menuRmbScrDisp_->setChecked(b);
    }
    for (int i = 0; i < 5; i++) {
        const int v = s.synth[i];
        int slot;
        if (i == 4)
            slot = v == 0 ? 0 : v == 1 ? 1 : v == 2 ? 2 : 3;
        else
            slot = v == 0 ? 0 : v == 1 ? 1 : 2;
        menuSynth_[i][slot]->setChecked(true);
    }
}

void EditorController::setRestMouseMenu(bool b) {  // FUN_00483aac
    restMouseMenu_ = b;
    if (ctr_) ctr_->restoreMouse = b;
}

void EditorController::setHideCursor(bool b) {
    hideCursor_ = b;
    if (!b) showCursor();  // turned off in the middle of a turn
}

// The knobs report the start and the end of every turn, a cancelled one included, so the
// cursor can never be left hidden; the flag keeps the platform calls balanced.
void EditorController::hideCursor() {
    if (!hideCursor_ || cursorHidden_) return;
    cursorHidden_ = true;
    // A hidden cursor still travels with the mouse: it would run into the edge of the screen
    // and the knob would stop turning, and it would come back wherever the turn ended. The
    // lock pins it where the turn started and gives the knob the distance travelled instead,
    // so the movement has no bounds at all.
    const Point at = ui_.cursorPos();
    ui_.setCursorVisible(false);
    view_.lockPointer(at.x, at.y);
}

void EditorController::showCursor() {
    if (!cursorHidden_) return;
    cursorHidden_ = false;
    view_.unlockPointer();  // parks it where the turn started, before it can be seen again
    ui_.setCursorVisible(true);
}

// ==================================================================== display refresh

void EditorController::refreshAll() {  // FUN_004842d8
    ctr_->refresh(false);
    ctr_->drawAll();
    refreshProgram();
    lcdDrawn();  // FUN_004847a4
}

void EditorController::refreshProgram() {  // FUN_00484304
    EditBuffer& eb = host_.editBuffer();
    view_.progNameEdit().setText(eb.name(0));
    std::string s(1, bankLetter(eb.bank()));
    s += delphi::intToStrZ(prog(), 3);
    view_.numLcd().writeText(0, 0, s, 0);
    view_.numLcd().update();
    updateLeds(false);
}

void EditorController::updateLeds(bool redraw) {  // FUN_004843cc
    const Program& p = host_.editBuffer().current();
    view_.ledSync().setValue(p.s8(ofs::Sync) < 1 ? 0.0f : 1.0f);
    const int am = p.s8(ofs::AmMod);
    view_.ledAm().setValue(am == 1 ? 1.0f : 0.0f);
    view_.ledMono().setValue(p.s8(ofs::Mono) < 1 ? 0.0f : 1.0f);
    if (redraw) {
        ctr_->refresh(false);
        ctr_->drawAll();
    }
}

void EditorController::setStatus(const std::string& s) { view_.setStatusText(s); }      // FUN_0048367c
void EditorController::setVoicesText(const std::string& s) { view_.setVoicesText(s); }  // FUN_004836cc

void EditorController::showMessage(const std::string& text, int ticks) {  // FUN_0048371c
    if (ticks < 0) ticks = 0x1c;
    ctr_->showMessage(-1, -1, text, "", ticks);
}

void EditorController::showMessage2(const std::string& l1, const std::string& l2, int ticks) {  // FUN_00483784
    if (ticks < 0) ticks = 0x1c;
    ctr_->showMessage(-1, -1, l1, l2, ticks);
}

// ==================================================================== ClcdCtr callbacks

void EditorController::lcdKnobValue(int knob, int value) {  // LAB_004846c4
    if (knob >= 0 && knob < kNumKnobs) view_.knob(knob).setValue(float(value));
}

void EditorController::lcdKnobRange(int knob, int min, int max, bool faded) {  // FUN_00484700
    if (knob < 0 || knob >= kNumKnobs) return;
    Knob& k = view_.knob(knob);
    // (port, issue #43) A parameter with only two values gets a soft button in
    // place of the knob: one click flips it, instead of a drag long enough to
    // carry the value across. The knob stays as the model behind it (range,
    // value), it is simply not shown.
    const bool twoState = max - min == 1;
    view_.toggle(knob).setVisible(twoState);
    k.setVisible(!twoState);
    // (port) An empty range is how a page without a parameter for this knob is reported
    // (updateKnobRanges sends 0, 0): the knob is drawn faded then, see issue #24. So is one
    // whose parameter has no effect for now (`faded`), but that one still turns: the value
    // matters again once the parameter does (HUMAN of a synced LFO, issue #42).
    k.setActive(max > min && !faded);
    k.setMinValue(float(min));
    k.setMaxValue(float(max));
    // (port) How far the mouse travels to cross the whole range. The original:
    // Trunc(Single((max - min + 1) * 1.5625)) clamped to 40..180, in Delphi's Single
    // arithmetic; nothing in it needs a float, so this is the same shape in integers.
    //
    // The bounds are the port's, and the floor is the point of them. At 1.5625 px per entry
    // every parameter between 25 and 83 entries came out at the same ~0.64 units per pixel,
    // which means the smaller its range the fewer pixels crossed all of it: RES (32 entries)
    // swept end to end in 50 px while FREQ (128) took the full 180, and the small ranges felt
    // twitchy beside the big ones. A floor of 144 px gives every range a comparable sweep
    // rather than a comparable speed; the cap is 208 so that the widest ones stay settled.
    const int px = (max - min + 1) * 2;
    k.setMaxPixDist(std::clamp(px, 144, 208));
}

void EditorController::lcdDrawn() {  // FUN_004847a4
    view_.lcd().writeText(0, 1, host_.editBuffer().modified() ? "*C*" : "   ", 0);
}

void EditorController::lcdParamMouseDown(int param) { highlightParam(param); }  // SUB_00484800

void EditorController::highlightParam(int param) {  // SUB_00484800
    LcdSubPage* sp = ctr_->currentSub();
    if (!sp) return;
    for (LcdParam& p : sp->params) {
        p.highlight = p.index == param && p.knob >= 0;
        ctr_->drawParam(p);
    }
    // (port, issue #45) The focus rectangle follows the underline, because they are the same
    // selection: this is the one place it moves, so it is the one place that has to say so.
    view_.setFocusKnob(focusKnob());
    savePlace();
}

void EditorController::lcdHint(int param) {  // LAB_00484874
    LcdSubPage* sp = ctr_->currentSub();
    LcdParam* p = sp ? sp->param(param) : nullptr;
    if (p) setStatus(p->hint);
}

void EditorController::lcdSaveMouse() { saveMouse(ctr_.get()); }        // FUN_00483a5c(form, ctr)
void EditorController::lcdRestoreMouse() { restoreMouse(ctr_.get()); }  // FUN_00483a84(form, ctr)

void EditorController::lcdScrollArrows(bool down, bool up) {  // LAB_00484908
    view_.button("pscrDownButton").setAniIdx(down ? 3 : 2);
    view_.button("pscrUpButton").setAniIdx(up ? 1 : 0);
}

void EditorController::lcdParamChanged(int page, int param) {  // FUN_0048489c
    if (unsigned(page - 0x13) < 2) {
        // pages 19/20 (zone blocks of the extension buffer) do not exist in the SQ8L
        LcdPage* pg = ctr_->page(page);
        LcdParam* p = pg ? pg->sub().param(param) : nullptr;
        if (p && p->knob == 0) {
            host_.editBuffer().resetExtZone(page - 0x13);
            ctr_->refresh(true);
        }
    } else {
        updateLeds(false);
    }
}

void EditorController::lcdShowPopup(const LcdPopup& popup) {  // TPopupMenu.Popup (+0x80)
    commands_.clear();
    lcdPopupCommands_ = true;
    const int id = ui_.popupMenu(popup.items, popup.x, popup.y);
    if (id > 0) post(kMsgCommand, uint32_t(id));
}

// ==================================================================== helpers

void EditorController::saveMouse(const void* owner) {  // FUN_00483a5c
    if (restMouseMenu_) mouseJump_.save(ui_, owner ? owner : this);
}

void EditorController::restoreMouse(const void* owner) {  // FUN_00483a84
    if (restMouseMenu_) mouseJump_.restore(ui_, owner ? owner : this);
}

void EditorController::focusForm() { ui_.focusForm(); }  // FUN_00483a3c

int EditorController::bank() const { return host_.editBuffer().bank(); }           // FUN_0048495c
int EditorController::prog() const { return host_.editBuffer().programNumber(); }  // FUN_004849a0

void EditorController::selectProgram(int p) {  // FUN_004849b0
    if (p < 0)
        p = 0;
    else if (p > 0x7f)
        p = 0x7f;
    host_.editBuffer().selectProgram(p, -1);
}

void EditorController::selectBank(int b) {  // FUN_0048496c
    int m = b % 4;  // Delphi mod: sign of the dividend
    host_.editBuffer().setBank(m);
    host_.editBuffer().selectProgram(prog(), -1);
}

void EditorController::pageButton(int tag, bool cycle) {  // FUN_00484a10
    const int pg = tag >> 4, sub = tag & 0xf;
    const int curPage = ctr_->pageIndex();
    const int curSub = ctr_->subPage();
    if (curPage == pg) {
        if (sub == 0xf || curSub == sub) {
            if (cycle) nextSubPage();
        } else {
            selectSubPage(sub);
        }
    } else if (sub == 0xf) {
        // (port, issue #45) A page of a group opens on the group's shared view rather than on
        // the one it was last left on: the LFOs read as one page with four sources behind it,
        // and the envelopes likewise. A button that names its own view (buttDca1 and the rest)
        // is unaffected - it goes where it says.
        const int g = pageGroup(pg);
        if (g >= 0)
            selectPageSub(pg, groupSub_[g]);
        else
            selectPage(pg);
    } else {
        selectPageSub(pg, sub);
    }
}

// (port, issue #45) After any page change: if the display is on a page of a group, that is
// the group's view from now on. One place, so every route - the buttons, the chords, the
// scroll arrows, the page popup - keeps it true.
void EditorController::rememberGroupSub() {
    const int g = pageGroup(ctr_->pageIndex());
    if (g >= 0) groupSub_[g] = ctr_->subPage();
}

// (port, issue #45) Every page change goes through these four, so that the focus lands on the
// new page's first slot rather than being left on a parameter that is no longer shown.
// LcdController clears the selection when it builds a page, so without this the keys and the
// focus rectangle would have nothing to work on until the user clicked something.
void EditorController::selectPage(int page) {
    ctr_->selectPage(page);
    rememberGroupSub();
    focusFirstKnob();
}

void EditorController::selectPageSub(int page, int sub) {
    ctr_->selectPageSub(page, sub);
    rememberGroupSub();
    focusFirstKnob();
}

void EditorController::selectSubPage(int sub) {
    ctr_->selectSubPage(sub);
    rememberGroupSub();
    focusFirstKnob();
}

void EditorController::nextSubPage() {
    ctr_->nextSubPage();
    rememberGroupSub();
    focusFirstKnob();
}

void EditorController::prevSubPage() {
    ctr_->prevSubPage();
    rememberGroupSub();
    focusFirstKnob();
}

// ==================================================================== popups

void EditorController::popup(PopupMenuDef& menu, int x, int y) {
    // TPopupMenu.Popup: build the Windows menu from the visible items, TrackPopupMenu; the
    // chosen item's Click comes back as WM_COMMAND.
    commands_.clear();
    lcdPopupCommands_ = false;
    std::function<std::vector<MenuItem>(std::vector<MenuNode>&)> conv = [&](std::vector<MenuNode>& nodes) {
        std::vector<MenuItem> out;
        for (MenuNode& n : nodes) {
            if (!n.visible) continue;
            MenuItem it;
            it.separator = n.caption == "-";
            it.text = it.separator ? std::string() : n.caption;
            it.checked = n.checked;
            it.enabled = n.enabled;
            it.radio = n.radio;
            it.isDefault = n.isDefault;
            it.barBreak = n.barBreak;
            commands_.push_back(&n);
            it.id = int(commands_.size());
            if (!n.items.empty()) it.sub = conv(n.items);
            out.push_back(std::move(it));
        }
        return out;
    };
    std::vector<MenuItem> items = conv(menu.items);
    if (menu.autoHotkeys) rethinkHotkeys(items);
    const int id = ui_.popupMenu(items, x, y);
    if (id > 0) post(kMsgCommand, uint32_t(id));
}

void EditorController::dispatchCommand(int id) {
    if (lcdPopupCommands_) {
        ctr_->popupClicked(id);
        return;
    }
    if (id < 1 || id > int(commands_.size())) return;
    MenuNode* n = commands_[size_t(id - 1)];
    if (n && n->enabled && n->onClick) n->onClick(*n);
}

void EditorController::menuButtonClick(ImageArea& image, PopupMenuDef& menu) {  // menuButtonClick
    focusForm();
    popup(menu, image.left(), image.top() + image.height() + 1);
}

void EditorController::contextMenu(int x, int y) {
    // TWinControl.WMContextMenu of the form: a graphic control under the point with an
    // auto-popup menu (menuOptImage / menuPanicImage: OPTIONS, menuInfoImage: INFO; the FILE
    // menu has AutoPopup = False) shows it at the mouse; otherwise OnContextPopup.
    for (const char* n : {"menuPanicImage", "menuOptImage", "menuInfoImage", "menuFileImage"}) {
        ImageArea& im = view_.image(n);
        if (im.visible() && im.bounds().contains(x, y)) {
            if (std::string(n) == "menuInfoImage") {
                popup(infoMenu_, x, y);
                return;
            }
            if (std::string(n) != "menuFileImage") {
                popup(optionsMenu_, x, y);
                return;
            }
            break;
        }
    }
    // (port) A right click on a knob opens its parameter's value menu, the same menu a double
    // click opens. Double clicking still works; the page popup this replaces is on the
    // display and the whole background anyway. A knob the page does not use (faded, issue
    // #24) has no menu to show, so it falls through to the page popup like any other spot.
    for (int i = 0; i < kNumKnobs; i++) {
        const Knob& k = view_.knob(i);
        if (!k.visible() || !k.bounds().contains(x, y)) continue;
        LcdParam* p = ctr_->currentSub() ? ctr_->currentSub()->paramForKnob(i) : nullptr;
        if (p) {
            ctr_->cellDoubleClick(p->x, p->y);
            return;
        }
        break;
    }
    displayPanelContextPopup(x, y);
}

void EditorController::displayPanelContextPopup(int x, int y) {  // TplugEditForm_displayPanelContextPopup
    const LcdDisplay& lcd = view_.lcd();
    bool scroll = false;
    if (rmbScroll_ && ctr_->subPageCount() > 1 && lcd.left() <= x && lcd.top() <= y && x < lcd.left() + lcd.width() &&
        y < lcd.top() + lcd.height())
        scroll = true;
    if (scroll) {
        nextSubPage();  // pscrDownButton.OnClick
    } else {
        saveMouse(nullptr);
        buildPagePopup();
        popup(pageMenu_, x, y);
    }
}

void EditorController::buildPagePopup() {  // FUN_00484ab0
    pageMenu_.items.clear();
    MenuNode mod;
    mod.caption = "Modulation usage...";
    mod.tag = -1;
    auto click = [this](MenuNode& n) {  // FUN_00484a84
        if (n.tag == -1) {
            showModInfo();
            return;
        }
        pageButton(n.tag, false);
        restoreMouse(nullptr);
    };
    mod.onClick = click;
    pageMenu_.items.push_back(mod);
    MenuNode line;
    line.caption = "-";
    pageMenu_.items.push_back(line);
    int group = ctr_->page(0) ? ctr_->page(0)->sub().editor : 0;
    for (int i = 0; i < ctr_->pageCount(); i++) {
        LcdPage* pg = ctr_->page(i);
        if (!pg) continue;
        if (pg->sub().editor != group) {
            pageMenu_.items.push_back(line);
            group = pg->sub().editor;
        }
        const int n = int(pg->subs.size());
        for (int j = 0; j < n; j++) {
            const bool sel = ctr_->pageIndex() == i && ctr_->subPage() == j;
            const LcdSubPage& sp = pg->subs[size_t(j)];
            if (sp.flags[1] < 1) {
                MenuNode it;
                it.caption = delphi::stringOfChar(' ', sp.flags[0] << 2) + sp.title;
                it.onClick = click;
                it.tag = i * 16 + (j & 0xf);
                it.radio = true;
                it.checked = sel;
                pageMenu_.items.push_back(it);
            } else if (sel && !pageMenu_.items.empty()) {
                // the last item (already in the menu): Checked := True turns its radio
                // siblings off
                MenuNode& last = pageMenu_.items.back();
                if (!last.checked) {
                    last.checked = true;
                    if (last.radio)
                        for (MenuNode& s : pageMenu_.items)
                            if (&s != &last && s.radio) s.checked = false;
                }
            }
        }
    }
}

void EditorController::programDblClick() {  // TplugEditForm_programDblClick
    saveMouse(nullptr);
    focusForm();
    buildProgramPopup();
    const LcdDisplay& num = view_.numLcd();
    popup(programMenu_, num.left(), num.top() + num.height() + 1);
}

void EditorController::buildProgramPopup() {  // FUN_00484eb4
    programMenu_.items.clear();
    const int curBank = bank();
    const char letter = bankLetter(curBank);
    auto bankItems = [&](int i) {  // FUN_00484d70
        MenuNode it;
        if (i < 4) {
            it.caption = "Bank " + bankName(i);
            it.onClick = [this](MenuNode& n) {  // 0x485084
                if (n.tag >= 0 && n.tag < 4) {
                    selectBank(n.tag);
                    programDblClick();
                }
            };
            it.tag = i;
            it.groupIndex = 1;  // FUN_00439c00 = SetGroupIndex
            it.radio = true;
            it.checked = i == bank();
        }
        if (i > 0) it.barBreak = true;
        programMenu_.items.push_back(it);
        MenuNode line;
        line.caption = "-";
        programMenu_.items.push_back(line);
    };
    int col = 0;
    bankItems(0);
    int left = 0x20;
    for (int p = 0; p < 0x80; p++) {
        const int idx = host_.editBuffer().libraryIndex(p, -1);
        MenuNode it;
        it.caption = std::string(1, letter) + delphi::intToStrZ(p, 3) + "   " + host_.library().programName(idx);
        it.onClick = [this](MenuNode& n) {  // 0x48506c
            selectProgram(n.tag);
            restoreMouse(nullptr);
        };
        it.tag = p;
        it.checked = p == prog();
        it.radio = true;
        if (left > 0) {
            left--;
        } else {
            left = 0x1f;
            bankItems(++col);
        }
        programMenu_.items.push_back(it);
    }
    // (Checked is set before the items are added: no TurnSiblingsOff, the bank item and the
    // program item can both be checked.)
}

// ==================================================================== event handlers

void EditorController::mouseMoveHint(Control& sender) {  // TplugEditForm_MouseMoveHint
    if (&sender == lastHint_) return;
    setStatus(sender.hint);
    lastHint_ = &sender;
}

void EditorController::lcdKnobChange(Knob& k) {  // TplugEditForm_lcdKnobChange
    int tag = -1;
    for (int i = 0; i < kNumKnobs; i++)
        if (&view_.knob(i) == &k) tag = i;
    const int v = fpu::roundEven(k.value());  // 0x47abac (IntTruncMode = False: Round)
    ctr_->knobChanged(tag, v);
    LcdSubPage* sp = ctr_->currentSub();
    if (sp) highlightParam(sp->knobParam(tag));
}

// (port, issue #35) The knob slot under a point, or -1: a slot showing its soft button has no
// knob to turn (its knob is hidden), and one the page has no parameter for turns nothing.
int EditorController::knobAt(int x, int y) const {
  for (int i = 0; i < kNumKnobs; i++) {
    const Knob &k = view_.knob(i);
    if (!k.visible() || !k.bounds().contains(x, y))
      continue;
    LcdSubPage *sp = ctr_->currentSub();
    return (sp && sp->paramForKnob(i)) ? i : -1;
  }
  return -1;
}

// (port, issue #35) The wheel over a knob, or over the display field of one. `delta` is the
// platform's own, whatever its scale: what one notch of it is worth is learned rather than
// assumed, since a device may report a whole detent at once or a trickle of small values.
void EditorController::wheel(int x, int y, double delta, uint32_t keys) {
  if (delta == 0.0)
    return;
  int knob = knobAt(x, y);
  LcdDisplay &lcd = view_.lcd();
  if (knob < 0 && lcd.visible() && lcd.bounds().contains(x, y)) {
    int col, row;
    lcd.cellAt(x - lcd.left(), y - lcd.top(), col, row);
    const LcdParam *p = ctr_->paramAt(col, row);
    // A field of two values is a button now, not something to scroll (issue #43).
    if (p && p->knob >= 0 && p->max - p->min != 1)
      knob = p->knob;
  }
  if (knob < 0) { // nothing here answers the wheel
    wheelAccum_ = 0.0;
    wheelKnob_ = -1;
    return;
  }
  // The smallest movement ever reported is taken for one notch, so a wheel that sends a
  // detent at a time and a touchpad that sends a trickle both step once per notch.
  const double mag = std::fabs(delta);
  wheelStep_ = wheelStep_ == 0.0 ? mag : std::min(wheelStep_, mag);
  // Half a notch left on one knob must not carry into the next one the pointer meets.
  if (knob != wheelKnob_) {
    wheelAccum_ = 0.0;
    wheelKnob_ = knob;
  }
  wheelAccum_ += delta;
  const int by = (keys & MK_CONTROL_) ? kWheelCtrlStep : 1;
  while (std::fabs(wheelAccum_) >= wheelStep_ * 0.75) {
    const int dir = wheelAccum_ < 0 ? -1 : 1;
    wheelAccum_ -= dir * wheelStep_;
    stepKnobParam(knob, (naturalScroll_ ? -dir : dir) * by);
  }
}

// (port, issue #35) Move the parameter of a knob slot by `by`, the way a turn moves it.
void EditorController::stepKnobParam(int knob, int by) {
  LcdSubPage *sp = ctr_->currentSub();
  LcdParam *p = sp ? sp->paramForKnob(knob) : nullptr;
  if (!p)
    return;
  setKnobParamValue(knob, ctr_->currentValue(p->index) + by);
}

// (port, issue #45) Put the parameter of a knob slot at `v`, clamped, the way a turn leaves
// it: the knob is the value's home, then knobChanged takes the same route a turn takes.
void EditorController::setKnobParamValue(int knob, int v) {
  LcdSubPage *sp = ctr_->currentSub();
  LcdParam *p = sp ? sp->paramForKnob(knob) : nullptr;
  if (!p)
    return;
  if (v < p->min)
    v = p->min;
  if (v > p->max)
    v = p->max;
  if (v == ctr_->currentValue(p->index))
    return;
  lcdKnobValue(knob, v);
  ctr_->knobChanged(knob, v);
  highlightParam(sp->knobParam(knob));
}

// (port, issue #45) Put the parameter of a knob slot back to what INIT would give it. A slot
// the page has no parameter for has nothing to reset.
void EditorController::resetKnobParam(int knob) {
  LcdSubPage *sp = ctr_->currentSub();
  LcdParam *p = sp ? sp->paramForKnob(knob) : nullptr;
  if (!p)
    return;
  setKnobParamValue(knob, ctr_->defaultValue(*sp, *p));
}

// (port, issue #45) Focus is the selected parameter, the one the display underlines: there is
// no second piece of state for it, so the underline and the keys cannot disagree.
int EditorController::focusKnob() const {
  if (!keyboardControl_)
    return -1;
  const LcdSubPage *sp = ctr_->currentSub();
  if (!sp)
    return -1;
  for (const LcdParam &p : sp->params)
    if (p.highlight && p.knob >= 0)
      return p.knob;
  return -1;
}

// (port, issue #45) The slots the current sub-page gives a parameter, in the order Tab walks
// them. A slot the page does not use turns nothing and cannot be reached, the rule knobAt
// already applies to the mouse.
std::vector<int> EditorController::focusSlots() const {
  std::vector<int> slots;
  const LcdSubPage *sp = ctr_->currentSub();
  if (!sp)
    return slots;
  for (int i = 0; i < kNumKnobs; i++)
    for (const LcdParam &p : sp->params)
      if (p.knob == i) {
        slots.push_back(i);
        break;
      }
  return slots;
}

// (port, issue #45) Put the focus on the first slot the page uses. A page change lands here,
// so that with keyboard control on something is always focused: the rectangle is always on
// screen, and the keys always have something to move.
void EditorController::focusFirstKnob() {
  view_.setFocusKnob(-1);  // a page with no slot to focus must not keep the old rectangle
  if (!keyboardControl_)
    return;
  LcdSubPage *sp = ctr_->currentSub();
  const std::vector<int> slots = focusSlots();
  if (!sp || slots.empty())
    return;
  // (port, issue #45) With page memory on, a sub-page visited before comes back to the
  // parameter it was left on; one never visited, and every page with the switch off, starts
  // at the first slot it uses.
  int knob = slots.front();
  if (host_.pageMemory()) {
    const int i = EditorPlace::index(ctr_->pageIndex(), ctr_->subPage());
    const int8_t was = i >= 0 ? place_.focusBySub[i] : int8_t(-1);
    for (int s : slots)
      if (s == was) knob = s;  // (a page can be rebuilt with fewer slots than it had)
  }
  highlightParam(sp->knobParam(knob));
}

// (port, issue #45) Hand the place to the engine, which keeps it and saves what fits of it in
// the host chunk. Called after anything that moves the display or the focus, so that whenever
// the host asks for the state it is current.
void EditorController::savePlace() {
  place_.page = int8_t(ctr_->pageIndex());
  place_.sub = int8_t(ctr_->subPage());
  place_.knob = int8_t(focusKnob());
  place_.groupSub[0] = int8_t(groupSub_[0]);
  place_.groupSub[1] = int8_t(groupSub_[1]);
  const int i = EditorPlace::index(place_.page, place_.sub);
  if (i >= 0 && place_.knob >= 0) place_.focusBySub[i] = place_.knob;
  host_.setEditorPlace(place_);
}

// (port, issue #45) FormShow: open where this instance was left, if it was left anywhere and
// page memory is on. The welcome message and the rest of show() follow, so a new instance is
// unaffected - its place has nothing in it.
void EditorController::restorePlace() {
  place_ = host_.editorPlace();
  if (!host_.pageMemory() || !place_.valid()) return;
  groupSub_[0] = place_.groupSub[0] & 3;
  groupSub_[1] = place_.groupSub[1] & 3;
  selectPageSub(place_.page, place_.sub);
}

// (port, issue #45) Tab / Shift+Tab: the next or previous slot the page uses, wrapping.
void EditorController::focusStep(int dir) {
  LcdSubPage *sp = ctr_->currentSub();
  const std::vector<int> slots = focusSlots();
  if (!sp || slots.empty())
    return;
  const int at = focusKnob();
  size_t i = 0;
  for (size_t k = 0; k < slots.size(); k++)
    if (slots[k] == at) {
      i = (k + size_t(dir) + slots.size()) % slots.size();
      break;
    }
  highlightParam(sp->knobParam(slots[i]));
}

// (port, issue #45) Alt+L / Alt+E / Alt+M walk their group of pages. No single button does
// this, so unlike the other chords it is not one button's click: each press takes the next
// page of the group, entering at the first from anywhere outside it. `cycle` is false because
// the views of a page are Alt+V's job - the two are separate axes once inside a group.
void EditorController::cycleGroup(const int* tags, int n) {
  const int page = ctr_->pageIndex();
  int next = -1;
  for (int i = 0; i < n; i++)
    if ((tags[i] >> 4) == page) {
      next = (i + 1) % n;
      break;
    }
  if (next < 0) {
    // Entering the group from outside it. With page memory on that is the page of this group
    // the instance was last on; otherwise the group's first, every time.
    next = 0;
    const int g = groupIndex(tags);
    if (host_.pageMemory() && g >= 0 && place_.groupPage[g] >= 0)
      for (int i = 0; i < n; i++)
        if ((tags[i] >> 4) == place_.groupPage[g]) next = i;
  }
  pageButton(tags[next], false);
  const int g = groupIndex(tags);
  if (g >= 0) place_.groupPage[g] = int8_t(ctr_->pageIndex());
  savePlace();
}

// (port, issue #45) Which of the three groups a tag table is, for place_.groupPage.
int EditorController::groupIndex(const int* tags) {
  if (tags == kLfoTags) return 0;
  if (tags == kEnvTags) return 1;
  if (tags == kMatTags) return 2;
  return -1;
}

bool EditorController::keyDown(int vk, uint32_t mods) {
  if (!keyboardControl_)
    return false;
  // (port, issue #45) The chords. A chord that stands for a page button *is* that button,
  // `cycle` and all: pressing Alt+1 twice cycles the page's views exactly as clicking buttOsc1
  // twice does, so the keyboard and the mouse cannot grow different ideas of what a page is.
  if (mods == kKeyModAlt) {
    switch (vk) {
    case '1': pageButton(16, true); return true;      // buttOsc1
    case '2': pageButton(32, true); return true;      // buttOsc2
    case '3': pageButton(48, true); return true;      // buttOsc3
    case 'F': pageButton(64, true); return true;      // buttFilt
    case 'A': pageButton(80, true); return true;      // buttDca4
    case 'X': pageButton(15, true); return true;      // buttWav (OSC1-3 Mix)
    case 'U': pageButton(287, true); return true;     // buttModes
    case 'V': nextSubPage(); return true;             // the page's views, as the scroll arrow
    case 'L': cycleGroup(kLfoTags, 4); return true;
    case 'E': cycleGroup(kEnvTags, 4); return true;
    case 'M': cycleGroup(kMatTags, 3); return true;
    default: return false;
    }
  }
  // (port, issue #45) Shift and the arrows move through the library. The on-screen UP/DOWN
  // buttons have "Down arrow loads next program" to say which way they go, because a button
  // with an arrow on it is ambiguous; an arrow key is not, so it ignores that setting.
  // Shift, not Ctrl: macOS spends all four Ctrl+arrows on Mission Control and Spaces, so no
  // application sees them there. Not Alt either - the right Alt of many layouts is AltGr.
  if (mods == kKeyModShift) {
    switch (vk) {
    case kVkRight: selectProgram(prog() + 1); return true;
    case kVkLeft: selectProgram(prog() - 1); return true;
    case kVkDown: selectBank(bank() + 1); return true;
    case kVkUp: selectBank(bank() - 1); return true;
    default: break;  // Shift+Tab is below
    }
  }
  if (vk == kVkTab && (mods & ~kKeyModShift) == 0) {
    focusStep((mods & kKeyModShift) ? -1 : 1);
    return true;
  }
  // The keys below are the bare ones. A modifier we have no chord for does not make one of
  // them: Cmd+Up is the Mac user reaching for the bank, and it must not nudge a knob instead.
  if (mods != 0)
    return false;
  const int knob = focusKnob();
  LcdSubPage *sp = ctr_->currentSub();
  LcdParam *p = (knob >= 0 && sp) ? sp->paramForKnob(knob) : nullptr;
  if (!p)
    return false;
  switch (vk) {
  case kVkUp:
    stepKnobParam(knob, 1);
    return true;
  case kVkDown:
    stepKnobParam(knob, -1);
    return true;
  case kVkPrior:
    stepKnobParam(knob, kWheelCtrlStep);
    return true;
  case kVkNext:
    stepKnobParam(knob, -kWheelCtrlStep);
    return true;
  case kVkHome:
    setKnobParamValue(knob, p->min);
    return true;
  case kVkEnd:
    setKnobParamValue(knob, p->max);
    return true;
  case kVkDelete:
    resetKnobParam(knob);
    return true;
  default:
    return false;  // the host's, like every key the editor has no use for
  }
}

bool EditorController::knobIsTwoState(int knob) const { // (port, issue #43)
  if (knob < 0 || knob >= kNumKnobs)
    return false;
  LcdSubPage *sp = ctr_->currentSub();
  const LcdParam *p = sp ? sp->paramForKnob(knob) : nullptr;
  return p && p->max - p->min == 1;
}

void EditorController::toggleKnobParam(int knob) { // (port, issue #43)
  if (knob < 0 || knob >= kNumKnobs)
    return;
  LcdSubPage *sp = ctr_->currentSub();
  LcdParam *p = sp ? sp->paramForKnob(knob) : nullptr;
  if (!p || p->max - p->min != 1)
    return;
  const int v = ctr_->currentValue(p->index) == p->min ? p->max : p->min;
  // The knob behind the button is the value's home, as it is after a turn: set
  // it first, then take the same route a turn takes (knobChanged does not write
  // it back).
  lcdKnobValue(knob, v);
  ctr_->knobChanged(knob, v);
  highlightParam(sp->knobParam(knob));
}

void EditorController::panicClick() {  // menuPanicImageClick
    host_.panic();
    showMessage("*** All voices reset ***");
}

void EditorController::upButtonClick() { selectProgram(swapProgUpDn_ ? prog() - 1 : prog() + 1); }
void EditorController::downButtonClick() { selectProgram(swapProgUpDn_ ? prog() + 1 : prog() - 1); }

void EditorController::bankButtonClick() { selectBank(bank() + 1); }  // BankButtonClick

void EditorController::initButtonClick() {  // initButtonClick
    host_.editBuffer().initProgram();
    refreshAll();
    showMessage("** Program initialized **");
}

void EditorController::writeButtonClick() {  // writeButtonClick
    int b = bank(), p = prog();
    const bool ok = selectProgramDialog("Write/Compare", "Write", "", b, p);
    if (ok && SoundLibrary::isBankWriteProtected(b)) {
        ui_.messageBox("Bank is write protected.", "Error", kMbOk);
        return;
    }
    EditBuffer& eb = host_.editBuffer();
    eb.setName(0, view_.progNameEdit().text);
    if (ok && eb.writeProgram(p, b)) {
        eb.setBank(b);
        showMessage("** Program written **");
        refreshAll();
        return;
    }
    showMessage("** Write aborted **");
}

void EditorController::syncClick() {  // buttSyncClick
    Program& p = host_.editBuffer().current();
    p.setS8(ofs::Sync, p.s8(ofs::Sync) < 1 ? 1 : 0);
    host_.editBuffer().setModified(true);
    updateLeds(true);
}

void EditorController::amClick(int tag) {  // buttAmFmClick
    Program& p = host_.editBuffer().current();
    if (tag == 0) {
        const int v = p.s8(ofs::AmMod);
        if (v == 0 || v == 2)
            p.setS8(ofs::AmMod, 1);
        else if (v == 1)
            p.setS8(ofs::AmMod, 0);
    }
    host_.editBuffer().setModified(true);
    updateLeds(true);
}

void EditorController::monoClick() {  // buttMonoClick
    Program& p = host_.editBuffer().current();
    p.setS8(ofs::Mono, p.s8(ofs::Mono) < 1 ? 1 : 0);
    host_.editBuffer().setModified(true);
    updateLeds(true);
}

void EditorController::nameEditFocus(bool focused, const std::string& text) {  // FUN_004839a4
    if (focused) {
        keyCapture_ = keyCaptMode_ >= 0;  // CkeyCapt.start(form, keyCaptMode)
    } else {
        keyCapture_ = false;              // CkeyCapt.stop(form)
        host_.editBuffer().setName(0, text);
    }
}

void EditorController::nameEditKeyDown(int key) {  // progNameEditKeyDown
    if (key == 0x0d) focusForm();
}

// ==================================================================== OPTIONS / INFO

void EditorController::emuModeClick(MenuNode& item) {  // menu_emuModeClick
    if (item.checked) return;
    item.setChecked(true);
    host_.setSynthSetting(item.tag >> 4, item.tag & 0xf);
}

void EditorController::restMouseMenuClick(MenuNode& item) {  // menu_restMouseMenuClick
    item.setChecked(!item.checked);
    host_.setGuiSetting(0, item.checked);
    setRestMouseMenu(host_.settings().restoreMouseAfterMenu());
}

void EditorController::hideCursorClick(MenuNode& item) {  // menu_restMouseKnobClick
    item.setChecked(!item.checked);
    host_.setGuiSetting(1, item.checked);
    setHideCursor(host_.settings().hideCursorWhileEditing());
}

void EditorController::rmbScrDispClick(MenuNode& item) {  // menu_rmbScrDispClick
    item.setChecked(!item.checked);
    host_.setGuiSetting(5, item.checked);
    rmbScroll_ = host_.settings().rightClickScrollsDisplay();
}

// Port addition: the original's swapProgUpDn ini key (readme E.7), now also in OPTIONS.
void EditorController::swapProgUpDnClick(MenuNode& item) {
    item.setChecked(!item.checked);
    host_.setGuiSetting(4, item.checked);
    swapProgUpDn_ = host_.settings().swapProgramUpDown();
}

// Port addition: this instance's playable voices, 0 = set by the program (EMU->VOICES).
void EditorController::polyphonyClick(MenuNode& item) {
    if (item.checked) return;
    item.setChecked(true);
    host_.setPolyphonyOverride(item.tag);
    updateVoices();
}

// Port addition (issue #52): cut this instance's output by the item's dB.
void EditorController::outputCutClick(MenuNode& item) {
    if (item.checked) return;
    item.setChecked(true);
    host_.setOutputCut(item.tag);
}

// Port addition: size of the editor window ([port] zoom), applied by the host.
void EditorController::zoomClick(MenuNode& item) {
    if (item.checked) return;
    item.setChecked(true);
    host_.setZoom(item.tag);
}

// Port addition: follow an MTS-ESP master.
void EditorController::mtsEnableClick(MenuNode& item) {
    item.setChecked(!item.checked);
    if (menuMts_) menuMts_->setChecked(item.checked);
    host_.setMtsEnabled(item.checked);
}

// Port addition (issue #40): respond to MIDI program changes, which the original never did.
void EditorController::programChangeClick(MenuNode& item) {
    item.setChecked(!item.checked);
    host_.setProgramChangeEnabled(item.checked);
}

// Port addition: correct the SQ-80's own per-key pitch offsets to the master's frequencies
// (absolute tuning) instead of applying only its deviation from 12-ET.
void EditorController::mtsCorrectPitchClick(MenuNode& item) {
    item.setChecked(!item.checked);
    host_.setMtsCorrectPitch(item.checked);
}

// Port addition: ask before loading a library, a bank or a SysEx bank ([port] confirmLoad).
void EditorController::confirmLoadClick(MenuNode& item) {
    item.setChecked(!item.checked);
    host_.setPortSetting(0, item.checked ? 1 : 0);
}

// Port addition (issue #35): invert the wheel ([port] naturalScroll).
void EditorController::naturalScrollClick(MenuNode& item) {
    item.setChecked(!item.checked);
    host_.setPortSetting(1, item.checked ? 1 : 0);
}

void EditorController::keyboardControlClick(MenuNode& item) {  // (port, issue #45)
    item.setChecked(!item.checked);
    host_.setPortSetting(Settings::kPortKeyboardControl, item.checked ? 1 : 0);
}

void EditorController::pageMemoryClick(MenuNode& item) {  // (port, issue #45)
    item.setChecked(!item.checked);
    host_.setPageMemory(item.checked);
}

void EditorController::showModInfo() {  // FUN_0047c9c0
    ui_.showModInfo(modulationUsage(host_.editBuffer().current()));
}

void EditorController::showAbout() { ui_.showAbout(aboutText()); }  // 0x47d260

// ==================================================================== dialogs

bool EditorController::selectProgramDialog(const std::string& caption, const std::string& okCaption,
                                           const std::string& cancelCaption, int& b, int& p) {  // 0x47dd28
    SelSingleDialog dlg(host_, caption, okCaption, cancelCaption, b, p);
    keyCapture_ = keyCaptMode_ >= 0;  // CkeyCapt.start(dialog)
    const int r = dlg.showModal(ui_);
    keyCapture_ = false;
    if (r == 1) {
        b = dlg.bank;
        p = dlg.prog;
        return true;
    }
    b = -1;
    p = -1;
    return false;
}

}  // namespace sq8l::gui
