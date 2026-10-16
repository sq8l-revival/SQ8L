// Additions of the port to the VFD pages (GuiLogicData.cpp is generated from the original
// DLL and must not be edited):
//
// * VOICES on the EMU page, the playable voices of the program (program byte 0x197,
//   parameter 375; see docs/modules/program.md);
// * DIR on the three per-oscillator pages, the wave playback direction (program osc byte
//   0x0b, parameters 8 / 27 / 46). WAVE= shortens to WF= to pay for it, and the OSC1-3 and
//   LFO pages follow WF= so the label reads the same everywhere. The LFO field also widens
//   from 4 to 6 cells, which lets Fmt::LfoWave spell the oscillator wave names out instead
//   of collapsing them to the two-digit number the original's 4 columns forced.
//
// Every page is derived from the generated one instead of being a copy of it, so re-running
// re/scripts/extract_gui_logic_data.py keeps working. If a generated page ever stops matching
// what is patched here, the result is the original's page without the addition and
// tests/test_gui_extensions.py fails on it (it checks the knob, the column and the row).
#include <string>
#include <vector>

#include "../LcdData.h"

namespace sq8l::gui::data {

namespace {

// Replace `from` with `to` once; returns the string unchanged when it does not match.
std::string replaceOnce(const std::string& s, const std::string& from, const std::string& to) {
    const size_t at = s.find(from);
    return at == std::string::npos ? s : s.substr(0, at) + to + s.substr(at + from.size());
}

// Appended to the row of AMBUG: 18 spaces put the label at column 33, directly below VSTEAL
// (knob 4) on the row above, and knob 9 is the knob below knob 4. Value 1..64, width 2.
constexpr const char* kVoicesItem = " 18 [VOICES={9,VOICES,2,1,64}]";

// The 6th "[" item of the sub-page (after EMU, BEND, MODE, VSTEAL, AMBUG).
constexpr int kVoicesIndex = 5;
constexpr int kVoicesParam = 375;  // program 0x197, body 0x175

// The sub-page of page kPortEmuPage that VOICES is added to (the first "Emulation" one).
constexpr int kEmuSubPage = 1;

std::string emuDefinition() {
    const std::string s = kPages[kPortEmuPage].definition;
    const size_t ambug = s.find("[AMBUG=");
    if (ambug == std::string::npos) return s;
    const size_t rowEnd = s.find("\\&", ambug);  // the row break after AMBUG's row
    if (rowEnd == std::string::npos) return s;
    return s.substr(0, rowEnd) + kVoicesItem + s.substr(rowEnd);
}

// The generated parameter list of that sub-page plus the VOICES entry.
const std::vector<ParamSetup>& emuParams() {
    static const std::vector<ParamSetup> v = [] {
        const SubPageSetup& src = kPages[kPortEmuPage].subPages[kEmuSubPage];
        std::vector<ParamSetup> out(src.params, src.params + src.numParams);
        out.push_back({kVoicesIndex, "Playable voices of this program (8 matches the SQ-80)",
                       kVoicesParam, Fmt::Polyphony, PopText::None, false, 0, Change::None});
        return out;
    }();
    return v;
}

const PageSetup& emuPage() {
    static const std::string definition = emuDefinition();
    static const PageSetup page = [] {
        PageSetup p = kPages[kPortEmuPage];
        p.definition = definition.c_str();
        p.subPages[kEmuSubPage].params = emuParams().data();
        p.subPages[kEmuSubPage].numParams = static_cast<int16_t>(emuParams().size());
        return p;
    }();
    return page;
}

// ------------------------------------------------------------------ DIR, and WF= everywhere
//
// Row 1 of a per-oscillator page is full at 44 columns, so DIR is paid for out of WAVE= and
// the gap before it:
//
//   OSC1 OCT=## SEMI=### FINE=###    WAVE=######   the original, exactly 44
//   OSC1 OCT=## SEMI=### FINE=### WF=###### ##     with DIR, 42
//
// WF keeps knob 4 (1-based) and DIR takes knob 5, which #43 turns into a soft button because
// its range is two values. OCT, SEMI and FINE keep their columns and the wave value stays 6
// cells wide. The arrow sits at columns 40-41, centred on knob 5 (which is over column 40.3).
constexpr const char* kOscWaveOld = " 3 [WAVE={4,WAVE,6,0,74}] []";
constexpr const char* kOscWaveNew = " [WF={3,WAVE,6,0,74}] [{4,DIR,2,0,1}]";

// DIR is the 6th "[" item (after OSCn, OCT, SEMI, FINE, WF), taking the slot the original
// left as an empty "[]" there, so the MODS items below it keep their indices.
constexpr int kDirIndex = 5;
constexpr int kOscSubPage = 0;   // the OSCn sub-page; the DCAn one is untouched
constexpr int kDirOffset = 8;    // program osc byte 0x0b, inside the oscillator's block

// Taking an ordinal shifts every later one, so from DIR on the page names its parameters
// outright rather than counting from the page base, as the OSC1-3 page already does. These
// are the MODS items of the OSCn sub-page and their parameters inside the block.
constexpr int kOscModItem[4] = {7, 9, 10, 12};    // MOD1.SEL, MOD1.AMT, MOD2.SEL, MOD2.AMT
constexpr int kOscModOffset[4] = {4, 5, 6, 7};

const std::vector<std::vector<ParamSetup>>& oscParams() {
    static const std::vector<std::vector<ParamSetup>> all = [] {
        std::vector<std::vector<ParamSetup>> v;
        for (int n = 0; n < kPortNumOscPages; n++) {
            const SubPageSetup& s = kPages[kPortOsc1Page + n].subPages[kOscSubPage];
            std::vector<ParamSetup> out(s.params, s.params + s.numParams);
            for (ParamSetup& p : out)
                for (int k = 0; k < 4; k++)
                    if (p.index == kOscModItem[k])
                        p.paramIndex = static_cast<int16_t>(s.base + kOscModOffset[k]);
            out.push_back({kDirIndex, "Playback direction of oscillator",
                           static_cast<int16_t>(s.base + kDirOffset), Fmt::WaveDir, PopText::None, true, 0,
                           Change::None});
            v.push_back(std::move(out));
        }
        return v;
    }();
    return all;
}

const PageSetup& oscPage(int i) {
    static const std::vector<std::string> defs = [] {
        std::vector<std::string> v;
        for (int n = 0; n < kPortNumOscPages; n++)
            v.push_back(replaceOnce(kPages[kPortOsc1Page + n].definition, kOscWaveOld, kOscWaveNew));
        return v;
    }();
    static const std::vector<PageSetup> all = [&] {
        std::vector<PageSetup> v;
        for (int n = 0; n < kPortNumOscPages; n++) {
            PageSetup p = kPages[kPortOsc1Page + n];
            p.definition = defs[static_cast<size_t>(n)].c_str();
            p.subPages[kOscSubPage].params = oscParams()[static_cast<size_t>(n)].data();
            p.subPages[kOscSubPage].numParams =
                static_cast<int16_t>(oscParams()[static_cast<size_t>(n)].size());
            v.push_back(p);
        }
        return v;
    }();
    return all[static_cast<size_t>(i)];
}

// The OSC1-3 page shares one label across the three wave fields, so it only follows the
// rename. WF= sits at columns 8-10, so its "=" lines up with the one of OCT= on the row
// below (7-10); the second of the two columns WAVE= gives up goes back in front of the first
// wave, which keeps all three values -- and SYNC -- exactly where they were.
constexpr const char* kMixWaveOld = "[OSC1-3] [WAVE=]";
constexpr const char* kMixWaveNew = "[OSC1-3] 1 [WF=] 1";

const PageSetup& mixPage() {
    static const std::string definition =
        replaceOnce(kPages[kPortOscMixPage].definition, kMixWaveOld, kMixWaveNew);
    static const PageSetup page = [] {
        PageSetup p = kPages[kPortOscMixPage];
        p.definition = definition.c_str();
        return p;
    }();
    return page;
}

// The LFO pages follow the rename and spend the column it frees, plus one of the two
// single-space gaps, on a 6-cell field: wide enough for Fmt::LfoWave to spell the
// oscillator wave names instead of the two-digit number 4 cells forced.
constexpr const char* kLfoWaveOld = " 1 [WAV={4,WAVE,4,0,82}]";
constexpr const char* kLfoWaveNew = " [WF={4,WAVE,6,0,82}]";

const PageSetup& lfoPage(int i) {
    static const std::vector<std::string> defs = [] {
        std::vector<std::string> v;
        for (int n = 0; n < kPortNumLfoPages; n++)
            v.push_back(replaceOnce(kPages[kPortLfo1Page + n].definition, kLfoWaveOld, kLfoWaveNew));
        return v;
    }();
    static const std::vector<PageSetup> all = [&] {
        std::vector<PageSetup> v;
        for (int n = 0; n < kPortNumLfoPages; n++) {
            PageSetup p = kPages[kPortLfo1Page + n];
            p.definition = defs[static_cast<size_t>(n)].c_str();
            v.push_back(p);
        }
        return v;
    }();
    return all[static_cast<size_t>(i)];
}

}  // namespace

const PageSetup& pageSetup(int page) {
    if (page == kPortEmuPage) return emuPage();
    if (page == kPortOscMixPage) return mixPage();
    if (page >= kPortOsc1Page && page < kPortOsc1Page + kPortNumOscPages)
        return oscPage(page - kPortOsc1Page);
    if (page >= kPortLfo1Page && page < kPortLfo1Page + kPortNumLfoPages)
        return lfoPage(page - kPortLfo1Page);
    return kPages[page];
}

}  // namespace sq8l::gui::data
