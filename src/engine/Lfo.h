// SQ-80 style LFO with SQ8L extensions (original unit mod_lfo4_22, class Csq_lfo,
// instance size 0xf0). See docs/modules/lfo.md.
//
// The LFO runs at the control rate (one tick() per voice control update). It is
// almost entirely integer code; floating point is only used to convert the
// frequency to a phase increment and to rescale the delay rate (both depend on
// the control rate).
#pragma once

#include <cstdint>

#include "WaveRom.h"

namespace sq8l {

namespace data {
extern const uint8_t kLfoWaveParams[280];   // 70 x {osc wave, key offset, freq shift, 0}
extern const int8_t kLfoShapeTables[2048];  // 8 x 256: EXP, EXP2..4, TAN, TAN2..4
extern const uint8_t kLfoLevelShift[2];     // per delay mode: {1, 4}
extern const uint8_t kLfoAmShift[2];        // per delay mode: {0, 3}
extern const int8_t kLfoNoise[256];         // NOISE wave, index = signed byte + 128
extern const int8_t kLfoHumanize[256];      // random steps -1/0/+1, index = signed byte + 128
}  // namespace data

// LFO waveform numbers (input field waveIn, +0xb8).
enum LfoWave : int32_t {
    kLfoTri = 0,
    kLfoSaw = 1,
    kLfoSquare = 2,
    kLfoNoise = 3,
    kLfoBipolar = 4,
    kLfoOscWaveFirst = 5,    // 5..74 = oscillator waves 00..69 (wave ROM)
    kLfoShapeFirst = 0x4b,   // 75..82 = EXP, EXP2, EXP3, EXP4, TAN, TAN2, TAN3, TAN4
};

class Lfo {
public:
    // Callback returning the ROM location of an oscillator wave for an LFO
    // (original: method pointer at +0xa8/+0xac = LAB_0045c4f8 bound to the Cdoc).
    using WaveSource = WaveLocation (*)(int32_t key, uint32_t wave);

    // ---------------------------------------------------------------- state
    // Comments give the field offset in the original object (for differential tests).
    int32_t output = 0;          // +0x04  last output value (returned while stopped)
    int32_t running = 0;         // +0x08  -1 = running, 0 = stopped (one-shot end / never triggered)
    int32_t smoothCur = 0;       // +0x0c  smoothing amount in use (0 = off)
    int32_t smoothState = 0;     // +0x10  one-pole smoother state (wave value * 256)
    int32_t smoothA = 0;         // +0x14  smoother feedback weight (/256)
    int32_t smoothB = 0;         // +0x18  smoother input weight (256 - smoothA)
    uint32_t phase = 0;          // +0x1c  phase, one cycle = 2^30
    int32_t phaseInc = 0;        // +0x20  phase increment per tick (signed)
    int32_t phaseOffset[2] = {}; // +0x24, +0x28  phase offset of wave 1 / twin wave 2
    int32_t freqShift = 0;       // +0x2c  increment >> freqShift for long ROM waves
    int32_t twin = 0;            // +0x30  1 = twin mode (output = wave1 - wave2)
    int32_t freqSeen = 0;        // +0x34  copy of freqIn (never read; 0x80000000 after trigger)
    int32_t humanOffset = 0;     // +0x38  humanization frequency offset (8.8 like freqIn)
    int32_t reverse = 0;         // +0x3c  copy of reverseIn
    int32_t oneShot = 0;         // +0x40  copy of oneShotIn
    int32_t level1Seen = 0;      // +0x44  level1In / level2In / delayIn at the last level setup
    int32_t level2Seen = 0;      // +0x48
    int32_t delaySeen = 0;       // +0x4c
    int32_t delayMode = 0;       // +0x50  0 = EMU, 1 = SMTH
    int32_t level = 0;           // +0x54  current amplitude, << levelShift
    int32_t levelTarget = 0;     // +0x58  final amplitude (level2In << levelShift)
    int32_t levelStep = 0;       // +0x5c  amplitude change per (EMU or SMTH) step, 0 = done
    int32_t ticks = 0;           // +0x60  delay steps since trigger (saturating)
    int32_t am = 0;              // +0x64  copy of amIn
    int32_t wave = 0;            // +0x68  waveform in use (0 forces TRI on invalid waves)
    const uint8_t* waveData = nullptr;  // +0x6c  ROM wave or shaping table in use
    uint32_t waveMask = 0;       // +0x70  ROM wave length - 1
    int32_t waveShift = 0;       // +0x74  30 - log2(ROM wave length)
    int32_t human = 0;           // +0x78  humanization mode in use (copy of humanIn)
    int32_t humanIndex = 0;      // +0x7c  position in the humanization table
    int32_t humanStep = 0;       // +0x80  humanization step per EMU step (random * 256)
    int32_t humanCount = 0;      // +0x84  remaining EMU steps of the current humanization step
    int32_t emuClock = 0;        // +0x88  EMU step clock (counts down by 1024 per tick)
    int32_t emuPeriod = 0x1c80;  // +0x8c  EMU step period (7296 = 7.125 ticks at 83.59 Hz)
    uint8_t levelShift = 0;      // +0x90  kLfoLevelShift[delayMode]
    uint8_t amShift = 0;         // +0x91  kLfoAmShift[delayMode]
    float controlRate = 0;       // +0x94  control rate in Hz
    float rateRatio = 0;         // +0x98  83.59257598 / controlRate
    float rateRatioLow = 0;      // +0x9c  rateRatioHigh / 7 (FREQ < 7)
    float rateRatioHigh = 0;     // +0xa0  rateRatio (FREQ >= 7)
    uint8_t defaultRate = 0;     // +0xa4  1 until setControlRate (disables delay rate scaling)
    WaveSource waveSource = nullptr;  // +0xa8 code, +0xac Cdoc (null = Delphi Assigned() false)

