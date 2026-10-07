// SQ8L sound program: the raw 540-byte record (0x21c) used by the original everywhere (edit
// buffer, library, library/bank files, host chunks). The layout is kept byte for byte; named
// offsets and accessors are provided on top of it. See docs/modules/program.md for the full
// field table (meaning, range, encoding).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "VoiceSlots.h"

namespace sq8l {

constexpr size_t kProgramSize = 0x21c;  // 540

// A modulation slot: 16-bit source (-1 = OFF, see ModSource) followed by a signed amount.
struct ModSlot {
    int16_t source;
    int8_t amount;
};

// Modulation source numbers (int16 in mod slots). 16 + n = MIDI CC n (0..127).
namespace ModSource {
constexpr int16_t Off = -1;
constexpr int16_t Lfo1 = 0, Lfo2 = 1, Lfo3 = 2, Lfo4 = 3;
constexpr int16_t Env1 = 4, Env2 = 5, Env3 = 6, Env4 = 7;
constexpr int16_t Vel = 8, VelX = 9, Keyb = 10, Keyb2 = 11, Press = 12;  // Press = poly key pressure
constexpr int16_t Mat1 = 13, Mat2 = 14, Mat3 = 15;
constexpr int16_t Cc0 = 16;  // CC n -> 16 + n; Wheel = 17, Breath = 18, ...
constexpr int16_t ChannelPressure = 144, PitchBend = 145;
constexpr int16_t Max = 145;
}  // namespace ModSource

// Field offsets inside the 540-byte record (absolute, from the start of the record).
namespace ofs {
constexpr size_t Version = 0x000;  // uint16, always 2
constexpr size_t Name = 0x002;     // ShortString[15]: length byte + 15 chars
constexpr size_t Name2 = 0x012;    // second ShortString[15] (unused by the 0.91b GUI, empty)
constexpr size_t Body = 0x022;     // start of the parameter body (CparamEditor base)
constexpr size_t BodySize = 0x18e; // 0x022..0x1af
constexpr size_t Ext = 0x1b0;      // 0x6c-byte extension block (kept by the edit buffer)
constexpr size_t ExtSize = 0x6c;

// Oscillators 1..3: 24-byte blocks.
constexpr size_t osc(int i) { return 0x022 + 0x18 * size_t(i); }
namespace Osc {
constexpr size_t Oct = 0x00;      // s8  -3..+5
constexpr size_t Semi = 0x01;     // s8  -12..+12 (SQ80 0..11)
constexpr size_t Fine = 0x02;     // s8  -31..+31 (SQ80 0..31)
constexpr size_t Wave = 0x03;     // s16 0..74
constexpr size_t PitchMod1 = 0x05;// slot (FM source 1 / amount -63..63)
constexpr size_t PitchMod2 = 0x08;// slot
constexpr size_t Level = 0x0e;    // s8  DCA level 0..63
constexpr size_t Enable = 0x0f;   // s8  DCA output on/off 0..1
constexpr size_t AmpMod1 = 0x10;  // slot (DCA modulation)
constexpr size_t AmpMod2 = 0x13;  // slot
// 0x0b..0x0d and 0x16..0x17 are unused (always 0).
}  // namespace Osc

// Envelopes 1..4: 14-byte blocks.
constexpr size_t env(int i) { return 0x06a + 0x0e * size_t(i); }
namespace Env {
constexpr size_t L0 = 0x00;   // s8 start level -63..+63
constexpr size_t L1 = 0x01;   // s8 -63..+63
constexpr size_t L2 = 0x02;
constexpr size_t L3 = 0x03;
// 0x04 unused
constexpr size_t LV = 0x05;   // s8 0..127: bits 0-5 velocity -> levels, bit 6 exponential ("X")
constexpr size_t T1V = 0x06;  // s8 0..63
constexpr size_t T1 = 0x07;   // s8 0..63
constexpr size_t T2 = 0x08;
constexpr size_t T3 = 0x09;
constexpr size_t T4 = 0x0a;   // s8 0..127: bits 0-5 release time, bit 6 = 2nd release ("R")
constexpr size_t TK = 0x0b;   // s8 0..63
constexpr size_t Flags = 0x0c;// bit 0 CYC, bits 1-3 SHAPE (0..7), bit 4 T1V mode (0 T1, 1 SMT)
constexpr size_t Smooth = 0x0d;// s8 0..63
}  // namespace Env

// LFOs 1..4: 16-byte blocks.
constexpr size_t lfo(int i) { return 0x0a2 + 0x10 * size_t(i); }
namespace Lfo {
constexpr size_t Freq = 0x00;   // s8 0..127
constexpr size_t Reset = 0x01;  // s8 -1 (OFF) .. 63 (start phase)
constexpr size_t Human = 0x02;  // s8 0..6 (OFF, ON, 1x, 2x, 4x, 8x, 16x)
constexpr size_t Wave = 0x03;   // s8 0..82 (0 TRI 1 SAW 2 SQR 3 NOISE ...)
constexpr size_t L1 = 0x04;     // s8 0..63
constexpr size_t Delay = 0x05;  // bits 0-5 DELAY 0..63, bit 6 delay mode (0 EMU, 1 SMTH)
constexpr size_t L2 = 0x06;     // s8 0..63
constexpr size_t Mod = 0x07;    // slot (amplitude modulator, amount -63..63)
constexpr size_t FreqMod = 0x0a;// slot (amount -127..127)
constexpr size_t Modes = 0x0d;  // bits 0-1 AM mode, 2-3 FM mode (UNI BIP PHS SMT), 4-5 PLAY (FWD REV 1XF 1XR)
constexpr size_t Phase = 0x0e;  // u8 0..127 (>= 64: twin mode)
constexpr size_t Smooth = 0x0f; // s8 0..63
}  // namespace Lfo

// Modulation matrices MAT1..3: 12-byte blocks of 4 slots (M1, M2, M3, AMP).
constexpr size_t matrix(int i) { return 0x0e2 + 0x0c * size_t(i); }

// Filters (2 blocks, only filter 1 is used by the SQ8L): 14-byte blocks.
constexpr size_t filter(int i) { return 0x106 + 0x0e * size_t(i); }
namespace Filter {
constexpr size_t Freq = 0x00;   // s8 0..127
constexpr size_t Res = 0x01;    // s8 0..31
constexpr size_t Keybd = 0x02;  // s8 -63..+63 (SQ80 0..63)
constexpr size_t Mod1 = 0x04;   // slot (amount -127..127)
constexpr size_t Mod2 = 0x07;   // slot
constexpr size_t Mod3 = 0x0b;   // slot (not exposed by the GUI)
}  // namespace Filter

constexpr size_t Block122 = 0x122;  // 16 bytes, 2 slots at +2/+5, filled only by old-format import

// Final volume (DCA4).
constexpr size_t Dca4Level = 0x132;  // s8 0..63 (ENV4 amount, "FINAL")
constexpr size_t Pan = 0x133;        // s8 -63..+63
constexpr size_t AmpMod = 0x134;     // slot (amount -63..63)
constexpr size_t PanMod = 0x137;     // slot (amount -127..127)
constexpr size_t Saturation = 0x13a; // s8 -1 OFF, 0 EMU, 1..15
constexpr size_t EmuFlags2 = 0x13b;  // bits 0-1 ?, bit 2 MUFFLE
constexpr size_t FilterKeyMode = 0x13c; // s8, value-1 passed to filter setKeyParam (0 by default)

// Modes page.
constexpr size_t Sync = 0x170;     // s8 0..1
constexpr size_t AmMod = 0x171;    // s8 0..1 (AM: osc 1 modulates osc 2)
constexpr size_t Mono = 0x172;     // s8 0..1
constexpr size_t Glide = 0x173;    // s8 0..63
constexpr size_t RestartVoice = 0x174;
constexpr size_t RestartEnv = 0x175;
constexpr size_t RestartOsc = 0x176;
constexpr size_t Cycle = 0x177;    // s8 0..1 (all envelopes without sustain)

// Emulation page.
constexpr size_t BendRange = 0x190; // s8 0..36 semitones
constexpr size_t BendMode = 0x191;  // s8 0..6 (ALL HELD NEW NEWH HELD2 NEW2 NEWH2)
constexpr size_t EmuFlags = 0x192;  // bit 0 AMBUG, bits 1-2 DCA1-3 smoothing (0 EMU 1 FAST),
                                    // bits 3-4 DC-BLOCK (0 SMART 1 ON 2 OFF)
constexpr size_t VoiceSteal = 0x195;// s8 -1 HARD, 0 SOFT
constexpr size_t EmuFlags3 = 0x196; // bit 0 (voice flag), bit 1 DCA4 smoothing (0 EMU 1 HARD)
// (port) playable voices, EMU -> VOICES: 0 = the original's 8, 1..64 = explicit. One of the
// bytes the original never writes ("not in the GUI", always 0), so every program made by the
// SQ8L, every bank/library file, every pre-0.90 record and every SysEx import reads as 0 and
// keeps the original's 8 voices. See docs/modules/program.md.
constexpr size_t Polyphony = 0x197;
}  // namespace ofs

// (port) Playable voices of a program record: ofs::Polyphony, 1..64. The original's 8 for 0
// (anything the original or an import wrote), for a missing record, and for a value out of
// range, which no version of the port writes.
inline int programPolyphony(const uint8_t* program) {
    if (!program) return kOriginalPlayableVoices;
    const int n = static_cast<int>(program[ofs::Polyphony]);
    if (n < kMinPlayableVoices || n > kMaxPlayableVoices) return kOriginalPlayableVoices;
    return n;
}

// The program record. Plain bytes; all multi-byte fields are little endian and unaligned.
struct Program {
    uint8_t bytes[kProgramSize];

