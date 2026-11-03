#include "SoundParameterLabels.h"
#include "Program.h"
#include <string>
namespace sq8l::sound::labels {
// oscillator waves (FUN_00461114)
const char* const kWaveNames[75] = {
    "   SAW", "  BELL", "  SINE", "SQUARE", " PULSE", "NOISE1", "NOISE2", "NOISE3",
    "  BASS", " PIANO", "EL PNO", "VOICE1", "VOICE2", "  KICK", "  REED", " ORGAN",
    "SYNTH1", "SYNTH2", "SYNTH3", "FORMT1", "FORMT2", "FORMT3", "FORMT4", "FORMT5",
    "PULSE2", "SQR  2", "4 OCTS", " PRIME", "BASS 2", "E PNO2", "OCTAVE", " OCT+5",
    "SAW  2", "TRIANG", "REED 2", "REED 3", "GRIT 1", "GRIT 2", "GRIT 3", "GLINT1",
    "GLINT2", "GLINT3", "  CLAV", " BRASS", "STRING", "DIGIT1", "DIGIT2", "BELL 2",
    " ALIEN", "BREATH", "VOICE3", " STEAM", " METAL", " CHIME", "BOWING", "PICK 1",
    "PICK 2", "MALLET", "  SLAP", " PLINK", " PLUCK", " PLUNK", " CLICK", " CHIFF",
    " THUMP", "LOGDRM", "KICK 2", " SNARE", "TOMTOM", "HI-HAT", "DRUMS1", "DRUMS2",
    "DRUMS3", "DRUMS4", "DRUMS5",
};
// modulation sources -1..145 (FUN_00461184)
const char* const kModSourceNames[147] = {
    " *OFF*", "  LFO1", "  LFO2", "  LFO3", "  LFO4", "  ENV1", "  ENV2", "  ENV3",
    "  ENV4", "   VEL", " VEL-X", "  KEYB", " KEYB2", " PRESS", "  MAT1", "  MAT2",
    "  MAT3", "  BANK", " WHEEL", "BREATH", " CTR 3", " PEDAL", " PORTA", "  DATA",
    "VOLUME", "BALANC", " CTR 9", "   PAN", "EXPRES", "EFFCT1", "EFFCT2", "CTR 14",
    "CTR 15", "GENP 1", "GENP 2", "GENP 3", "GENP 4", "CTR 20", "CTR 21", "CTR 22",
    "CTR 23", "CTR 24", "CTR 25", "CTR 26", "CTR 27", "CTR 28", "CTR 29", "CTR 30",
    "CTR 31", "BANKLO", "WHEELL", "BRTHLO", "CTR 35", "PEDALL", "PORTAL", "DATALO",
    "VOL LO", "BANKLO", "CTR 41", "PAN LO", "EXPRLO", "EFF1LO", "EFF2LO", "CTR 46",
    "CTR 47", "CTR 48", "CTR 49", "CTR 50", "CTR 51", "CTR 52", "CTR 53", "CTR 54",
    "CTR 55", "CTR 56", "CTR 57", "CTR 58", "CTR 59", "CTR 60", "CTR 61", "CTR 62",
    "CTR 63", "HLDPED", "PORTON", "SUSPED", "SFTPED", "LEGPED", "HLD2PD", "VARIAT",
    "TIMBRE", "RELEAS", "ATTACK", "BRIGHT", " SND 6", " SND 7", " SND 8", " SND 9",
    "SND 10", "GENP1H", "GENP2H", "GENP3H", "GENP4H", "CTR 84", "CTR 85", "CTR 86",
    "CTR 87", "CTR 88", "CTR 89", "CTR 90", "EFFLEV", "TREMOL", "CHORUS", "CELEST",
    "PHASER", "DATINC", "DATDEC", "NREGLO", "NREGHI", " REGLO", " REGHI", "CTR102",
    "CTR103", "CTR104", "CTR105", "CTR106", "CTR107", "CTR108", "CTR109", "CTR110",
    "CTR111", "CTR112", "CTR113", "CTR114", "CTR115", "CTR116", "CTR117", "CTR118",
    "CTR119", "SNDOFF", "CTROFF", " LOCAL", "ALLOFF", "OMNOFF", "OMNION", "  MONO",
    "  POLY", "CHPRES", "  BEND",
};
// LFO basic waves (FUN_004612e4)
const char* const kLfoWaveNames[5] = {
    "TRI", "SAW", "SQR", "NOI", "BIP",
};
// EXP/TAN shapes (FUN_00461928, LFO waves 75..82)
const char* const kShapeNames[8] = {
    "EXP ", "EXP2", "EXP3", "EXP4", "TAN ", "TAN2", "TAN3", "TAN4",
};
// pitch bend modes (FUN_004619d4)
const char* const kBendModeNames[7] = {
    "ALL", "HELD", "NEW", "NEWH", "HELD2", "NEW2", "NEWH2",
};
// LFO waves 5..74 -> oscillator wave (FUN_004613dc)
const uint8_t kLfoWaveMap[70] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
    28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
    42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55,
    56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69,
};


