// C API: the editor logic (src/gui/logic) on a C++ EditorView, for tests/test_gui_extensions.py.
//
// A Box holds the engine side (SoundLibrary, EditBuffer, Settings), the EditorView, a
// scripted PlatformUi and the EditorController. Input is injected as Win32 mouse messages in
// form coordinates (like the oracle driver); the state is exported as JSON.
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "Dialogs.h"
#include "EditBuffer.h"
#include "EditorController.h"
#include "EditorView.h"
#include "Formatters.h"
#include "LcdControl.h"
#include "SQ8LVersion.h"
#include "Settings.h"
#include "SoundLibrary.h"
#include "SoundParameterState.h"
#include "VfdText.h"

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
    Settings cfg;
    Synth synth{44100.0f, &lib, &cfg};
    EditBuffer& eb = synth.editBuffer();
    sound::SoundParameterService service{eb.current()};
    sound::SoundParameterLifecycle lifecycle{synth, service};
    std::ostringstream soundTrace;
    bool echo = true;
    std::recursive_mutex mutex;
    sound::PanelNotificationQueue notifications;
    struct Engine {
        TestHost& host;
        explicit Engine(TestHost& h) : host(h) { host.mutex.lock(); host.notifications.engineEntered(); }
        ~Engine() {
            host.mutex.unlock(); host.notifications.engineLeft(); host.drain();
        }
    };
    void drain() {
        notifications.drain([this](const sound::PanelNotification& event) {
            if (notifications.engineDepth() != 0) soundTrace << "BAD_LOCK\n";
            if (event.kind == sound::PanelNotification::Value) {
                Engine lock(*this);
                if (!service.currentPanelEcho(event.token)) return;
            }
            const char kind = event.kind == sound::PanelNotification::Begin ? 'B' :
                              event.kind == sound::PanelNotification::End ? 'E' : 'V';
            soundTrace << kind << " " << event.index;
            if (kind == 'V') {
                soundTrace << " " << event.token.value << " " << event.token.stamp.revision;
                if (echo) {
                    Engine lock(*this);
                    auto echoed = service.acceptScopedPanelEcho(event.index, event.token.value, event.token);
                    if (echoed.status != sound::Acceptance::Echo) soundTrace << " BAD_ECHO";
                }
            }
            soundTrace << "\n";
        });
    }
    int soundValue(unsigned id) override { int v = 0; service.get(id, v); return v; }
    int soundButtonValue(unsigned index) override {
        const auto* p = sound::parameter(index);
        if (p && p->field == 325 && service.stamp(index).origin == sound::Origin::Replacement &&
            eb.current().s8(ofs::AmMod) == 2) return 2;
        return soundValue(index);
    }
    uint64_t soundGeneration() override { return service.generation(); }
    void soundBegin(unsigned id) override { notifications.push({sound::PanelNotification::Begin, id, {}}); }
    void soundEnd(unsigned id) override { notifications.push({sound::PanelNotification::End, id, {}}); }
    void soundEdit(unsigned id, int v) override {
        auto result = service.accept(id, v, sound::Origin::Panel);
        if (result.status != sound::Acceptance::Accepted) return;
        eb.setModified(true);
        notifications.push({sound::PanelNotification::Value, id, result.token});
    }
    EditorController* ctl = nullptr;
    int maxVoices = 8, usedVoices = 0;
    std::string dir = "C:\\VST\\SQ8L\\";
    std::ostringstream log;

    TestHost() {
        const auto engineListener = eb.listener;
        eb.listener = [this, engineListener](EditBuffer::Event e, const Program* p) {
            if (engineListener) engineListener(e, p);
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
    void setPortSetting(int i, int v) override {
        if (cfg.setPort(i, v) && ctl) ctl->post(kMsgNotify, 0, 0x20 + i);
    }
    // (port) OPTIONS -> Polyphony: per instance, so the plugin keeps it in its own state
    // (here just a field), not in SQ8L.ini.
    int polyOverride = 0;
    int polyphonyOverride() override { return polyOverride; }
    void setPolyphonyOverride(int voices) override { polyOverride = voices; }
    // (port, issue #52) OPTIONS -> Output gain: the cut of this instance (a field here, the
    // plugin's own state there), and the same value in SQ8L.ini, which is where a newly
    // created instance starts. Both as PluginEditorHost does it.
    int outputCutDb = 0;
    int outputCut() override { return outputCutDb; }
    void setOutputCut(int db) override {
        outputCutDb = Settings::validOutputCut(db) ? db : 0;
        cfg.setPort(Settings::kPortOutputCut, outputCutDb);
    }
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
    std::function<void()> beginPopup, endPopup, popupAction;
    int formX = 0, formY = 0;                 // screen position of the form client area

    int popupMenu(const std::vector<MenuItem>& items, int x, int y) override {
        if (beginPopup) beginPopup();
        struct Finish {
            std::function<void()>& action;
            ~Finish() { if (action) action(); }
        } finish{endPopup};
        auto action = std::move(popupAction);
        popupAction = {};
        if (action) action();
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
    // (port) OPTIONS -> Mouse -> "Hide cursor when editing": > 0 while a knob is turned.
    int cursorHidden = 0;
    void setCursorVisible(bool visible) override {
        cursorHidden += visible ? -1 : 1;
        events << "{\"ev\":\"cursor\",\"visible\":" << (visible ? "true" : "false")
               << "}\n";
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

    ~Box() { TestHost::Engine engine(host); ctl.reset(); }

    explicit Box(int first) {
        ctl = std::make_unique<EditorController>(view, host, ui, first != 0);
        host.ctl = ctl.get();
        ui.pumpDuringModal = [this] { ctl->pump(); };
        // Match SQ8LUI's modal hook, including its outer engine release/reacquire.
        ui.beginPopup = [this] {
            ctl->prepareModalInput();
            host.mutex.unlock();
            host.notifications.engineLeft();
            host.drain();
        };
        ui.endPopup = [this] {
            host.mutex.lock();
            host.notifications.engineEntered();
        };
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

SQ8L_API int32_t sq8l_gl_sound_queue_check(void* v) {
    auto& h = B(v)->host;
    int callbacks = 0, bad = 0;
    h.mutex.lock(); h.notifications.engineEntered();
    h.notifications.push({sound::PanelNotification::Begin, 0, {}});
    h.mutex.lock(); h.notifications.engineEntered();
    h.notifications.push({sound::PanelNotification::End, 0, {}});
    auto callback = [&](const sound::PanelNotification&) {
        ++callbacks;
        if (h.notifications.engineDepth()) ++bad;
        if (callbacks == 1) {
            h.notifications.push({sound::PanelNotification::Begin, 1, {}});
            h.notifications.drain([&](const sound::PanelNotification&) { ++bad; });
        }
    };
    h.mutex.unlock(); h.notifications.engineLeft(); h.notifications.drain(callback);
    if (callbacks) ++bad; // recursive unlock cannot dispatch
    h.mutex.unlock(); h.notifications.engineLeft(); h.notifications.drain(callback);
    if (callbacks != 3) ++bad;
    // Modal release/reacquire uses the same outer scope; empty final drain is inert.
    h.mutex.lock(); h.notifications.engineEntered();
    h.notifications.push({sound::PanelNotification::End, 1, {}});
    h.mutex.unlock(); h.notifications.engineLeft(); h.notifications.drain(callback);
    h.mutex.lock(); h.notifications.engineEntered();
    h.mutex.unlock(); h.notifications.engineLeft(); h.notifications.drain(callback);
    if (callbacks != 4) ++bad;
    sound::PanelNotificationQueue queue;
    std::array<int, sound::kParameterCount> balance{}, latest{}, expected{};
    for (unsigned n = 0; n < 5000; ++n) {
        const unsigned id = n % sound::kParameterCount;
        expected[id] = int(n);
        queue.push({sound::PanelNotification::Begin, id, {}});
        sound::EchoToken token; token.value = int(n);
        queue.push({sound::PanelNotification::Value, id, token});
        queue.push({sound::PanelNotification::End, id, {}});
    }
    queue.drain([&](const sound::PanelNotification& event) {
        auto& b = balance[event.index];
        if (event.kind == sound::PanelNotification::Begin && ++b != 1) ++bad;
        if (event.kind == sound::PanelNotification::Value) { if (b != 1) ++bad; latest[event.index] = event.token.value; }
        if (event.kind == sound::PanelNotification::End && --b != 0) ++bad;
    });
    for (unsigned id = 0; id < sound::kParameterCount; ++id)
        if (balance[id] || latest[id] != expected[id]) ++bad;
    // Reproduce the unlocked bridge gap with the exact production setter helper.
    {
        TestHost::Engine engine(h);
        auto accepted = h.service.accept(0, 2, sound::Origin::Panel);
        if (!h.service.currentPanelEcho(accepted.token)) ++bad;
        h.service.accept(0, 3, sound::Origin::Host);
        if (h.service.acceptScopedPanelEcho(0, 2, accepted.token).status != sound::Acceptance::Rejected || h.soundValue(0) != 3) ++bad;
        accepted = h.service.accept(0, 2, sound::Origin::Panel);
        h.eb.initProgram();
        if (h.service.acceptScopedPanelEcho(0, 2, accepted.token).status != sound::Acceptance::Rejected) ++bad;
        const auto revision = h.service.snapshot().revision;
        h.service.accept(0, h.soundValue(0), sound::Origin::Host);
        if (h.service.snapshot().revision != revision + 1) ++bad;
    }
    return bad;
}
SQ8L_API int32_t sq8l_gl_sound_trace(void* v, char* out, int32_t max) { TestHost::Engine engine(B(v)->host);
    auto& trace = B(v)->host.soundTrace;
    const auto result = trace.str(); trace.str(""); return copyOut(result, out, max);
}
SQ8L_API void sq8l_gl_sound_host(void* v, unsigned id, int value) { TestHost::Engine engine(B(v)->host);
    B(v)->host.service.accept(id, value, sound::Origin::Host);
}
SQ8L_API int32_t sq8l_gl_sound_get(void* v, unsigned id) { TestHost::Engine engine(B(v)->host); return B(v)->host.soundValue(id); }
SQ8L_API void sq8l_gl_sound_action(void* v, int op) { TestHost::Engine engine(B(v)->host);
    auto& h = B(v)->host;
    if (op >= 0 && op <= 2) {
        auto* b = B(v);
        const auto& button = b->view.button(op == 0 ? "buttSync" : op == 1 ? "buttAm" : "buttMono");
        const int x = button.left() + button.width() / 2, y = button.top() + button.height() / 2;
        b->ctl->prepareSoundInput();
        b->view.mouseMove(x, y, 0);
        b->view.mouseDown(MouseButton::Left, x, y, 1);
        b->view.mouseUp(MouseButton::Left, x, y, 0);
    }
    if (op == 9) { h.eb.current().setS8(ofs::AmMod, 2); h.service.replace(h.eb.current()); }
    if (op == 3) h.eb.initProgram();
    if (op == 4) h.eb.selectIndex(4);
    if (op == 5) h.eb.compare(5, 0);
    if (op == 6) h.eb.compareOff();
    if (op == 7) { auto chunk = h.eb.getChunk(); h.eb.setChunk(chunk.data(), chunk.size()); }
    if (op == 8) { auto sysex = h.eb.exportSysex(); h.eb.importSysex(sysex.data(), sysex.size(), true); }
}
SQ8L_API int32_t sq8l_gl_sound_raw(void* v, unsigned id) { TestHost::Engine engine(B(v)->host);
    const auto* p = sound::parameter(id); return p ? sound::read(*p, B(v)->host.eb.current()) : 0;
}
SQ8L_API void sq8l_gl_sound_sync(void* v, int legacy) { TestHost::Engine engine(B(v)->host);
    B(v)->host.eb.synchronize();
    if (legacy) B(v)->host.service.replace(B(v)->host.eb.current());
}
SQ8L_API void sq8l_gl_sound_cancel(void* v) { TestHost::Engine engine(B(v)->host); B(v)->ctl->cancelSoundEdit(); }
// Focus loss invokes cancelSoundEdit under Engine in SQ8LUI::uiFocus. Destruction
// keeps this Box alive so the resulting real host trace remains inspectable.
SQ8L_API void sq8l_gl_sound_boundary(void* v, int op) {
    auto* b = B(v);
    TestHost::Engine engine(b->host);
    if (op == 0) b->ctl->cancelSoundEdit();
    if (op == 1) b->view.cancelMouseMode();
    if (op == 2) { b->ctl.reset(); b->host.ctl = nullptr; }
}
SQ8L_API uint64_t sq8l_gl_sound_revision(void* v) {
    TestHost::Engine engine(B(v)->host);
    return B(v)->host.service.snapshot().revision;
}
SQ8L_API int32_t sq8l_gl_sound_project(void* v) {
    auto& h = B(v)->host;
    sound::SoundParameterProjectSnapshot snapshot;
    { TestHost::Engine engine(h); snapshot = h.lifecycle.capture(); }
    std::vector<uint8_t> bytes;
    sound::ValidatedSoundParameterState state;
    if (!sound::encodeSoundParameterState(snapshot.base.data(), snapshot.base.size(), snapshot.pairs, bytes) ||
        !sound::parseSoundParameterState(bytes.data(), bytes.size(), state)) return 0;
    TestHost::Engine engine(h);
    return h.lifecycle.restore(state) ? int32_t(bytes.size()) : 0;
}
SQ8L_API void sq8l_gl_sound_popup_action(void* v, int op, unsigned id) {
    auto* b = B(v);
    TestHost::Engine engine(b->host);
    b->ui.popupAction = [b, op, id] {
        TestHost::Engine reentrant(b->host);
        if (op == 0) b->host.eb.initProgram();
        if (op == 1) b->ctl->lcd().selectPageSub(0, 0);
        if (op == 2) b->host.service.accept(id, 0, sound::Origin::Host);
        if (op == 3) b->ctl->cancelSoundEdit();
        if (op == 4) {
            const auto& knob = b->view.knob(int(id));
            b->ctl->wheel(knob.left() + knob.width() / 2, knob.top() + knob.height() / 2, -1, 0);
        }
    };
}
SQ8L_API void sq8l_gl_sound_reopen(void* v) { TestHost::Engine engine(B(v)->host);
    auto* b = B(v);
    b->ctl.reset();
    b->ctl = std::make_unique<EditorController>(b->view, b->host, b->ui, false);
    b->host.ctl = b->ctl.get();
    b->ctl->show();
}
SQ8L_API void* sq8l_gl_new(int first) { return new Box(first); }
// (port) OPTIONS -> Polyphony, the playable voices of this instance (0 = set by the program)
SQ8L_API int32_t sq8l_gl_poly_override(void* v) { TestHost::Engine engine(B(v)->host); return B(v)->host.polyOverride; }
// (port, issue #52) OPTIONS -> Output gain, the cut of this instance in dB (0 = none)
SQ8L_API int32_t sq8l_gl_output_cut(void* v) { TestHost::Engine engine(B(v)->host); return B(v)->host.outputCutDb; }
// (port) Knob::active: false for the knobs the current page has no parameter for, which are
// drawn faded.
SQ8L_API int32_t sq8l_gl_knob_active(void* v, int32_t i) { TestHost::Engine engine(B(v)->host);
    return i >= 0 && i < 10 ? int32_t(B(v)->view.knob(i).active()) : -1;
}
// (port, issue #43) Whether knob slot i shows its soft button instead of the knob, i.e. the
// page's parameter there has only two values. Kept out of the state dump for the same reason.
SQ8L_API int32_t sq8l_gl_toggle_shown(void* v, int32_t i) { TestHost::Engine engine(B(v)->host);
    return i >= 0 && i < 10 ? int32_t(B(v)->view.toggle(i).visible()) : -1;
}
// (port, issue #35) The mouse wheel at a form point; `delta` as the platform would report it.
SQ8L_API void sq8l_gl_wheel(void* v, int32_t x, int32_t y, double delta, int32_t keys) { TestHost::Engine engine(B(v)->host);
    B(v)->ctl->wheel(x, y, delta, uint32_t(keys));
}
SQ8L_API int32_t sq8l_gl_port_setting(void* v, int32_t i) { TestHost::Engine engine(B(v)->host);
    return i >= 0 && i < Settings::kNumPort ? B(v)->host.cfg.port[i] : -1;
}
SQ8L_API void sq8l_gl_free(void* v) { delete B(v); }

// engine state (before show): library image (512 x 540 bytes), header (0x3c), clean flag;
// edit buffer state image (EditBuffer::kStateSize); settings gui[6] + synth[5].
SQ8L_API void sq8l_gl_set_library(void* v, const uint8_t* progs, const uint8_t* header, int clean) { TestHost::Engine engine(B(v)->host);
    std::memcpy(B(v)->host.lib.raw(), progs, size_t(SoundLibrary::kNumPrograms) * kProgramSize);
    (void)header;
    B(v)->host.lib.setClean(clean != 0);
}
SQ8L_API void sq8l_gl_set_editbuffer(void* v, const uint8_t* img) { TestHost::Engine engine(B(v)->host); B(v)->host.eb.loadState(img); B(v)->host.service.replace(B(v)->host.eb.current()); }
SQ8L_API void sq8l_gl_set_settings(void* v, const int32_t* gui, const int32_t* synth) { TestHost::Engine engine(B(v)->host);
    for (int i = 0; i < Settings::kNumGui; i++) B(v)->host.cfg.gui[i] = gui[i];
    for (int i = 0; i < Settings::kNumSynth; i++) B(v)->host.cfg.synth[i] = synth[i];
}
SQ8L_API void sq8l_gl_set_host(void* v, int maxVoices, int usedVoices, const char* dir) { TestHost::Engine engine(B(v)->host);
    B(v)->host.maxVoices = maxVoices;
    B(v)->host.usedVoices = usedVoices;
    if (dir) B(v)->host.dir = dir;
}
SQ8L_API void sq8l_gl_set_form_origin(void* v, int x, int y) { TestHost::Engine engine(B(v)->host);
    B(v)->ui.formX = x;
    B(v)->ui.formY = y;
}

// FormShow, then the first paint (like the original's window: the LEDs fit their size to the
// GIF frame when painted, which matters for the mouse hit test)
SQ8L_API void sq8l_gl_show(void* v) { TestHost::Engine engine(B(v)->host);
    B(v)->ctl->show();
    Bitmap frame;
    B(v)->view.render(frame);
}

// ------------------------------------------------------------------ input / time

SQ8L_API void sq8l_gl_mouse(void* v, int32_t msg, int32_t x, int32_t y, int32_t keys) { TestHost::Engine engine(B(v)->host);
    Box* b = B(v);
    b->ctl->prepareSoundInput();
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
SQ8L_API void sq8l_gl_context_menu(void* v, int32_t x, int32_t y) { TestHost::Engine engine(B(v)->host);
    B(v)->ui.cursor = Point{x, y};
    B(v)->ctl->contextMenu(x, y);
}
// What the plugin UI does after a press that opened a menu or a dialog (SQ8LUI::onMouse):
// the modal loop keeps the button up, so the control the press was made on must not keep the
// mouse. Scripted input drives the logic without that shell, so a test says it itself.
SQ8L_API void sq8l_gl_cancel_mouse_mode(void* v) { TestHost::Engine engine(B(v)->host); B(v)->ctl->cancelSoundEdit(); B(v)->view.cancelMouseMode(); }
SQ8L_API void sq8l_gl_idle(void* v, int32_t ms) { TestHost::Engine engine(B(v)->host); B(v)->ctl->idle(ms); }
SQ8L_API void sq8l_gl_timer(void* v) { TestHost::Engine engine(B(v)->host); B(v)->ctl->timerTick(); }
SQ8L_API int32_t sq8l_gl_pump(void* v) { TestHost::Engine engine(B(v)->host); return B(v)->ctl->pump(); }
SQ8L_API void sq8l_gl_post(void* v, uint32_t msg, uint32_t wp, int32_t lp) { TestHost::Engine engine(B(v)->host); B(v)->ctl->post(msg, wp, lp); }
SQ8L_API void sq8l_gl_name_focus(void* v, int focused, const char* text) { TestHost::Engine engine(B(v)->host);
    B(v)->view.progNameEdit().text = text;
    B(v)->ctl->nameEditFocus(focused != 0, text);
}
SQ8L_API void sq8l_gl_name_key(void* v, int key) { TestHost::Engine engine(B(v)->host); B(v)->ctl->nameEditKeyDown(key); }
SQ8L_API void sq8l_gl_set_name_text(void* v, const char* text) { TestHost::Engine engine(B(v)->host); B(v)->view.progNameEdit().text = text; }

// scripted platform answers
SQ8L_API void sq8l_gl_menu_choice(void* v, const int32_t* path, int32_t n) { TestHost::Engine engine(B(v)->host);
    B(v)->ui.menuChoices.emplace_back(path, path + n);
}
SQ8L_API void sq8l_gl_msg_answer(void* v, int32_t r) { TestHost::Engine engine(B(v)->host); B(v)->ui.msgAnswers.push_back(r); }
SQ8L_API void sq8l_gl_file_answer(void* v, const char* path) { TestHost::Engine engine(B(v)->host); B(v)->ui.fileAnswers.emplace_back(path); }
// drop the scripted answers not used (popup choices, message boxes, file and modal dialogs)
// MIDI ports: names separated by '\n'; ok = opening succeeds
SQ8L_API void sq8l_gl_set_midi(void* v, const char* ins, const char* outs, int ok) { TestHost::Engine engine(B(v)->host);
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
SQ8L_API void sq8l_gl_ctr(void* v, int op, int a, int b) { TestHost::Engine engine(B(v)->host);
    LcdController& ctr = B(v)->ctl->lcd();
    if (op == 0)
        ctr.selectPageSub(a, b);
    else if (op == 1)
        ctr.setParam(a, b);
}
// (port) The main VFD's dotted text (src/gui/logic/VfdText.h), as the boot message writes the
// version: a '.' is the decimal point of the cell before it, not a column. Returns the width.
SQ8L_API int32_t sq8l_gl_write_dotted(void* v, int32_t col, int32_t row, const char* text, int32_t attr) { TestHost::Engine engine(B(v)->host);
    return writeDotted(B(v)->view.lcd(), col, row, text ? text : "", attr);
}
// (port) The version of this build (SQ8LVersion.h, written by cmake/Version.cmake).
SQ8L_API const char* sq8l_gl_version() { return kVersion; }
// the host changes the program while the editor is open (effSetProgram -> master FUN_004631f0)
SQ8L_API void sq8l_gl_host_program(void* v, int index) { TestHost::Engine engine(B(v)->host); B(v)->host.eb.selectIndex(index); }
// a SysEx message received on the open MIDI input (CmidiIO callback -> FUN_0048427c)
SQ8L_API void sq8l_gl_sysex_received(void* v, const uint8_t* data, int32_t n) { TestHost::Engine engine(B(v)->host);
    B(v)->ctl->sysexReceived(data, size_t(n));
}
SQ8L_API void sq8l_gl_clear_answers(void* v) { TestHost::Engine engine(B(v)->host);
    B(v)->ui.menuChoices.clear();
    B(v)->ui.msgAnswers.clear();
    B(v)->ui.fileAnswers.clear();
    B(v)->ui.modalScripts.clear();
}
SQ8L_API void sq8l_gl_set_file(void* v, const char* path, const uint8_t* data, int32_t n) { TestHost::Engine engine(B(v)->host);
    B(v)->ui.files[path] = std::vector<uint8_t>(data, data + n);
}
SQ8L_API int32_t sq8l_gl_get_file(void* v, const char* path, uint8_t* out, int32_t max) { TestHost::Engine engine(B(v)->host);
    auto f = B(v)->ui.files.find(path);
    if (f == B(v)->ui.files.end()) return -1;
    if (out) std::memcpy(out, f->second.data(), std::min<size_t>(f->second.size(), size_t(max)));
    return int32_t(f->second.size());
}
// one modal dialog: actions separated by ';' ("item 5;compare;bank;ok", "cancel")
SQ8L_API void sq8l_gl_modal_script(void* v, const char* script) { TestHost::Engine engine(B(v)->host);
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

SQ8L_API int32_t sq8l_gl_events(void* v, char* out, int32_t max) { TestHost::Engine engine(B(v)->host);
    std::string s = B(v)->ui.events.str();
    B(v)->ui.events.str("");
    s += B(v)->host.log.str();
    B(v)->host.log.str("");
    return copyOut(s, out, max);
}

// ------------------------------------------------------------------ state

SQ8L_API void sq8l_gl_editbuffer(void* v, uint8_t* img) { TestHost::Engine engine(B(v)->host); B(v)->host.eb.synchronize(); B(v)->host.eb.saveState(img); }
SQ8L_API void sq8l_gl_library(void* v, uint8_t* progs) { TestHost::Engine engine(B(v)->host);
    std::memcpy(progs, B(v)->host.lib.raw(), size_t(SoundLibrary::kNumPrograms) * kProgramSize);
}
SQ8L_API void sq8l_gl_settings(void* v, int32_t* out) { TestHost::Engine engine(B(v)->host);
    for (int i = 0; i < Settings::kNumGui; i++) out[i] = B(v)->host.cfg.gui[i];
    for (int i = 0; i < Settings::kNumSynth; i++) out[6 + i] = B(v)->host.cfg.synth[i];
}

SQ8L_API int32_t sq8l_gl_state(void* v, char* out, int32_t max) { TestHost::Engine engine(B(v)->host);
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
          << ",\"dragActive\":" << int(k.dragActive) << ",\"intMode\":" << int(k.intMode)
          << "}";
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
    s << ",\"form\":{\"restMouseMenu\":" << int(c.restMouseMenu())
      << ",\"hideCursor\":" << int(c.hideCursorEnabled())
      << ",\"rmbScroll\":" << int(c.rmbScroll()) << ",\"swap\":" << int(c.swapProgUpDn())
      << ",\"keyCaptMode\":" << c.keyCaptMode() << ",\"lcdDragKnob\":" << c.lcdDragKnob()
      << ",\"tickDivider\":" << c.tickDivider() << ",\"midiIn\":" << c.midiInPort() << ",\"midiOut\":"
      << c.midiOutPort() << ",\"sysexReq\":" << c.sysexRequestPending() << "}";
    s << ",\"pending\":" << int(c.hasPending()) << "}";
    return copyOut(s.str(), out, max);
}

// The page tree as built.
SQ8L_API int32_t sq8l_gl_pages(void* v, char* out, int32_t max) { TestHost::Engine engine(B(v)->host);
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

// The parameter value formatters.
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
