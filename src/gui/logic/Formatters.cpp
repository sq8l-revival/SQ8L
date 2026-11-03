#include "Formatters.h"

#include <cstdint>
#include <cstdlib>

#include "Program.h"     // (port) kSyncDivisions
#include "VoiceSlots.h"  // (port) kOriginalPlayableVoices

namespace sq8l::gui {

namespace delphi {

std::string intToStr(int v) { return std::to_string(v); }

std::string strWidth(int v, int width) {
    std::string s = std::to_string(v);
    if (int(s.size()) < width) s.insert(0, size_t(width) - s.size(), ' ');
    return s;
}

void zeroPad(std::string& s, int n) {
    if (n > int(s.size())) s.insert(0, size_t(n) - s.size(), '0');
}

std::string intToStrZ(int v, int n) {
    if (n <= 0) return {};
    // Str(abs(v)) computed with cdq/xor/sub (INT_MIN stays negative)
    const int32_t a = int32_t((uint32_t(v) ^ uint32_t(v >> 31)) - uint32_t(v >> 31));
    std::string s = std::to_string(a);
    if (v >= 0) {
        zeroPad(s, n);
    } else {
        zeroPad(s, n - 1);
        s = "-" + s;
    }
    return s;
}

int numDigits(int v) {
    int n = 1;
    while (v >= 10) {
        n++;
        v /= 10;
    }
    return n;
}

std::string stringOfChar(char c, int n) { return n > 0 ? std::string(size_t(n), c) : std::string(); }

}  // namespace delphi

using namespace delphi;


void formatValue(Fmt f, int width, int v, std::string& s) {
    switch (f) {
    case Fmt::None:
        break;
    case Fmt::OffOn:  // 0x458ff8
        s = v > 0 ? " ON" : "OFF";
        break;
    case Fmt::Unsigned:  // 0x45903c
        s = intToStr(v);
        zeroPad(s, width);
        break;
    case Fmt::Signed:  // 0x45907c
        if (v >= 0) {
            s = intToStr(v);
            zeroPad(s, width - 1);
            s = "+" + s;
        } else {
            s = intToStr(-v);
            zeroPad(s, width - 1);
            s = "-" + s;
        }
        break;
    default:
        {
            const auto text = sound::labels::format(static_cast<sound::LabelKind>(f), v, width);
            if (!text.empty()) s = text; // LfoPlay preserves previous text for invalid storage.
        }
        break;
    }
}

void formatPopupText(PopText f, int v, std::string& s) {
    switch (f) {
    case PopText::None:
        break;
    case PopText::ModSource:  // 0x4611c4
        if (v < 0 || v >= 0x92) {
            s = data::kModSourceNames[0];
        } else if (v - 0x10 < 0 || v - 0x10 >= 0x80) {
            s = data::kModSourceNames[v + 1];
        } else {
            s = intToStrZ(v - 0x10, 3) + "   " + data::kModSourceNames[v + 1];
        }
        break;
    case PopText::LfoWave:  // 0x4613dc
        if (v < 0) {
            s = " ? ";
        } else if (v < 5) {
            s = data::kLfoWaveNames[v];
        } else if (v - 5 <= 0x45) {
            s = intToStrZ(v - 5, 2) + "   " + data::kWaveNames[data::kLfoWaveMap[v - 5]];
        } else if (v - 5 - 0x46 < 8) {
            s = data::kShapeNames[v - 5 - 0x46];
        } else {
            s = " ? ";
        }
        break;
    }
}

}  // namespace sq8l::gui
