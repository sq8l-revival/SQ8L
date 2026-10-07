// C API: the editor logic (src/gui/logic) on a C++ EditorView, for tests/test_gui_logic*.py
// (differential comparison with the original editor running in the emulator).
//
// A Box holds the engine side (SoundLibrary, EditBuffer, Settings), the EditorView, a
// scripted PlatformUi and the EditorController. Input is injected as Win32 mouse messages in
// form coordinates (like the oracle driver); the state is exported as JSON.
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "Dialogs.h"
#include "EditBuffer.h"
#include "EditorController.h"
#include "EditorView.h"
#include "Formatters.h"
#include "LcdControl.h"
#include "Settings.h"
#include "SoundLibrary.h"

#include "capi_export.h"

using namespace sq8l;
using namespace sq8l::gui;

namespace {

uint32_t fbits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += char(c);
        } else if (c < 0x20 || c >= 0x7f) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else {
            o += char(c);
        }
    }
    return o + "\"";
}

std::string menuJson(const std::vector<MenuItem>& items) {
    std::string s = "[";
    for (size_t i = 0; i < items.size(); i++) {
        const MenuItem& m = items[i];
        if (i) s += ",";
        s += "{\"text\":" + jstr(m.text) + ",\"checked\":" + (m.checked ? "true" : "false") +
             ",\"disabled\":" + (m.enabled ? "false" : "true") + ",\"separator\":" + (m.separator ? "true" : "false") +
             ",\"radio\":" + (m.radio ? "true" : "false") + ",\"break\":" + (m.barBreak ? "true" : "false") +
             ",\"default\":" + (m.isDefault ? "true" : "false");
        if (!m.sub.empty()) s += ",\"sub\":" + menuJson(m.sub);
        s += "}";
    }
    return s + "]";
}

struct Box;

// ---------------------------------------------------------------- engine side
struct TestHost : EditorHost {
    SoundLibrary lib;
    EditBuffer eb{&lib};
    Settings cfg;
    EditorController* ctl = nullptr;
    int maxVoices = 8, usedVoices = 0;
    std::string dir = "C:\\VST\\SQ8L\\";
    std::ostringstream log;

    TestHost() {
        eb.listener = [this](EditBuffer::Event e, const Program*) {
            if (e == EditBuffer::Event::ProgramLoaded && ctl) ctl->post(kMsgNotify, 0x10002, 0);
        };
    }
    EditBuffer& editBuffer() override { return eb; }
    SoundLibrary& library() override { return lib; }
    const Settings& settings() override { return cfg; }
    void setGuiSetting(int i, bool v) override {
        if (cfg.setGuiBool(i, v) && ctl) ctl->post(kMsgNotify, 0, i);
    }
    void setSynthSetting(int i, int v) override {
        if (cfg.setSynth(i, v) && ctl) ctl->post(kMsgNotify, 0, 0x10 + i);
    }
    // The differential tests compare with the original: port additions off unless asked.
    bool extensions = false;
    bool portExtensions() override { return extensions; }
    void setPortSetting(int i, int v) override {
        if (cfg.setPort(i, v) && ctl) ctl->post(kMsgNotify, 0, 0x20 + i);
    }
    // (port) OPTIONS -> Polyphony: per instance, so the plugin keeps it in its own state
    // (here just a field), not in SQ8L.ini.
    int polyOverride = 0;
    int polyphonyOverride() override { return polyOverride; }
    void setPolyphonyOverride(int voices) override { polyOverride = voices; }
    void panic() override { log << "panic\n"; }
    int voicesUsed() override { return usedVoices; }
    int voicesMax() override { return maxVoices; }
    std::string pluginDirectory() override { return dir; }
    // MIDI ports (names), whether opening succeeds; sent data goes to the event log
    std::vector<std::string> inPorts, outPorts;
    bool midiOpenOk = false;
    std::ostringstream* events = nullptr;
    std::vector<std::string> midiInPorts() override { return inPorts; }
    std::vector<std::string> midiOutPorts() override { return outPorts; }
    void midiCloseAll() override {}
    bool midiOpenOut(int port) override { return midiOpenOk && port >= 0 && port < int(outPorts.size()); }
    void midiSendOut(const std::vector<uint8_t>& d) override {
        if (!events) return;
        std::string hex;
        char b[4];
        for (uint8_t c : d) {
            std::snprintf(b, sizeof b, "%02x", c);
            hex += b;
        }
        *events << "{\"ev\":\"midi\",\"data\":\"" << hex << "\"}\n";
    }
    bool midiOpenIn(int port) override { return midiOpenOk && port >= 0 && port < int(inPorts.size()); }
};

