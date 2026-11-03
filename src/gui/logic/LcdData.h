// Static data of the VFD pages: what the setup routine FUN_0047e340 (unit_47e330) passes to the
// page objects. Generated into data/GuiLogicData.cpp by re/scripts/extract_gui_logic_data.py.
#pragma once

#include <cstdint>
#include "SoundParameterLabels.h"

namespace sq8l::gui {

// Value formatters (ClcdCtr_param +0x38): the three defaults of lcdControl (chosen from the
// range, FUN_00458f98) and the custom ones of unit editBuffer (0x461114..0x461bac).
using Fmt = sound::LabelKind; // One shared semantic choice ordering.

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
using sound::labels::kWaveNames;
using sound::labels::kModSourceNames;
using sound::labels::kLfoWaveNames;
using sound::labels::kShapeNames;
using sound::labels::kBendModeNames;
using sound::labels::kLfoWaveMap;
extern const char kLcdChars[];                  // main VFD character set
extern const char kNumLcdChars[];               // program number display character set
}  // namespace data

}  // namespace sq8l::gui
