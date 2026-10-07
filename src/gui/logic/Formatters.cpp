#include "Formatters.h"

#include <cstdint>
#include <cstdlib>

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

namespace {

int clamp127(int v) { return v <= 0 ? 0 : (v < 0x7f ? v : 0x7f); }  // FUN_00450d90

// FUN_00461808 / FUN_00461898: Str(v and 63:2) with a leading '0' for values below 10 and a
// suffix depending on bit 6 of the clamped value.
void twoDigitsFlag(int value, const char* lo, const char* hi, std::string& s) {
    const int v = clamp127(value);
    const int n = v & 0x3f;
    s = strWidth(n, 2);
    if (n < 10) s[0] = '0';
    s += v < 0x40 ? lo : hi;
}

}  // namespace

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
    case Fmt::Wave:  // 0x461114
        if (v < 0) s = "*OFF*";
        else if (v < 0x4b) s = data::kWaveNames[v];
        else s = "WAV???";
        break;
    case Fmt::ModSource:  // 0x461184
        s = (v >= 0 && v < 0x92) ? data::kModSourceNames[v + 1] : data::kModSourceNames[0];
        break;
    case Fmt::LfoReset:  // 0x4612a4
        s = v < 0 ? std::string("OFF") : intToStrZ(v, 2);
        break;
    case Fmt::LfoWave:  // 0x4612e4
        if (v < 0) {
            s = " ? ";
        } else if (v < 5) {
            s = std::string(" ") + data::kLfoWaveNames[v];   // ShortString[4] concatenation
        } else if (v - 5 <= 0x45) {
            s = " " + intToStrZ(v - 5, 2);
        } else if (v - 5 - 0x46 < 8) {
            s = data::kShapeNames[v - 5 - 0x46];
        } else {
            s = " ? ";
        }
        break;
    case Fmt::LfoHuman: {  // 0x4614e4
        static const char* const k[7] = {"OFF", " ON", " X1", " X2", " X4", " X8", "X16"};
        s = (unsigned(v) <= 6) ? k[v] : " ? ";
        break;
    }
    case Fmt::LfoPhase:  // 0x4615f0
        s = v < 0x40 ? intToStrZ(v, 2) + " " : intToStrZ(v & 0x3f, 2) + "T";
        break;
    case Fmt::LfoDelayMode:  // 0x461698
        s = v == 1 ? "SMTH" : "EMU ";
        break;
    case Fmt::LfoModMode:  // 0x4616e4
        switch (v) {
        case 0: s = "UNI"; break;
        case 1: s = "BIP"; break;
        case 2: s = "PHS"; break;
        case 3: s = "SMT"; break;
        default: s = " ? "; break;
        }
        break;
    case Fmt::LfoPlay:  // 0x461784 (other values: s unchanged)
        switch (v) {
        case 0: s = "FWD"; break;
        case 1: s = "REV"; break;
        case 2: s = "1XF"; break;
        case 3: s = "1XR"; break;
        default: break;
        }
        break;
    case Fmt::EnvVelLevel:  // 0x461808
        twoDigitsFlag(v, "L", "X", s);
        break;
    case Fmt::EnvT4:  // 0x461898
        twoDigitsFlag(v, " ", "R", s);
        break;
    case Fmt::EnvShape:  // 0x461928
        if (v - 1 < 0) s = " OFF";
        else if (v - 1 <= 7) s = data::kShapeNames[v - 1];
        else s = " ? ";
        break;
    case Fmt::EnvT1VMode:  // 0x461990
        s = v > 0 ? "SMT" : "T1 ";
        break;
    case Fmt::BendMode:  // 0x4619d4
        s = unsigned(v) < 7 ? data::kBendModeNames[v] : "?";
        break;
    case Fmt::Saturation:  // 0x461a14
        if (v == -1) s = "OFF";
        else if (v == 0) s = "EMU";
        else s = "+" + intToStrZ(v, 2);
        break;
    case Fmt::VoiceSteal:  // 0x461ac0
        s = v == -1 ? "HARD" : "SOFT";
        break;
    case Fmt::Dca13Mode:  // 0x461b10
        s = v == 1 ? "FAST" : "EMU ";
        break;
    case Fmt::Dca4Mode:  // 0x461b5c
        s = v == 0 ? "EMU " : "HARD";
        break;
    case Fmt::DcBlock:  // 0x461bac
        s = v == 0 ? "SMART" : (v == 1 ? "ON   " : "OFF  ");
        break;
    case Fmt::Polyphony:  // (port) EMU -> VOICES
        // 0 is what every program of the original has: its 8 voices.
        s = intToStr(v == 0 ? kOriginalPlayableVoices : v);
        zeroPad(s, width);
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