// ---------------------------------------------------------------- platform side
struct TestUi : PlatformUi {
    std::ostringstream events;               // JSON objects, one per line
    std::deque<std::vector<int>> menuChoices;   // item index paths ({} = dismiss)
    std::deque<int> msgAnswers;
    std::deque<std::string> fileAnswers;       // "" = cancel
    std::map<std::string, std::vector<uint8_t>> files;
    std::deque<std::vector<std::string>> modalScripts;
    Point cursor;
    std::function<void()> pumpDuringModal;
    int formX = 0, formY = 0;                 // screen position of the form client area

    int popupMenu(const std::vector<MenuItem>& items, int x, int y) override {
        events << "{\"ev\":\"popup\",\"x\":" << x + formX << ",\"y\":" << y + formY << ",\"tree\":" << menuJson(items)
               << "}\n";
        if (menuChoices.empty()) return 0;
        std::vector<int> path = menuChoices.front();
        menuChoices.pop_front();
        const std::vector<MenuItem>* level = &items;
        const MenuItem* it = nullptr;
        for (int idx : path) {
            if (idx < 0 || idx >= int(level->size())) return 0;
            it = &(*level)[size_t(idx)];
            level = &it->sub;
        }
        // like the oracle: separators, disabled items and sub-menus can't be chosen
        if (!it || it->separator || !it->enabled || !it->sub.empty()) return 0;
        return it->id;
    }
    int messageBox(const std::string& text, const std::string& caption, int flags) override {
        int r = flags == kMbYesNo ? kIdYes : kIdOk;
        if (!msgAnswers.empty()) {
            r = msgAnswers.front();
            msgAnswers.pop_front();
        }
        events << "{\"ev\":\"msgbox\",\"text\":" << jstr(text) << ",\"caption\":" << jstr(caption)
               << ",\"flags\":" << flags << ",\"result\":" << r << "}\n";
        return r;
    }
    bool fileDialog(const FileDialogRequest& rq, std::string& path) override {
        std::string a;
        if (!fileAnswers.empty()) {
            a = fileAnswers.front();
            fileAnswers.pop_front();
        }
        events << "{\"ev\":\"filedialog\",\"save\":" << (rq.save ? "true" : "false") << ",\"dir\":" << jstr(rq.initialDir)
               << ",\"filter\":" << jstr(rq.filter) << ",\"ext\":" << jstr(rq.defaultExt)
               << ",\"name\":" << jstr(rq.fileName) << ",\"result\":" << jstr(a) << "}\n";
        if (a.empty()) return false;
        path = a;
        return true;
    }
    bool readFile(const std::string& path, std::vector<uint8_t>& data) override {
        auto f = files.find(path);
        if (f == files.end()) return false;
        data = f->second;
        return !data.empty();
    }
    bool writeFile(const std::string& path, const std::vector<uint8_t>& data) override {
        files[path] = data;
        events << "{\"ev\":\"write\",\"path\":" << jstr(path) << ",\"size\":" << data.size() << "}\n";
        return true;
    }
    Point cursorPos() override { return cursor; }
    void setCursorPos(Point p) override {
        cursor = p;
        events << "{\"ev\":\"setcursor\",\"x\":" << p.x + formX << ",\"y\":" << p.y + formY << "}\n";
    }
    void runModal(ModalDialog& d) override {
        std::vector<std::string> script;
        if (!modalScripts.empty()) {
            script = modalScripts.front();
            modalScripts.pop_front();
        }
        std::string items = "[";
        for (size_t i = 0; i < d.items.size(); i++) items += (i ? "," : "") + jstr(d.items[i]);
        items += "]";
        events << "{\"ev\":\"modal\",\"caption\":" << jstr(d.caption) << ",\"ok\":" << jstr(d.okCaption)
               << ",\"cancel\":" << jstr(d.cancelCaption) << ",\"index\":" << d.itemIndex << ",\"items\":" << items;
        if (d.kind() == ModalDialog::Kind::SelectProgram)
            events << ",\"compare\":" << (static_cast<SelSingleDialog&>(d).compareChecked ? "true" : "false");
        events << "}\n";
        // the modal loop: posted messages are dispatched before each user action (like the
        // original's Application.HandleMessage loop)
        for (const std::string& a : script) {
            if (d.modalResult != kMrNone) break;
            if (pumpDuringModal) pumpDuringModal();
            if (a == "ok")
                d.clickOk();
            else if (a == "cancel")
                d.clickCancel();
            else if (a == "compare" && d.kind() == ModalDialog::Kind::SelectProgram)
                static_cast<SelSingleDialog&>(d).clickCompare();
            else if (a == "bank" && d.kind() == ModalDialog::Kind::SelectProgram)
                static_cast<SelSingleDialog&>(d).clickBank();
            else if (a.rfind("item ", 0) == 0)
                d.clickItem(std::stoi(a.substr(5)));
            else if (a.rfind("dbl ", 0) == 0)
                d.dblClickItem(std::stoi(a.substr(4)));
            else if (a.rfind("key ", 0) == 0)
                d.keyDown(std::stoi(a.substr(4)));
        }
        if (d.modalResult == kMrNone) {
            if (pumpDuringModal) pumpDuringModal();
            d.clickCancel();  // closed (WM_CLOSE)
        }
    }
    void showModInfo(const std::vector<std::string>& lines) override {
        std::string s = "[";
        for (size_t i = 0; i < lines.size(); i++) s += (i ? "," : "") + jstr(lines[i]);
        events << "{\"ev\":\"modinfo\",\"lines\":" << s << "]}\n";
    }
    void showAbout(const std::string& text) override {
        events << "{\"ev\":\"about\",\"text\":" << jstr(text) << "}\n";
    }
};