    int8_t s8(size_t off) const { return static_cast<int8_t>(bytes[off]); }
    uint8_t u8(size_t off) const { return bytes[off]; }
    int16_t s16(size_t off) const {
        return static_cast<int16_t>(bytes[off] | (bytes[off + 1] << 8));
    }
    void setS8(size_t off, int v) { bytes[off] = static_cast<uint8_t>(v); }
    void setU8(size_t off, unsigned v) { bytes[off] = static_cast<uint8_t>(v); }
    void setS16(size_t off, int v) {
        bytes[off] = static_cast<uint8_t>(v);
        bytes[off + 1] = static_cast<uint8_t>(static_cast<unsigned>(v) >> 8);
    }
    // Bit field (unsigned) of `width` bits at bit `shift` of byte `off`.
    unsigned bits(size_t off, int shift, int width) const {
        return (bytes[off] >> shift) & ((1u << width) - 1u);
    }
    void setBits(size_t off, int shift, int width, unsigned v) {
        const unsigned m = ((1u << width) - 1u) << shift;
        bytes[off] = static_cast<uint8_t>((bytes[off] & ~m) | ((v << shift) & m));
    }
    ModSlot mod(size_t off) const { return {s16(off), s8(off + 2)}; }
    void setMod(size_t off, ModSlot m) {
        setS16(off, m.source);
        setS8(off + 2, m.amount);
    }

