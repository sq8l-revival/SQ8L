// SQ-80 style 4-stage envelope with the SQ8L extras (original unit mod_env_2, class Csq_env,
// 128 bytes; 4 per voice slot). See docs/modules/env.md.
//
// The envelope is integer arithmetic (8.8 fixed point levels) run at control rate
// (83.592575 Hz); only the rate setup touches the FPU.
#pragma once

#include <cstdint>

namespace sq8l {

namespace data {
// Static tables of the original (src/engine/data/EnvData.cpp, re/scripts/extract_env_data.py).
extern const uint16_t kEnvTimeTable[64];         // 0x4c2090: time 0..63 -> control ticks
extern const int8_t kEnvShapeTables[8 * 256];    // 0x4c1688: EXP..EXP4, TAN..TAN4 curves
extern const uint8_t kVelocityCurveLinear[128];  // 0x494d4c: LV 0..63 ("L")
extern const uint8_t kVelocityCurveExp[128];     // 0x494dcc: LV 64..127 ("X")
}  // namespace data

// Layout of the 14-byte envelope record in a patch (patch + 0x6a + 14 * envIndex),
// as read by Env::start (FUN_0045dd2c).
struct EnvRecord {
    enum : int {
        kL0 = 0,     // int8 -63..63 start level
        kL1 = 1,     // int8 -63..63
        kL2 = 2,     // int8 -63..63
        kL3 = 3,     // int8 -63..63 (sustain)
        kL4 = 4,     // int8, copied to level[4] but never used by the envelope
        kLV = 5,     // 0..63 linear ("L"), 64..127 exponential ("X") velocity level scaling
        kT1V = 6,    // 0..63 velocity -> attack time (or smoothing, see kFlags bit 4)
        kT1 = 7,     // & 0x3f
        kT2 = 8,     // & 0x3f
        kT3 = 9,     // & 0x3f
        kT4 = 10,    // & 0x3f; bit 6 = "R" second release
        kTK = 11,    // int8 0..63 keyboard scaling of T2/T3
        kFlags = 12, // bit 0 CYC (read by plugCore), bits 1..3 SHAPE (0=OFF), bit 4 MODE-T1V=SMT
        kSMTH = 13,  // 0..63 smoothing
        kSize = 14,
    };
};

class Env {
public:
    // Per-tick ramp routine: the Delphi method pointer at +0x68 (code) / +0x6c (data = self).
    // None = nil (zero-filled object before the constructor's reset; skipped by Assigned()).
    enum class Ramp : uint8_t {
        None,  // 0
        Idle,  // 0x45e0bc (empty procedure)
        Rise,  // 0x45dfd4
        Fall,  // 0x45e030
        Hold,  // 0x45e08c
    };
    // Routine called when a segment's tick count runs out: method pointer at +0x70 / +0x74.
    enum class Next : uint8_t {
        None,           // 0 (nil)
        Decay,          // 0x45e0c0: end of attack -> segment to L2 in T2
        Decay2,         // 0x45e0f8: -> segment to L3 in T3
        Sustain,        // 0x45e130: end of T3 -> sustain, release (CYC / released) or finish
        SecondRelease,  // 0x45e160: end of the "R" half release -> to 0 in time 39
        Finish,         // 0x45e17c: end of release
    };

    // Comments give the field offset in the original object (for differential tests).
    int32_t level[5] = {};       // +0x04..+0x14  L0 (start), L1, L2, L3, L4(unused); 2*x or 2*(x-1)
    int32_t time[4] = {};        // +0x18..+0x24  T1..T4 (0..63)
    int32_t levelScale = 0;      // +0x28  velocity level scaling, 256 = unity (may be <= 0 for LV>=64)
    int32_t timeKeyScale = 0;    // +0x2c  TK: subtracted from T2 and T3
    uint8_t secondRelease = 0;   // +0x30  T4 "R" flag
    uint8_t cycle = 0;           // +0x31  CYC: no sustain stage, release ignored
    // +0x32, +0x33: padding
    int32_t value = 0;           // +0x34  current level, 8.8 fixed point
    int32_t target = 0;          // +0x38  current segment target level
    int32_t step = 0;            // +0x3c  per-tick increment magnitude (8.8)
    int32_t ticksLeft = 0;       // +0x40  ticks left in the current segment
    int32_t output = 0;          // +0x44  last (unshaped) output
    int32_t smoothing = 0;       // +0x48  smoothing coefficient, 0 = off
    int32_t smoothed = 0;        // +0x4c  smoothed level, 8.8 fixed point
    int32_t smoothA = 0;         // +0x50  = smoothing
    int32_t smoothB = 0;         // +0x54  = 256 - smoothing
    int32_t shape = 0;           // +0x58  SHAPE - 1: -1 = OFF, 0..6
    const int8_t* shapeTable = nullptr;  // +0x5c  data::kEnvShapeTables + 256 * shape, or null
    uint8_t active = 0;          // +0x60  running (env 4: voice alive)
    uint8_t sustaining = 0;      // +0x61  in sustain stage
    uint8_t released = 0;        // +0x62  key released
    uint8_t releasePending = 0;  // +0x63  key released while the hold pedal was down
    uint8_t finished = 0;        // +0x64  release done, deactivates at next tick
    // +0x65..+0x67: padding
    Ramp ramp = Ramp::None;      // +0x68 / +0x6c
    Next next = Next::None;      // +0x70 / +0x74
    float rate = 0;              // +0x78  control rate (Hz)
    float rateScale = 0;         // +0x7c  rate / 83.59257598 (1.0 at the standard rate)

    // Constructor (FUN_0045dbcc): zero-filled object, then setRate(rate).
    void init(float controlRate);
    // FUN_0045dcb8: set the control rate and reset() (master ctor, FUN_00462380).
    void setRate(float controlRate);
    // FUN_0045dc3c: clear all envelope state (not cycle, smoothA/B, shape, shapeTable, rate).
    void reset();
    // Note on (FUN_0045dd2c, called by FUN_004625d4).
    //   rec: 14-byte EnvRecord; key: 0..127; velocity: raw, clamped to 0..127 here;
    //   restart: start at L0 (else continue from prev, if any; prev may be this);
    //   cycle: byte stored at +0x31.
    void start(const uint8_t* rec, int32_t key, int32_t velocity, uint8_t restart, uint8_t cycle,
               const Env* prev);
    // Note off (FUN_0045dfb4): pedalHeld (master+0x11420) defers the release.
    void release(bool pedalHeld);
    // Control-rate tick (FUN_0045e210): returns the (shaped) output level.
    int32_t tick(bool pedalHeld);
    // Output shaper (FUN_0045e1d4): shaped output, or `output` when SHAPE is OFF. Pure.
    int32_t shaped() const;

private:
    void setSmoothing(int32_t amount);              // FUN_0045dce4
    void startSegment(int32_t tgt, int32_t timeIndex);  // FUN_0045df18
    void runRamp();                                 // call [+0x68]
    void runNext();                                 // call [+0x70]
    void rampEnd();                                 // shared tail of the ramp routines
    void beginRelease();                            // FUN_0045e190
    void finish();                                  // FUN_0045e17c
};

}  // namespace sq8l
