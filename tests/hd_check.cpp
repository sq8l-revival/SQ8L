// HD graphics (src/gui/hd) without a window: what is read from the original's bitmaps (the
// display segments each frame lights, the knob angles), and the editor drawn at several
// scales, where redrawing only what changed must give the same pixels as a full redraw.
// With a directory, snapshots of the classic frame and the HD renders (PPM) for a visual check.
//
//   sq8l_hd_check [snapshot dir]
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "EditorView.h"
#include "hd/HdRenderer.h"
#include "text/StbTextRenderer.h"

using namespace sq8l::gui;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) failures++;
}

void save(const Bitmap& b, const std::string& path) {
    std::vector<uint8_t> rgb(static_cast<size_t>(b.width()) * b.height() * 3);
    b.toRGB24(rgb.data());
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", b.width(), b.height());
        std::fwrite(rgb.data(), 1, rgb.size(), f);
        std::fclose(f);
    }
}

// VFD segment bits (HdRenderer::vfdSegments)
enum : uint32_t {
    A1 = 1u << 0, A2 = 1u << 1, B = 1u << 2, C = 1u << 3, D1 = 1u << 4, D2 = 1u << 5, E = 1u << 6, F = 1u << 7,
    G1 = 1u << 8, G2 = 1u << 9, H = 1u << 10, I = 1u << 11, J = 1u << 12, K = 1u << 13, L = 1u << 14, M = 1u << 15,
    UNDERLINE = 1u << 16, DOT = 1u << 17,
};

int vfdFrame(char c, int attr) {
    const std::string chars = EditorView::kLcdCharset;
    return static_cast<int>(chars.find(c)) * 4 + attr;
}

int ledFrame(char c) { return static_cast<int>(std::string(EditorView::kNumLcdCharset).find(c)); }

