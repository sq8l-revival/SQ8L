// Static data of the VFD pages: what the setup routine FUN_0047e340 (unit_47e330) passes to the
// page objects. Generated into data/GuiLogicData.cpp by re/scripts/extract_gui_logic_data.py.
#pragma once

#include <cstdint>

namespace sq8l::gui {

// Value formatters (ClcdCtr_param +0x38): the three defaults of lcdControl (chosen from the
// range, FUN_00458f98) and the custom ones of unit editBuffer (0x461114..0x461bac).
enum class Fmt : uint8_t {
    None,          // no formatter
    OffOn,         // 0x458ff8  "OFF" / " ON"
    Unsigned,      // 0x45903c  zero padded to the field width
    Signed,        // 0x45907c  "+"/"-" and zero padded to width-1
    Wave,          // 0x461114  oscillator wave name
    ModSource,     // 0x461184  modulation source name
    LfoReset,      // 0x4612a4  "OFF" / 2 digits
    LfoWave,       // 0x4612e4  " TRI".." BIP", " 00".." 69", "EXP ".."TAN4"
    LfoHuman,      // 0x4614e4  OFF ON X1 X2 X4 X8 X16
    LfoPhase,      // 0x4615f0  2 digits + " " or "T" (twin mode)
    LfoDelayMode,  // 0x461698  "EMU " / "SMTH"
    LfoModMode,    // 0x4616e4  UNI BIP PHS SMT
    LfoPlay,       // 0x461784  FWD REV 1XF 1XR
    EnvVelLevel,   // 0x461808  2 digits + "L"/"X"
    EnvT4,         // 0x461898  2 digits + " "/"R"
    EnvShape,      // 0x461928  " OFF", EXP..TAN4
    EnvT1VMode,    // 0x461990  "T1 " / "SMT"
    BendMode,      // 0x4619d4  ALL HELD NEW NEWH HELD2 NEW2 NEWH2
    Saturation,    // 0x461a14  OFF EMU +01..+15
    VoiceSteal,    // 0x461ac0  HARD / SOFT
    Dca13Mode,     // 0x461b10  "EMU " / "FAST"
    Dca4Mode,      // 0x461b5c  "EMU " / "HARD"
    DcBlock,       // 0x461bac  SMART ON OFF
    Polyphony,     // (port) EMU -> VOICES: 0 = the original's 8, else the voices
};

// Popup menu item texts (ClcdCtr_param +0x40).
enum class PopText : uint8_t {
    None,
    ModSource,     // 0x4611c4  "nnn   NAME" for MIDI controllers
    LfoWave,       // 0x4613dc  "nn   WAVE" for the oscillator waves
};

// Value changed callback (ClcdCtr_param +0x58): only SYNC and AM use it (form FUN_0048489c).
enum class Change : uint8_t { None, Form };

struct ParamSetup {
    int16_t index;          // parameter index inside the sub-page (ClcdCtr_param +0x08)
    const char* hint;       // +0x14 status bar text
    int16_t paramIndex;     // +0x74 edit buffer parameter (-1: page base + ordinal)
    Fmt fmt;                // +0x38 custom formatter (sets +0x48), None = default
    PopText pop;            // +0x40
    bool showNumber;        // +0x64 popup items show "nnn   value"
    int16_t perColumn;      // +0x68 popup items per column (0 = 16)
    Change change;          // +0x58
};

struct SubPageSetup {
    const char* title;      // ClcdCtr_pageSL +0x14 (page popup menu)
    int16_t c;              // +0x0c (= page number)
    int16_t group;          // +0x10
    int16_t base;           // +0x24 first edit buffer parameter
    int16_t offset;         // +0x28 added to every parameter index
    int16_t indent;         // +0x2c[0] page menu indent level
    int16_t hidden;         // +0x2c[1] not listed in the page menu
    const ParamSetup* params;
    int16_t numParams;
};

constexpr int kNumPages = 18;
constexpr int kMaxSubPages = 3;

struct PageSetup {
    const char* definition; // the string passed to ClcdCtr_pageML.setText
    int numSubPages;
    SubPageSetup subPages[kMaxSubPages];
};

namespace data {
extern const PageSetup kPages[kNumPages];
// (port) the EMU page, the only one the port adds a control to (VOICES, see below).
constexpr int kPortEmuPage = 17;
// (port) kPages[page], or that page with the port's additions when the editor host allows
// them (EditorHost::portExtensions): page kPortEmuPage then has the VOICES control.
const PageSetup& pageSetup(int page, bool portExtensions);
extern const char* const kWaveNames[75];        // ShortString[6]
extern const char* const kModSourceNames[147];  // ShortString[7], source -1..145
extern const char* const kLfoWaveNames[5];      // ShortString[3]
extern const char* const kShapeNames[8];        // ShortString[4]
extern const char* const kBendModeNames[7];     // ShortString[7]
extern const uint8_t kLfoWaveMap[70];
extern const char kLcdChars[];                  // main VFD character set
extern const char kNumLcdChars[];               // program number display character set
}  // namespace data

}  // namespace sq8l::gui