namespace {
std::string intToStr(int v) { return std::to_string(v); }
void zeroPad(std::string& s, int n) { if(n > int(s.size())) s.insert(0, size_t(n)-s.size(), '0'); }
std::string intToStrZ(int v, int n) { auto s=std::to_string(v < 0 ? -v : v); zeroPad(s,n-(v<0)); return v<0 ? "-"+s : s; }
std::string strWidth(int v, int n) { auto s=std::to_string(v); if(n>int(s.size())) s.insert(0,size_t(n)-s.size(),' '); return s; }
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

// (port) kSyncQuarters[i] as the display shows it, right-aligned in 3 cells. X marks bars:
// B is D plus one segment on the VFD, so 4B and 4D would look alike.
const char* divisionName(int i) {
    static const char* const k[kSyncDivisions] = {
        "16X", "15X", "14X", "13X", "12X", "11X", "10X", " 9X", " 8X", " 7X", " 6X", " 5X",
        " 4X", " 3X", " 2X", " 1D", "  1", " 2D", " 1T", "  2", " 4D", " 2T", "  4", " 8D",
        " 4T", "  8", "16D", " 8T", " 16", "32D", "16T", " 32", "64D", "32T", " 64", "64T"};
    return k[i < 0 ? 0 : (i < kSyncDivisions ? i : kSyncDivisions - 1)];
}

}
std::string format(LabelKind f, int v, int width) {
    std::string s;
    switch(f) {
    case LabelKind::Wave:  // 0x461114
        if (v < 0) s = "*OFF*";
        else if (v < 0x4b) s = kWaveNames[v];
        else s = "WAV???";
        break;
    case LabelKind::ModSource:  // 0x461184
        s = (v >= 0 && v < 0x92) ? kModSourceNames[v + 1] : kModSourceNames[0];
        break;
    case LabelKind::LfoReset:  // 0x4612a4
        s = v < 0 ? std::string("OFF") : intToStrZ(v, 2);
        break;
    case LabelKind::LfoWave:  // 0x4612e4
        // (port) the field is 6 cells wide (the original's 4 only fitted a number), so the
        // oscillator waves spell out the name that PopText::LfoWave already builds for the
        // popup. Everything is right-aligned in 6, like kWaveNames itself.
        if (v < 0) {
            s = "   ?  ";
        } else if (v < 5) {
            s = std::string("   ") + kLfoWaveNames[v];  // ShortString[4] concatenation
        } else if (v - 5 <= 0x45) {
            s = kWaveNames[kLfoWaveMap[v - 5]];
        } else if (v - 5 - 0x46 < 8) {
            s = std::string("  ") + kShapeNames[v - 5 - 0x46];
        } else {
            s = "   ?  ";
        }
        break;
    case LabelKind::LfoHuman: {  // 0x4614e4
        static const char* const k[7] = {"OFF", " ON", " X1", " X2", " X4", " X8", "X16"};
        s = (unsigned(v) <= 6) ? k[v] : " ? ";
        break;
    }
    case LabelKind::LfoPhase:  // 0x4615f0
        s = v < 0x40 ? intToStrZ(v, 2) + " " : intToStrZ(v & 0x3f, 2) + "T";
        break;
    case LabelKind::LfoDelayMode:  // 0x461698
        s = v == 1 ? "SMTH" : "EMU ";
        break;
    case LabelKind::LfoModMode:  // 0x4616e4
        switch (v) {
        case 0: s = "UNI"; break;
        case 1: s = "BIP"; break;
        case 2: s = "PHS"; break;
        case 3: s = "SMT"; break;
        default: s = " ? "; break;
        }
        break;
    case LabelKind::LfoPlay:  // 0x461784 (other values: s unchanged)
        switch (v) {
        case 0: s = "FWD"; break;
        case 1: s = "REV"; break;
        case 2: s = "1XF"; break;
        case 3: s = "1XR"; break;
        default: break;
        }
        break;
    case LabelKind::EnvVelLevel:  // 0x461808
        twoDigitsFlag(v, "L", "X", s);
        break;
    case LabelKind::EnvT4:  // 0x461898
        twoDigitsFlag(v, " ", "R", s);
        break;
    case LabelKind::EnvShape:  // 0x461928
        if (v - 1 < 0) s = " OFF";
        else if (v - 1 <= 7) s = kShapeNames[v - 1];
        else s = " ? ";
        break;
    case LabelKind::EnvT1VMode:  // 0x461990
        s = v > 0 ? "SMT" : "T1 ";
        break;
    case LabelKind::BendMode:  // 0x4619d4
        s = unsigned(v) < 7 ? kBendModeNames[v] : "?";
        break;
    case LabelKind::Saturation:  // 0x461a14
        if (v == -1) s = "OFF";
        else if (v == 0) s = "EMU";
        else s = "+" + intToStrZ(v, 2);
        break;
    case LabelKind::VoiceSteal:  // 0x461ac0
        s = v == -1 ? "HARD" : "SOFT";
        break;
    case LabelKind::Dca13Mode:  // 0x461b10
        s = v == 1 ? "FAST" : "EMU ";
        break;
    case LabelKind::Dca4Mode:  // 0x461b5c
        s = v == 0 ? "EMU " : "HARD";
        break;
    case LabelKind::DcBlock:  // 0x461bac
        s = v == 0 ? "SMART" : (v == 1 ? "ON   " : "OFF  ");
        break;
    case LabelKind::Polyphony:  // (port) EMU -> VOICES
        // 0 is what every program of the original has: its 8 voices.
        s = intToStr(v == 0 ? 8 : v);
        zeroPad(s, width);
        break;
    case LabelKind::WaveDir:  // (port) wave playback direction
        // Two cells of the 16-segment font: a centre bar and a chevron whose apex faces it.
        // "-<" and ">-" would not read as arrows (the chevron sits at the far edge of its
        // cell, pointing away), so the arrow always leads with the bar it closes on.
        s = v != 0 ? "<-" : "->";
        break;
    case LabelKind::TempoSync:  // (port) LFO or envelope tempo sync
        s = v != 0 ? "SYNC" : "FREE";
        break;
    case LabelKind::LfoDivision:  // (port) a synced LFO's FREQ
        s = divisionName(v);
        break;
    case LabelKind::EnvDivision:  // (port) a synced envelope time, shortest first
        s = divisionName(kSyncDivisions - 1 - v);
        break;
    case LabelKind::EnvT4Division:  // (port) a synced T4: the second half of the range is "R"
        s = std::string(divisionName(kSyncDivisions - 1 - v % kSyncDivisions)) +
            (v >= kSyncDivisions ? "R" : " ");
        break;
    default: break;
    }
    return s;
}
} // namespace sq8l::sound::labels
