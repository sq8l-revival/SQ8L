// Pure sound choice data. Ordering is shared by host metadata and the panel.
#pragma once
#include <cstdint>
#include <string>
namespace sq8l::sound {
enum class LabelKind : uint8_t {
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
    WaveDir,       // (port) oscillator wave direction: "->" forward, "<-" reverse
    TempoSync,     // (port) LFO or envelope tempo sync: "FREE" / "SYNC"
    LfoDivision,   // (port) a synced LFO's FREQ: 16X..2X, 1D..64T (kSyncQuarters)
    EnvDivision,   // (port) a synced envelope's T1-T3: 64T..16X, kSyncQuarters backwards
    EnvT4Division, // (port) a synced T4: EnvDivision, then the same with "R" (36..71)
};
namespace labels {
extern const char* const kWaveNames[75];
extern const char* const kModSourceNames[147];
extern const char* const kLfoWaveNames[5];
extern const char* const kShapeNames[8];
extern const char* const kBendModeNames[7];
extern const uint8_t kLfoWaveMap[70];
std::string format(LabelKind kind, int value, int width = 0);
} // namespace labels
} // namespace sq8l::sound