    uint16_t version() const { return static_cast<uint16_t>(s16(ofs::Version)); }

    // Name (ShortString at +2, max 15 chars). setName copies like Delphi's ShortString
    // assignment: only length + chars are written, older trailing bytes stay.
    std::string name() const { return shortString(ofs::Name); }
    void setName(std::string_view s) { setShortString(ofs::Name, s); }
    std::string name2() const { return shortString(ofs::Name2); }
    void setName2(std::string_view s) { setShortString(ofs::Name2, s); }

    std::string shortString(size_t off) const;          // full length byte (up to the record end)
    void setShortString(size_t off, std::string_view s); // FUN_0040291c(dst, s, 15)
};
static_assert(sizeof(Program) == kProgramSize, "Program must be the raw 540-byte record");

// INIT program (original FUN_004539c0 = FUN_00453814 + FUN_004537c0): what the INIT button,
// library init and all imports start from.
void initProgram(Program& p);
Program makeInitProgram();

// Clear `count` modulation slots (3 bytes each) to source -1 / amount 0xff in the first two
// bytes, as FUN_0045377c does (the amount byte is left untouched).
void clearModSlots(uint8_t* p, int count);

// Conversion of a pre-0.90 program (0x220 bytes, version word 1 at +0x200) into the current
// layout (original FUN_00453c64).
constexpr size_t kOldProgramSize = 0x220;
void convertOldProgram(const uint8_t* src, Program& dst);

}  // namespace sq8l
