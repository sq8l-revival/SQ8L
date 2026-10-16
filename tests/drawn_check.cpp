// The drawn menus and dialogs (src/gui/drawn, the Linux editor) without a window: scripted
// mouse/keyboard input inside their nested loops, checked answers, and snapshots of the
// editor with the overlay (PPM) for a visual check.
//
//   sq8l_drawn_check [snapshot dir]
#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "EditorView.h"
#include "drawn/PlatformUiDrawn.h"
#include "logic/Dialogs.h"
#include "text/StbTextRenderer.h"

using namespace sq8l::gui;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) failures++;
}

struct Harness {
    StbTextRenderer text{true};
    EditorView view;
    Bitmap editor{EditorView::kWidth, EditorView::kHeight};
    std::deque<std::function<void()>> script;
    std::string snapDir;
    std::vector<std::string> log;
    std::unique_ptr<PlatformUiDrawn> ui;

    Harness(const std::string& dir) : snapDir(dir) {
        view.setTextRenderer(&text);
        view.render(editor);
        PlatformUiDrawn::Hooks h;
        h.releaseEngine = [this] { log.push_back("release"); };
        h.acquireEngine = [this] { log.push_back("acquire"); };
        h.pump = [this] { log.push_back("pump"); };
        h.nameFocus = [this](bool f, const std::string& t) { log.push_back(std::string(f ? "focus " : "blur ") + t); };
        h.nameKey = [this](int k) { log.push_back("key " + std::to_string(k)); };
        h.runLoopStep = [this] {  // one scripted user action per loop step
            if (script.empty()) return false;
            auto f = script.front();
            script.pop_front();
            f();
            return true;
        };
        ui = std::make_unique<PlatformUiDrawn>(text, h);
    }

    void snap(const std::string& name) {
        if (snapDir.empty()) return;
        Bitmap b = editor;
        ui->render(b);
        std::vector<uint8_t> rgb(static_cast<size_t>(b.width()) * b.height() * 3);
        b.toRGB24(rgb.data());
        const std::string path = snapDir + "/" + name + ".ppm";
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fprintf(f, "P6\n%d %d\n255\n", b.width(), b.height());
            std::fwrite(rgb.data(), 1, rgb.size(), f);
            std::fclose(f);
        }
    }
    // script helpers
    void move(int x, int y) { script.push_back([=] { ui->mouseMove(x, y); }); }
    void click(int x, int y, bool dbl = false) {
        script.push_back([=] { ui->mouseMove(x, y); });
        script.push_back([=] { ui->mouseDown(MouseButton::Left, x, y, dbl); });
        script.push_back([=] { ui->mouseUp(MouseButton::Left, x, y); });
    }
    void key(int vk) { script.push_back([=] { ui->keyDown(vk); }); }
    void shot(const std::string& name) { script.push_back([=] { snap(name); }); }
};

MenuItem item(const std::string& t, int id, bool checked = false, bool radio = false) {
    MenuItem m;
    m.text = t;
    m.id = id;
    m.checked = checked;
    m.radio = radio;
    return m;
}
MenuItem sep() {
    MenuItem m;
    m.separator = true;
    return m;
}

}  // namespace