struct Box {
    TestHost host;
    EditorView view;
    TestUi ui;
    std::unique_ptr<EditorController> ctl;
    bool autoContextMenu = false;

    explicit Box(int first, bool extensions = false) {
        host.extensions = extensions;
        ctl = std::make_unique<EditorController>(view, host, ui, first != 0);
        host.ctl = ctl.get();
        ui.pumpDuringModal = [this] { ctl->pump(); };
        host.events = &ui.events;
        view.onContextMenu = [this](Control&, int x, int y) {
            if (autoContextMenu) ctl->contextMenu(x, y);
        };
    }
};

Box* B(void* v) { return static_cast<Box*>(v); }

int copyOut(const std::string& s, char* out, int32_t max) {
    if (out && max > 0) {
        std::strncpy(out, s.c_str(), size_t(max));
        out[max - 1] = 0;
    }
    return int32_t(s.size());
}

std::string lcdJson(const LcdDisplay& l) {
    std::string s = "[";
    for (int r = 0; r < l.rows(); r++) {
        s += r ? ",[" : "[";
        for (int c = 0; c < l.cols(); c++) {
            const auto& cell = l.cell(c, r);
            s += (c ? ",[" : "[") + std::to_string(cell.ch) + "," + std::to_string(cell.attr) + "]";
        }
        s += "]";
    }
    return s + "]";
}

}  // namespace

// ------------------------------------------------------------------ lifecycle

SQ8L_API void* sq8l_gl_new(int first) { return new Box(first); }
// With the port's additions (EditorHost::portExtensions), for tests/test_gui_extensions.py.
SQ8L_API void* sq8l_gl_new_ext(int first) { return new Box(first, true); }
// (port) OPTIONS -> Polyphony, the playable voices of this instance (0 = set by the program)
SQ8L_API int32_t sq8l_gl_poly_override(void* v) { return B(v)->host.polyOverride; }
SQ8L_API int32_t sq8l_gl_port_setting(void* v, int32_t i) {
    return i >= 0 && i < Settings::kNumPort ? B(v)->host.cfg.port[i] : -1;
}
SQ8L_API void sq8l_gl_free(void* v) { delete B(v); }