    // ---------------------------------------------------------------- inputs
    // Written by plugCore before trigger() and before every tick().
    int32_t freqIn = 0;          // +0xb0  FREQ << 8 plus frequency modulation (8.8 fixed point)
    int32_t humanIn = 0;         // +0xb4  HUMAN: 0 OFF, 1 ON (SQ-80), 2 1x, 3 2x, 4 4x, 5 8x, 6 16x
    int32_t waveIn = 0;          // +0xb8  WAVE 0..82 (see LfoWave)
    int32_t level1In = 0;        // +0xbc  L1 (start amplitude) 0..63
    int32_t level2In = 0;        // +0xc0  L2 (final amplitude) 0..63
    int32_t delayIn = 0;         // +0xc4  DELAY (fade speed) 0..63
    int32_t phaseIn = 0;         // +0xc8  phase offset (PHS * 1024 + phase modulation), << 14
    int32_t twinPhaseIn = 0;     // +0xcc  twin wave phase offset (same scale)
    int32_t amIn = 0;            // +0xd0  amplitude modulation, added to the level
    int32_t delayModeIn = 0;     // +0xd4  MODE-DELAY: 0 EMU, 1 SMTH (low byte used)
    int32_t reverseIn = 0;       // +0xd8  1 = reversed playback (REV, 1XR)
    int32_t oneShotIn = 0;       // +0xdc  1 = one-shot playback (1XF, 1XR)
    int32_t twinIn = 0;          // +0xe0  1 = twin mode
    int32_t smoothIn = 0;        // +0xe4  smoothing 0..255 (SMTH << 2 or modulated)
    int32_t waveKeyIn = 0;       // +0xe8  added to the wave table key offset (always 0 in the original)
    int32_t freqShiftIn = 0;     // +0xec  added to the wave table frequency shift (always 0)

    // ---------------------------------------------------------------- API
    Lfo() = default;
    // Constructor FUN_0045cf9c(rate). The original zero-fills the object first.
    explicit Lfo(float rate) { construct(rate); }
    void construct(float rate);

    // FUN_0045d020: set the control rate (also resets the playback state).
    void setControlRate(float rate);

    // FUN_0045d2a4: voice start. phaseSteps (RESET value, 64 steps per cycle) is
    // added to the phase, after clearing it if resetPhase.
    void trigger(int32_t phaseSteps, bool resetPhase);

    // FUN_0045db2c: one control-rate tick: apply the inputs, then produce the
    // next output value (roughly -64..63 for full amplitude).
    int32_t tick() {
        updateParams();
        return step();
    }

    // ---------------------------------------------------------------- internals
    // (public for the differential tests)
    void resetState();                         // FUN_0045d1a8
    void setDelayMode(uint8_t mode);           // FUN_0045d218
    void setSmoothing(int32_t amount);         // FUN_0045d25c
    int32_t freqToIncrement(int32_t freq) const;  // FUN_0045d110
    void updateParams();                       // FUN_0045d3e0
    int32_t step();                            // FUN_0045d824
};

}  // namespace sq8l