std::string bits(uint32_t v) {
    char s[16];
    std::snprintf(s, sizeof s, "0x%05x", v);
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "";

    std::printf("VFD characters (charGIF):\n");
    const struct {
        char c;
        uint32_t segments;
    } vfd[] = {{' ', 0},
               {'8', A1 | A2 | B | C | D1 | D2 | E | F | G1 | G2},
               {'0', A1 | A2 | B | C | D1 | D2 | E | F | J | K},
               {'+', G1 | G2 | I | L},
               {'X', H | J | K | M},
               {'M', B | C | E | F | H | J},
               {'K', E | F | G1 | J | M},
               {'D', A1 | A2 | B | C | D1 | D2 | I | L},
               {'.', UNDERLINE},
               {',', K}};
    for (const auto& t : vfd) {
        const uint32_t s = HdRenderer::vfdSegments(vfdFrame(t.c, 0));
        check(s == t.segments, std::string("'") + t.c + "' lights " + bits(s) + " (expected " + bits(t.segments) + ")");
    }
    // the attributes: 1 underline, 2 decimal point, 3 both, on every character
    bool attrs = true;
    for (char c : std::string(EditorView::kLcdCharset)) {
        const uint32_t plain = HdRenderer::vfdSegments(vfdFrame(c, 0));
        attrs = attrs && !(plain & DOT) && HdRenderer::vfdSegments(vfdFrame(c, 1)) == (plain | UNDERLINE) &&
                HdRenderer::vfdSegments(vfdFrame(c, 2)) == (plain | DOT) &&
                HdRenderer::vfdSegments(vfdFrame(c, 3)) == (plain | UNDERLINE | DOT);
    }
    check(attrs, "attributes 1-3 add the underline, the decimal point, both (all 68 characters)");

    std::printf("Red digits (numCharGIF):\n");
    const struct {
        char c;
        uint32_t segments;
    } led[] = {{' ', 0}, {'8', 0x7f}, {'1', 0x06}, {'7', 0x27}, {'P', 0x73}, {'U', 0x3e}};
    for (const auto& t : led) {
        const uint32_t s = HdRenderer::ledSegments(ledFrame(t.c));
        check(s == t.segments, std::string("'") + t.c + "' lights " + bits(s) + " (expected " + bits(t.segments) + ")");
    }

    std::printf("Knob (KnobGif):\n");
    bool increasing = true;
    for (int f = 1; f < 128; f++) increasing = increasing && HdRenderer::knobAngle(f) > HdRenderer::knobAngle(f - 1);
    check(increasing, "the pointer turns clockwise frame after frame");
    check(std::fabs(HdRenderer::knobAngle(0) + 135) < 2 && std::fabs(HdRenderer::knobAngle(127) - 135) < 2,
          "from -135 to +135 degrees (" + std::to_string(HdRenderer::knobAngle(0)) + ", " +
              std::to_string(HdRenderer::knobAngle(127)) + ")");
    check(std::fabs(HdRenderer::knobAngle(63) + HdRenderer::knobAngle(64)) < 4, "up between frames 63 and 64");

    std::printf("Editor:\n");
    StbTextRenderer text{true};
    EditorView view;
    view.setTextRenderer(&text);
    view.lcd().writeText(0, 0, "OCT SEMI FINE  WAVE  MOD1 DEP1 MOD2 DEP2 OSC1", 0);
    view.lcd().writeText(0, 1, " +2   +7   +3  SAW   LFO1  +15 ENV3  -07  ON", 0);
    view.lcd().writeText(0, 1, " +2", 1);
    view.numLcd().writeText(0, 0, "A 12", 0);
    for (int i = 0; i < 10; i++) view.knob(i).setValue(static_cast<float>(i * 25 - 120));
    view.ledSync().setValue(1);
    view.button("buttOsc1").setAniIdx(2);
    view.button("pscrUpButton").setHover(true);
    view.setStatusText("Oscillator 1: octave, semitone, fine tuning and waveform");
    view.setVoicesText("3/16");
    view.progNameEdit().setText("SYNTH-BRASS");
    Bitmap classic{EditorView::kWidth, EditorView::kHeight};
    view.render(classic);
    if (!dir.empty()) save(classic, dir + "/classic.ppm");

    HdRenderer hd(text);
    const Rect all{0, 0, EditorView::kWidth, EditorView::kHeight};
    for (double S : {1.0, 1.25, 1.5, 2.0, 3.0}) {
        Bitmap full;
        hd.capture(view);
        const Rect r = hd.render(classic, S, full, all);
        const int w = static_cast<int>(std::lround(626 * S)), h = static_cast<int>(std::lround(430 * S));
        check(full.width() == w && full.height() == h && r.left == 0 && r.top == 0 && r.right == w && r.bottom == h,
              "scale " + std::to_string(S).substr(0, 4) + ": " + std::to_string(full.width()) + "x" +
                  std::to_string(full.height()) + ", all redrawn");
        if (!dir.empty()) save(full, dir + "/hd_" + std::to_string(static_cast<int>(S * 100)) + ".ppm");

        // turn a knob, edit the display, light a button and an LED: only that is redrawn
        Bitmap inc = full, before = classic, after{EditorView::kWidth, EditorView::kHeight};
        view.knob(2).setValue(77);
        view.lcd().writeText(5, 1, "-4", 1);
        view.button("buttOsc2").setAniIdx(2);
        view.ledAm().setValue(1);
        view.render(after);
        const Rect dirty = HdRenderer::changedArea(before, after);
        hd.capture(view);
        const Rect redrawn = hd.render(after, S, inc, dirty);
        hd.render(after, S, full, all);
        check(inc.pixels() == full.pixels() && !redrawn.empty() && redrawn.width() < w,
              "  what changed redrawn alone: same pixels as a full redraw");
        check(hd.render(after, S, inc, HdRenderer::changedArea(after, after)).empty(),
              "  nothing changed: nothing redrawn");
        view.knob(2).setValue(-70);
        view.lcd().writeText(5, 1, "+7", 0);
        view.button("buttOsc2").setAniIdx(0);
        view.ledAm().setValue(0);
        view.render(classic);
    }

    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "OK", failures);
    return failures ? 1 : 0;
}