// engine state (before show): library image (512 x 540 bytes), header (0x3c), clean flag;
// edit buffer state image (EditBuffer::kStateSize); settings gui[6] + synth[5].
SQ8L_API void sq8l_gl_set_library(void* v, const uint8_t* progs, const uint8_t* header, int clean) {
    std::memcpy(B(v)->host.lib.raw(), progs, size_t(SoundLibrary::kNumPrograms) * kProgramSize);
    (void)header;
    B(v)->host.lib.setClean(clean != 0);
}
SQ8L_API void sq8l_gl_set_editbuffer(void* v, const uint8_t* img) { B(v)->host.eb.loadState(img); }
SQ8L_API void sq8l_gl_set_settings(void* v, const int32_t* gui, const int32_t* synth) {
    for (int i = 0; i < Settings::kNumGui; i++) B(v)->host.cfg.gui[i] = gui[i];
    for (int i = 0; i < Settings::kNumSynth; i++) B(v)->host.cfg.synth[i] = synth[i];
}
SQ8L_API void sq8l_gl_set_host(void* v, int maxVoices, int usedVoices, const char* dir) {
    B(v)->host.maxVoices = maxVoices;
    B(v)->host.usedVoices = usedVoices;
    if (dir) B(v)->host.dir = dir;
}
SQ8L_API void sq8l_gl_set_form_origin(void* v, int x, int y) {
    B(v)->ui.formX = x;
    B(v)->ui.formY = y;
}

// FormShow, then the first paint (like the original's window: the LEDs fit their size to the
// GIF frame when painted, which matters for the mouse hit test)
SQ8L_API void sq8l_gl_show(void* v) {
    B(v)->ctl->show();
    Bitmap frame;
    B(v)->view.render(frame);
}

// ------------------------------------------------------------------ input / time

SQ8L_API void sq8l_gl_mouse(void* v, int32_t msg, int32_t x, int32_t y, int32_t keys) {
    Box* b = B(v);
    b->ui.cursor = Point{x, y};
    EditorView& ev = b->view;
    switch (msg) {
        case 0x200: ev.mouseMove(x, y, uint32_t(keys)); break;
        case 0x201: ev.mouseDown(MouseButton::Left, x, y, uint32_t(keys)); break;
        case 0x202: ev.mouseUp(MouseButton::Left, x, y, uint32_t(keys)); break;
        case 0x203: ev.mouseDoubleClick(MouseButton::Left, x, y, uint32_t(keys)); break;
        case 0x204: ev.mouseDown(MouseButton::Right, x, y, uint32_t(keys)); break;
        case 0x205: ev.mouseUp(MouseButton::Right, x, y, uint32_t(keys)); break;
        case 0x206: ev.mouseDoubleClick(MouseButton::Right, x, y, uint32_t(keys)); break;
        default: break;
    }
}
SQ8L_API void sq8l_gl_context_menu(void* v, int32_t x, int32_t y) {
    B(v)->ui.cursor = Point{x, y};
    B(v)->ctl->contextMenu(x, y);
}
// What the plugin UI does after a press that opened a menu or a dialog (SQ8LUI::onMouse):
// the modal loop keeps the button up, so the control the press was made on must not keep the
// mouse. Scripted input drives the logic without that shell, so a test says it itself.
SQ8L_API void sq8l_gl_cancel_mouse_mode(void* v) { B(v)->view.cancelMouseMode(); }
SQ8L_API void sq8l_gl_idle(void* v, int32_t ms) { B(v)->ctl->idle(ms); }
SQ8L_API void sq8l_gl_timer(void* v) { B(v)->ctl->timerTick(); }
SQ8L_API int32_t sq8l_gl_pump(void* v) { return B(v)->ctl->pump(); }
SQ8L_API void sq8l_gl_post(void* v, uint32_t msg, uint32_t wp, int32_t lp) { B(v)->ctl->post(msg, wp, lp); }
SQ8L_API void sq8l_gl_name_focus(void* v, int focused, const char* text) {
    B(v)->view.progNameEdit().text = text;
    B(v)->ctl->nameEditFocus(focused != 0, text);
}
SQ8L_API void sq8l_gl_name_key(void* v, int key) { B(v)->ctl->nameEditKeyDown(key); }
SQ8L_API void sq8l_gl_set_name_text(void* v, const char* text) { B(v)->view.progNameEdit().text = text; }