int main(int argc, char** argv) {
    Harness H(argc > 1 ? argv[1] : "");
    PlatformUiDrawn& ui = *H.ui;

    std::printf("popup menus:\n");
    std::vector<MenuItem> opts;
    MenuItem steal = item("&Voice stealing mode...", 0);
    steal.sub = {item("Set by program   (EMU->VSTEAL parameter)", 10, false, true), sep(),
                 item("HARD", 11, false, true), item("SOFT", 12, true, true)};
    opts.push_back(steal);
    MenuItem off = item("Disabled item", 13);
    off.enabled = false;
    opts.push_back(off);
    opts.push_back(sep());
    opts.push_back(item("&Right click on display -> scroll page", 14, true));
    MenuItem def = item("Default (bold) item", 15);
    def.isDefault = true;
    opts.push_back(def);
    // OPTIONS at (66, 24): row 0 = voice stealing (y 26..44), its sub-menu opens to the right
    H.move(100, 34);
    H.shot("menu_submenu");
    H.click(330, 86);  // "SOFT" (4th row of the sub-menu)
    int r = ui.popupMenu(opts, 66, 24);
    check(r == 12, "hover opens the sub-menu, click chooses SOFT (" + std::to_string(r) + ")");
    check(!ui.modal(), "menu closed after the choice");

    H.click(600, 400);
    r = ui.popupMenu(opts, 66, 24);
    check(r == 0, "click outside dismisses (" + std::to_string(r) + ")");

    H.script.push_back([&] { ui.mouseUp(MouseButton::Right, 66, 24); });  // release of the opening click
    H.key(kVkDown);
    H.key(kVkDown);
    H.key(kVkReturn);
    r = ui.popupMenu(opts, 66, 24);
    check(r == 14, "opening release chooses nothing; Down, Down (skips disabled), Enter (" + std::to_string(r) + ")");

    H.click(80, 70);  // the disabled item
    H.key(kVkEscape);
    r = ui.popupMenu(opts, 66, 24);
    check(r == 0, "disabled item can't be chosen; Escape dismisses (" + std::to_string(r) + ")");

    // the program list: 4 columns of a bank header, a separator and 32 programs
    std::vector<MenuItem> prog;
    for (int b = 0; b < 4; b++) {
        MenuItem h = item(std::string("Bank ") + char('A' + b) + (b < 2 ? "   (user)" : b == 2 ? "   (factory)" : "   (SQ-80 factory)"),
                          1000 + b, b == 0, true);
        h.barBreak = b > 0;
        prog.push_back(h);
        prog.push_back(sep());
        for (int p = 0; p < 32; p++) {
            char t[32];
            std::snprintf(t, sizeof t, "A%03d   %s", b * 32 + p, p % 3 ? "DARK-STRINGS" : "WONDERLAND");
            prog.push_back(item(t, 2000 + b * 32 + p, b == 0 && p == 0, true));
        }
    }
    H.shot("menu_programs");
    H.key(kVkEscape);
    ui.popupMenu(prog, 30, 60);
    // find where it was laid out: click the 3rd column, 5th program by keyboard instead
    H.key(kVkDown);  // A000 (first enabled after the header? header is enabled)
    for (int i = 0; i < 70; i++) H.key(kVkDown);
    H.key(kVkReturn);
    r = ui.popupMenu(prog, 30, 60);
    check(r >= 1000, "program list (4 columns) works with the keyboard (" + std::to_string(r) + ")");
    {
        Bitmap b = H.editor;
        // the right edge column of the frame stays the editor's when the list is open
        H.key(kVkEscape);
        H.script.push_front([&] {
            Bitmap m = H.editor;
            ui.render(m);
            int changedRight = 0, changedLeft = 0;
            for (int y = 60; y < 400; y++) {
                if (m.pixel(625, y) != b.pixel(625, y)) changedRight++;
                if (m.pixel(600, y) != b.pixel(600, y)) changedLeft++;  // the 4th column
            }
            check(changedLeft > 0 && changedRight == 0, "program list fits in the 626-pixel window");
        });
        ui.popupMenu(prog, 30, 60);
    }

    std::printf("message boxes:\n");
    H.shot("message_okcancel");
    H.key(kVkReturn);
    r = ui.messageBox("Load bank? (This will erase all programs in bank A !)", "Warning", kMbOkCancel);
    check(r == kIdOk, "Enter = OK (" + std::to_string(r) + ")");
    H.key(kVkEscape);
    r = ui.messageBox("Load bank?", "Warning", kMbOkCancel);
    check(r == kIdCancel, "Escape = Cancel (" + std::to_string(r) + ")");
    H.key(kVkEscape);
    r = ui.messageBox("This file could not be identified as ESQ-1/SQ-80 SysEx. Try to load anyway?", "Warning", kMbYesNo);
    check(r == kIdNo, "Escape = No (" + std::to_string(r) + ")");
    // click the second button: buttons are bottom right, 75x23, 8 apart
    H.script.push_back([&] {});
    r = -1;
    {
        // find "Cancel" by scanning for a click that returns kIdCancel: the box is centred
        for (int x = 300; x < 600 && r != kIdCancel; x += 20) {
            for (int y = 200; y < 330 && r != kIdCancel; y += 6) {
                H.script.clear();
                H.click(x, y);
                H.key(kVkReturn);  // fallback: OK
                r = ui.messageBox("Load bank?", "Warning", kMbOkCancel);
            }
        }
    }
    check(r == kIdCancel, "a click on Cancel answers Cancel");

    std::printf("dialogs:\n");
    std::vector<std::string> ports;
    for (int i = 0; i < 30; i++) ports.push_back("MIDI port " + std::to_string(i + 1));
    MidiSelDialog d("Select MIDI output", true, 2, ports);
    // TmidiSelForm 194x314 centred on (315, 214): list at client (8, 8), 13-pixel rows
    const int left = 315 - 97, top = 214 - (314 + 20) / 2 + 20;
    H.script.clear();
    H.click(left + 20, top + 8 + 2 + 5 * 13 + 4);  // row 5
    H.shot("dialog_midi");
    H.key(kVkDown);
    H.script.push_back([&] { ui.wheel(left + 50, top + 100, -1); });
    H.click(left + 8 + 40, top + 280 + 12);  // OK
    d.showModal(ui);
    check(d.modalResult == 6 + 9 && d.itemIndex == 6,  // the original returns the port + 9
          "click row 5, Down, OK (result " + std::to_string(d.modalResult) + ", item " + std::to_string(d.itemIndex) + ")");
    check(H.log.size() > 4, "dialog actions run under the engine lock with a pump");

    MidiSelDialog d2("Select MIDI input", false, 0, ports);
    H.script.clear();
    H.key(kVkEscape);
    d2.showModal(ui);
    check(d2.modalResult == kMrCancel, "Escape cancels the dialog");

    std::printf("info windows:\n");
    H.script.clear();

    std::vector<std::string> mod;
    for (int i = 0; i < 40; i++) mod.push_back("LFO" + std::to_string(i % 4 + 1) + " -> OSC" + std::to_string(i % 3 + 1));
    H.shot("modinfo");
    H.key('A');
    ui.showModInfo(mod);
    check(!ui.modal(), "Modulation sources closes with a key");

    std::printf("program name box:\n");
    H.log.clear();
    ui.beginNameEdit(110, 48, 158, 15, "TPIANO");
    for (char c : std::string("xy")) ui.character(static_cast<uint8_t>(c));
    ui.keyDown(kVkBack);
    ui.keyDown(kVkHome);
    ui.character('a');
    H.snap("name_edit");
    ui.keyDown(kVkReturn);
    check(!ui.nameEditing(), "Enter ends the editing");
    const std::vector<std::string> want = {"focus TPIANO", "key 13", "blur ATPIANOX"};
    check(H.log == want, "focus, typed upper case, Backspace, Home, Enter: " +
                             (H.log.size() == 3 ? H.log[0] + " | " + H.log[1] + " | " + H.log[2] : std::to_string(H.log.size())));
    ui.beginNameEdit(110, 48, 158, 15, "ABCDEFGHIJKLMNO");
    ui.character('P');
    ui.focusForm();
    check(H.log.back() == "blur ABCDEFGHIJKLMNO", "at most 15 characters; focus elsewhere ends the editing");

    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
