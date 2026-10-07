// Additions of the port to the VFD pages (GuiLogicData.cpp is generated from the original
// DLL and must not be edited): the VOICES parameter on the EMU page, the playable voices of
// the program (program byte 0x197, parameter 375; see docs/modules/program.md).
//
// The page is derived from the generated one instead of being a copy of it, so re-running
// re/scripts/extract_gui_logic_data.py keeps working. If the generated page ever stops
// matching what is patched here, the result is the original's page without the control and
// tests/test_gui_extensions.py fails on it (it checks the knob, the column and the row).
#include <string>
#include <vector>

#include "../LcdData.h"

namespace sq8l::gui::data {

namespace {

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
        out.push_back({kVoicesIndex, "Playable voices of this program (08 = the SQ80's, a port addition)",
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

}  // namespace

const PageSetup& pageSetup(int page, bool portExtensions) {
    if (portExtensions && page == kPortEmuPage) return emuPage();
    return kPages[page];
}

}  // namespace sq8l::gui::data