// scripted platform answers
SQ8L_API void sq8l_gl_menu_choice(void* v, const int32_t* path, int32_t n) {
    B(v)->ui.menuChoices.emplace_back(path, path + n);
}
SQ8L_API void sq8l_gl_msg_answer(void* v, int32_t r) { B(v)->ui.msgAnswers.push_back(r); }
SQ8L_API void sq8l_gl_file_answer(void* v, const char* path) { B(v)->ui.fileAnswers.emplace_back(path); }
// drop the scripted answers not used (popup choices, message boxes, file and modal dialogs)
// MIDI ports: names separated by '\n'; ok = opening succeeds
SQ8L_API void sq8l_gl_set_midi(void* v, const char* ins, const char* outs, int ok) {
    auto split = [](const char* s) {
        std::vector<std::string> out;
        std::string cur;
        for (const char* p = s; p && *p; p++) {
            if (*p == '\n') {
                out.push_back(cur);
                cur.clear();
            } else {
                cur += *p;
            }
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    };
    B(v)->host.inPorts = split(ins);
    B(v)->host.outPorts = split(outs);
    B(v)->host.midiOpenOk = ok != 0;
}
// direct calls of the page controller (like calling the original ClcdCtr methods):
// op 0 = selectPageSub(a, b) (FUN_0045a9a0), 1 = setParam(a, b) (FUN_0045b574)
SQ8L_API void sq8l_gl_ctr(void* v, int op, int a, int b) {
    LcdController& ctr = B(v)->ctl->lcd();
    if (op == 0)
        ctr.selectPageSub(a, b);
    else if (op == 1)
        ctr.setParam(a, b);
}
// the host changes the program while the editor is open (effSetProgram -> master FUN_004631f0)
SQ8L_API void sq8l_gl_host_program(void* v, int index) { B(v)->host.eb.selectIndex(index); }
// a SysEx message received on the open MIDI input (CmidiIO callback -> FUN_0048427c)
SQ8L_API void sq8l_gl_sysex_received(void* v, const uint8_t* data, int32_t n) {
    B(v)->ctl->sysexReceived(data, size_t(n));
}
SQ8L_API void sq8l_gl_clear_answers(void* v) {
    B(v)->ui.menuChoices.clear();
    B(v)->ui.msgAnswers.clear();
    B(v)->ui.fileAnswers.clear();
    B(v)->ui.modalScripts.clear();
}
SQ8L_API void sq8l_gl_set_file(void* v, const char* path, const uint8_t* data, int32_t n) {
    B(v)->ui.files[path] = std::vector<uint8_t>(data, data + n);
}
SQ8L_API int32_t sq8l_gl_get_file(void* v, const char* path, uint8_t* out, int32_t max) {
    auto f = B(v)->ui.files.find(path);
    if (f == B(v)->ui.files.end()) return -1;
    if (out) std::memcpy(out, f->second.data(), std::min<size_t>(f->second.size(), size_t(max)));
    return int32_t(f->second.size());
}
// one modal dialog: actions separated by ';' ("item 5;compare;bank;ok", "cancel")
SQ8L_API void sq8l_gl_modal_script(void* v, const char* script) {
    std::vector<std::string> acts;
    std::string s = script, cur;
    for (char c : s) {
        if (c == ';') {
            if (!cur.empty()) acts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) acts.push_back(cur);
    B(v)->ui.modalScripts.push_back(acts);
}

SQ8L_API int32_t sq8l_gl_events(void* v, char* out, int32_t max) {
    std::string s = B(v)->ui.events.str();
    B(v)->ui.events.str("");
    s += B(v)->host.log.str();
    B(v)->host.log.str("");
    return copyOut(s, out, max);
}

// ------------------------------------------------------------------ state

SQ8L_API void sq8l_gl_editbuffer(void* v, uint8_t* img) { B(v)->host.eb.saveState(img); }
SQ8L_API void sq8l_gl_library(void* v, uint8_t* progs) {
    std::memcpy(progs, B(v)->host.lib.raw(), size_t(SoundLibrary::kNumPrograms) * kProgramSize);
}
SQ8L_API void sq8l_gl_settings(void* v, int32_t* out) {
    for (int i = 0; i < Settings::kNumGui; i++) out[i] = B(v)->host.cfg.gui[i];
    for (int i = 0; i < Settings::kNumSynth; i++) out[6 + i] = B(v)->host.cfg.synth[i];
}

SQ8L_API int32_t sq8l_gl_state(void* v, char* out, int32_t max) {
    Box* b = B(v);
    EditorView& ev = b->view;
    EditorController& c = *b->ctl;
    std::ostringstream s;
    s << "{\"lcd\":" << lcdJson(ev.lcd()) << ",\"numLcd\":" << lcdJson(ev.numLcd());
    s << ",\"knobs\":[";
    for (int i = 0; i < 10; i++) {
        const Knob::State k = ev.knob(i).state();
        s << (i ? "," : "") << "{\"value\":" << fbits(k.value) << ",\"min\":" << fbits(k.min) << ",\"max\":" << fbits(k.max)
          << ",\"range\":" << fbits(k.range) << ",\"step\":" << fbits(k.step) << ",\"pixFactor\":" << fbits(k.pixFactor)
          << ",\"snapZone\":" << fbits(k.snapZone) << ",\"acc\":" << fbits(k.acc) << ",\"angle\":" << fbits(k.angle)
          << ",\"angleK\":" << fbits(k.angleK) << ",\"maxPixDist\":" << k.maxPixDist
          << ",\"maxFinePixDist\":" << k.maxFinePixDist << ",\"downX\":" << k.downX << ",\"downY\":" << k.downY
          << ",\"lastX\":" << k.lastX << ",\"lastY\":" << k.lastY << ",\"dragging\":" << int(k.dragging)
          << ",\"dragActive\":" << int(k.dragActive) << ",\"restoreMouse\":" << int(k.restoreMouse)
          << ",\"intMode\":" << int(k.intMode) << "}";
    }
    s << "],\"buttons\":{";
    bool first = true;
    for (GraphButton* g : ev.buttons()) {
        s << (first ? "" : ",") << jstr(g->name()) << ":[" << g->aniIdx() << "," << g->pressed() << "," << int(g->hover())
          << "]";
        first = false;
    }
    s << "},\"leds\":[" << fbits(ev.ledSync().value()) << "," << fbits(ev.ledAm().value()) << ","
      << fbits(ev.ledMono().value()) << "]";
    s << ",\"status\":" << jstr(ev.statusLabel1().caption()) << ",\"voices\":" << jstr(ev.statusLabel2().caption())
      << ",\"name\":" << jstr(ev.progNameEdit().text);
    LcdController& ctr = c.lcd();
    s << ",\"ctr\":{\"page\":" << ctr.pageIndex() << ",\"sub\":" << ctr.subPage() << ",\"displayLock\":"
      << ctr.displayLock() << ",\"updateLock\":" << ctr.updateLock << ",\"msgActive\":" << ctr.msgActive
      << ",\"msgTimeout\":" << ctr.msgTimeout << ",\"refreshCount\":" << ctr.refreshCount << ",\"mouseDown\":"
      << int(ctr.mouseIsDown) << ",\"downCol\":" << ctr.downCol << ",\"downRow\":" << ctr.downRow
      << ",\"restoreMouse\":" << int(ctr.restoreMouse) << ",\"params\":[";
    if (LcdSubPage* sp = ctr.currentSub()) {
        for (size_t i = 0; i < sp->params.size(); i++) {
            const LcdParam& p = sp->params[i];
            s << (i ? "," : "") << "[" << int(p.highlight) << "," << p.value << "," << jstr(p.valueText) << "]";
        }
    }
    s << "]}";
    const MouseJump& mj = c.mouseJump();
    s << ",\"mouseJump\":[" << int(mj.saved()) << "," << mj.position().x + b->ui.formX << ","
      << mj.position().y + b->ui.formY << "]";
    s << ",\"form\":{\"restMouseMenu\":" << int(c.restMouseMenu()) << ",\"restMouseKnob\":" << int(c.restMouseKnob())
      << ",\"rmbScroll\":" << int(c.rmbScroll()) << ",\"swap\":" << int(c.swapProgUpDn())
      << ",\"keyCaptMode\":" << c.keyCaptMode() << ",\"lcdDragKnob\":" << c.lcdDragKnob()
      << ",\"tickDivider\":" << c.tickDivider() << ",\"midiIn\":" << c.midiInPort() << ",\"midiOut\":"
      << c.midiOutPort() << ",\"sysexReq\":" << c.sysexRequestPending() << "}";
    s << ",\"pending\":" << int(c.hasPending()) << "}";
    return copyOut(s.str(), out, max);
}

// The page tree as built (for the comparison with the dump of the original's objects).
SQ8L_API int32_t sq8l_gl_pages(void* v, char* out, int32_t max) {
    LcdController& ctr = B(v)->ctl->lcd();
    std::ostringstream s;
    s << "[";
    for (int i = 0; i < ctr.pageCount(); i++) {
        const LcdPage& pg = ctr.pages()[size_t(i)];
        s << (i ? "," : "") << "{\"cur\":" << pg.cur << ",\"subs\":[";
        for (size_t j = 0; j < pg.subs.size(); j++) {
            const LcdSubPage& sp = pg.subs[j];
            s << (j ? "," : "") << "{\"title\":" << jstr(sp.title) << ",\"c\":" << sp.c << ",\"group\":" << sp.group
              << ",\"base\":" << sp.base << ",\"offset\":" << sp.offset << ",\"indent\":" << sp.flags[0]
              << ",\"hidden\":" << sp.flags[1] << ",\"knobs\":[";
            for (size_t k = 0; k < sp.knobMap.size(); k++) s << (k ? "," : "") << sp.knobMap[k];
            s << "],\"params\":[";
            for (size_t k = 0; k < sp.params.size(); k++) {
                const LcdParam& p = sp.params[k];
                s << (k ? "," : "") << "{\"knob\":" << p.knob << ",\"index\":" << p.index << ",\"ordinal\":" << p.ordinal
                  << ",\"name\":" << jstr(p.name) << ",\"hint\":" << jstr(p.hint) << ",\"label\":" << jstr(p.label)
                  << ",\"width\":" << p.width << ",\"x\":" << p.x << ",\"y\":" << p.y << ",\"min\":" << p.min
                  << ",\"max\":" << p.max << ",\"fmt\":" << int(p.fmt) << ",\"pop\":" << int(p.pop)
                  << ",\"customFmt\":" << int(p.customFmt) << ",\"showNumber\":" << int(p.showNumber)
                  << ",\"perColumn\":" << p.perColumn << ",\"paramIndex\":" << p.paramIndex << ",\"change\":"
                  << int(p.change) << "}";
            }
            s << "]}";
        }
        s << "]}";
    }
    s << "]";
    return copyOut(s.str(), out, max);
}

// formatters (exhaustive comparison with the original routines)
SQ8L_API int32_t sq8l_gl_format(int32_t fmt, int32_t width, int32_t value, const char* prev, char* out, int32_t max) {
    std::string s = prev ? prev : "";
    formatValue(Fmt(fmt), width, value, s);
    return copyOut(s, out, max);
}
SQ8L_API int32_t sq8l_gl_popup_text(int32_t pop, int32_t value, const char* prev, char* out, int32_t max) {
    std::string s = prev ? prev : "";
    formatPopupText(PopText(pop), value, s);
    return copyOut(s, out, max);
}
SQ8L_API int32_t sq8l_gl_mod_usage(const uint8_t* prog, char* out, int32_t max) {
    Program p;
    std::memcpy(p.bytes, prog, kProgramSize);
    std::string s;
    for (const std::string& l : modulationUsage(p)) s += l + "\n";
    return copyOut(s, out, max);
}
