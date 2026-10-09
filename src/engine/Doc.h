// Ensoniq DOC 5503 oscillator engine of the SQ-80 (original unit mod_doc8_5, class Cdoc).
//
// One Doc object serves all voice slots (the original creates it with 16). Per voice it runs
// three wavetable oscillators on the 8-bit SQ-80 wave ROM at the DOC's own sample rate
// (38455.86 Hz), mixes them (with the SQ-80 sync / AM modes and the "AM bug"), and resamples
// the mix to the host rate with a polynomial interpolator, optionally followed by a 15 Hz
// DC-blocking high-pass.
//
// plugCore drives it through the per-voice parameter block (DocVoiceParams, obtained with
// params()), which it fills at control rate before calling update(); see docs/modules/doc.md.
//
// All structs mirror the original object byte for byte (offsets in the comments, checked by
// static_asserts in Doc.cpp) so differential tests can load/save the original's state.
#pragma once

#include <cstdint>

#include "VoiceSlots.h"

namespace sq8l {

// Oscillator parameters written by plugCore (0x18 bytes).
struct DocOscParams {
    int32_t enabled;   // +0x00  -1 = on, 0 = off
    int32_t wave;      // +0x04  0..74 (>= 75: silent "no wave"); >= 54 are one-shot waves
    int32_t level;     // +0x08  DCA level 0..0x7f00: level >> 7 indexes kDocLevelTable, the low
                       //        7 bits interpolate
    int32_t semitone;  // +0x0c  octave * 12 + semitone
    int32_t fine;      // +0x10  fine tune (1/32 semitone steps)
    int32_t pitchMod;  // +0x14  pitch modulation in 1/256 semitone (added after the lookup)
};

// Per-voice parameter block written by plugCore (doc+0x20a0+v*0x80, 0x80 bytes).
struct DocVoiceParams {
    DocOscParams osc[3];     // +0x00, +0x18, +0x30
    int32_t init;            // +0x48  set by startVoice: next update takes levels without smoothing
    int32_t resetPhase;      // +0x4c  next update resets the oscillator phases (restart)
    int32_t newNote;         // +0x50  next update (re)configures the DC blocker
    int32_t forceRecalc;     // +0x54  next update recomputes all wavesample/pitch settings
    int32_t linkedSlot;      // +0x58  voice whose oscillator phases the next init update copies.
                             //        Original: pointer to that voice's state (0 = none);
                             //        here voice index + 1 (0 = none)
    int32_t waveKey;         // +0x5c  key used for the wavesample (multisample) selection
    int32_t pitchKey;        // +0x60  key used for the pitch (glide target)
    int32_t amMode;          // +0x64  1 = AM: osc 0 (silent) modulates the amplitude of osc 1
    int32_t sync;            // +0x68  != 0: osc 1 hard-synced to osc 0 (panel: OSC2 to OSC1)
    int32_t amBug;           // +0x6c  != 0: SQ-80 AM bug (in AM mode osc 1 takes wave ROM bank
                             //        bit 0 from osc 0's wavetable register)
    int32_t flag70;          // +0x70  program byte 0x174 bit 0; only copied to DocVoice::flag158
    int32_t reserved74;      // +0x74  never touched by this unit
    int32_t levelSmoothing;  // +0x78  DCA1-3 smoothing: 1 = EMU, 2 = FAST, other = none
    int32_t dcBlock;         // +0x7c  > 0: DC blocker on (taken on newNote)
};

// Oscillator state (0x70 bytes at voice + i*0x70).
struct DocOsc {
    int32_t enabled;      // +0x00  copy of DocOscParams::enabled (0 = muted in the mix)
    int32_t valid;        // +0x04  -1 once the cached wave/semitone/fine below are valid
    int32_t wave;         // +0x08  cached wave
    int32_t semitone;     // +0x0c  cached semitone
    int32_t fine;         // +0x10  cached fine
    int32_t pitch;        // +0x14  pitch without modulation (1/256 semitone + wavesample fine)
    int32_t modPitch;     // +0x18  pitch + pitchMod, clamped >= 0
    int32_t pitchMod;     // +0x1c  last pitchMod
    int32_t level;        // +0x20  last level parameter
    int32_t halted;       // +0x24  DOC halt bit (0 = running)
    int32_t reserved28;   // +0x28
    float ampTarget;      // +0x2c  smoothed amplitude target, premultiplied by (1 - pole)
    float amp;            // +0x30  current (smoothed) amplitude
    float levelCur;       // +0x34  amplitude for the current level (EMU/FAST smoothing)
    float levelPrev;      // +0x38  previous one
    uint32_t bank;        // +0x3c  wave ROM bank (0..3)
    uint32_t page;        // +0x40  wave ROM page (address bits 15..8)
    uint32_t address;     // +0x44  bank << 16 | page << 8
    int32_t sizeLog2;     // +0x48  table length log2 (8..15)
    int32_t tableSize;    // +0x4c  1 << sizeLog2 (unused by the render)
    int32_t resolution;   // +0x50  DOC resolution (0..7)
    uint32_t accMask;     // +0x54  (1 << (resolution + 17)) - 1
    int32_t shift;        // +0x58  resolution + 17 - sizeLog2: accumulator -> table index
    uint32_t acc;         // +0x5c  phase accumulator
    uint32_t freq;        // +0x60  frequency register (16 bit)
    int32_t wrapped;      // +0x64  accumulator wrapped on the previous step
    int32_t mode;         // +0x68  0 = free run, 1 = one-shot (halt after wrap), 2 = sync slave
    int32_t amBug;        // +0x6c  -1 when the AM bug selected this oscillator's bank
};

// Voice state (0x200 bytes at doc+8+v*0x200).
struct DocVoice {
    DocOsc osc[3];              // +0x000, +0x070, +0x0e0
    int32_t amMode;             // +0x150  copy of DocVoiceParams::amMode
    int32_t flag154;            // +0x154  cleared on resetPhase (not read by this unit)
    int32_t flag158;            // +0x158  DocVoiceParams::flag70 != 0 (not read by this unit)
    int32_t ditherIndex;        // +0x15c  alternates the +-1e-10 anti-denormal offset
    int32_t interpOrder;        // +0x160  resampler polynomial order 0..4
    uint32_t phase;             // +0x164  resampler phase (30-bit fraction of a DOC sample)
    uint32_t prevPhase;         // +0x168
    float hist[4];              // +0x16c  last 4 DOC output samples (oldest first)
    int32_t reserved17c;        // +0x17c
    float coef[4];              // +0x180  interpolation polynomial coefficients
    int32_t dcBlock;            // +0x190  DC blocker on
    float dcX1, dcX2;           // +0x194, +0x198  DC blocker input history
    float dcY1, dcY2;           // +0x19c, +0x1a0  DC blocker output history
    int32_t waveKey;            // +0x1a4  key of the current wavesample selection
    int32_t pitchKey;           // +0x1a8  key of the current pitch
    int32_t smoothMode;         // +0x1ac  DCA1-3 smoothing mode in effect (2 after reset)
    int32_t reserved1b0[20];    // +0x1b0..+0x1ff
};

// (port) Per-voice resampler clock and the DCA/AM smoothing poles at that clock, used by
// MTS-ESP retuning (see Tuning.h and docs/modules/tuning.md). Scaling a voice's phaseInc
// shifts its oscillator pitch without touching the frequency register, so the wave ROM index
// sequence and the three oscillators' relative detune stay exactly as the original computes
// them; the poles follow the clock so the 2 ms / 0.2 ms time constants do not stretch with it.
//
// Deliberately NOT a DocVoice field: tests/capi_doc.cpp round-trips the voice blocks with a
// whole-struct memcpy of 0x200 bytes, so anything kept inside DocVoice would be overwritten
// from the original's image on load and written back into the compared image on save.
struct VoiceClock {
    uint32_t phaseInc = 0;        // this voice's resampler increment
    float smoothPole = 0;         // DCA pole at this voice's clock
    float smoothGain = 0;         // 1 - smoothPole
    float amSmoothPole = 0;       // AM pole at this voice's clock
    float amSmoothGain = 0;       // 1 - amSmoothPole
    int32_t quantizedOffset = 0;  // residual the poles were computed for, 1/64 semitone units
    bool valid = false;
};

class Doc {
public:
    static constexpr uint32_t kMaxVoices = kMaxVoiceSlots;  // array size (the original: 16, see VoiceSlots.h)
    static constexpr float kDocRate = 38455.85546875f;  // 0x471637db

    // Constructor (FUN_0045b9e0; run with the host FPU mode: round to nearest). The original
    // also decompresses the wave ROM into a heap block (+0x2008); we use data::kWaveRom.
    // `capacity`: slots in use, the original's 16 unless more polyphony is wanted (the
    // engine passes kMaxVoiceSlots); numVoices and the voice links are limited to it.
    explicit Doc(uint32_t numVoices = kOriginalVoiceSlots, uint32_t capacity = kOriginalVoiceSlots);

    void setNumVoices(uint32_t n);          // FUN_0045bc20 (clamped to 16)
    void setSampleRate(float sr);           // FUN_0045bc4c
    void resetAll();                        // FUN_0045bc18 / FUN_0045bc00: resetVoice(0..15)
    void resetVoice(uint32_t v);            // FUN_0045bb54 (ignores numVoices, only v < capacity)

    // Voice start (FUN_0045bcd0; Delphi argument order: voice, key, resetPhase, newNote,
    // linkedVoice): key for wave and pitch, linked voice (phase source for the first update,
    // outside 0..15 = none), newNote -> DocVoiceParams::newNote, resetPhase -> ::resetPhase.
    void startVoice(uint32_t v, int32_t key, int32_t linkedVoice, int32_t newNote, int32_t resetPhase);
    // FUN_0045c460: set the pitch key (glide start) and update() immediately.
    void setPitchKey(uint32_t v, int32_t key) { setKeys(v, key, false); }
    // FUN_0045bd44: set the pitch key (and, if waveKeyToo, the wave key with a forced
    // recalculation), then update().
    void setKeys(uint32_t v, int32_t key, bool waveKeyToo);
    void stopVoice(uint32_t v);             // FUN_0045bd78: halt all oscillators
    void update(uint32_t v);                // FUN_0045bda4: apply DocVoiceParams (control rate)
    void interpolateLevels(uint32_t v);     // FUN_0045c378: half-step level smoothing (control rate)
    DocVoiceParams* params(uint32_t v);     // FUN_0045c7e0 (nullptr if v >= numVoices)

    // One output sample of voice v (FUN_0045c7f4): in the original it is left in ST0 for the
    // filter. Run inside RoundTowardZero like all of processReplacing.
    double render(uint32_t v);

    // ---- state, laid out as in the original object (offsets from the object start) ----
    // +0x0000 vmt
    uint32_t numVoices = 0;                 // +0x0004
    uint32_t capacity = kOriginalVoiceSlots;  // (port, not in the original object)
    DocVoice voice[kMaxVoices] = {};        // +0x0008
    // +0x2008 pointer to the decompressed wave ROM (here: data::kWaveRom)
    float dither[2] = {};                   // +0x200c  +1e-10, -1e-10
    float phaseScale = 0;                   // +0x2014  2^-30
    float jumpThreshold = 0;                // +0x2018  0.001
    float amScale = 0;                      // +0x201c  0.004724 (AM: osc1 sample -> amplitude)
    float amOffset = 0;                     // +0x2020  0.4
    float sampleScale = 0;                  // +0x2024  1/127
    float k0_25 = 0, k0_5 = 0, k0_75 = 0;   // +0x2028, +0x202c, +0x2030
    float k1_5 = 0, k1_75 = 0, k2 = 0;      // +0x2034, +0x2038, +0x203c
    float k2_5 = 0, k3 = 0, k4 = 0;         // +0x2040, +0x2044, +0x2048
    float k1_6 = 0, k1_9 = 0, k1_18 = 0;    // +0x204c, +0x2050, +0x2054  1/6, 1/9, 1/18
    float k5_3 = 0, k11_36 = 0;             // +0x2058, +0x205c  5/3, 11/36
    float k11_6 = 0, k85_36 = 0;            // +0x2060, +0x2064  11/6, 85/36
    uint32_t phaseInc = 0;                  // +0x2068  Trunc(docRate / sr * 2^30)
    float dcB0 = 0, dcB1 = 0, dcB2 = 0;     // +0x206c..+0x2074  DC blocker (RBJ high-pass 15 Hz)
    float dcA1 = 0, dcA2 = 0;               // +0x2078, +0x207c
    float smoothPole = 0;                   // +0x2080  DCA smoothing pole (per DOC sample)
    float amSmoothPole = 0;                 // +0x2084  pole of the AM amplitude path
    float smoothGain = 0;                   // +0x2088  1 - smoothPole
    float amSmoothGain = 0;                 // +0x208c  1 - amSmoothPole
    float docRate = 0;                      // +0x2090
    float sampleRate = 0;                   // +0x2094
    float invSampleRate = 0;                // +0x2098
    int32_t releaseSamples = 0;             // +0x209c  Round(0.07 * sr); read by plugCore (FUN_00464410)
    DocVoiceParams param[kMaxVoices] = {};  // +0x20a0..+0x289f (object size 10400)

    // (port) per-voice clock, outside the original's object image: see VoiceClock.
    VoiceClock clock[kMaxVoices] = {};

    // ---- internal helpers (public for the differential tests) ----
    // DCA level (0..0x7f00) -> amplitude, interpolating kDocLevelTable (FUN_0045c568).
    float levelToAmp(int32_t level) const;
    // Wavesample lookup + octave normalisation + pitch (FUN_0045c49c with FUN_00455f0c and
    // FUN_0045c470): outputs the pitch in 1/256 semitone, the DOC wavetable register (with
    // the resolution adjusted by whole octaves) and the ROM page.
    void computePitch(int32_t waveKey, int32_t pitchKey, int32_t* pitch, uint8_t* waveReg,
                      uint8_t* page, int32_t wave, int32_t fine, int32_t semitone) const;

    // (port) The voice's base pitch in 1/256 semitone at semitone 0 / fine 0: the anchor the
    // MTS-ESP retune is measured against.
    int32_t basePitch(int32_t key, int32_t wave) const;
    // (port) Scale voice v's resampler clock to `ratio` times nominal and recompute its
    // smoothing poles for that clock. ratio == 1.0 copies the nominal values verbatim, so the
    // untuned path cannot drift by a float LSB.
    void setVoiceClock(uint32_t v, double ratio);
    // (port) Every voice back to the nominal clock. Anything that writes `phaseInc` or the
    // smoothing poles directly must call this afterwards, or `render` keeps using the old
    // values: the per-voice fields mirror them. setSampleRate and setDocRate do; so does the
    // differential tests' state load (tests/capi_doc.cpp).
    void resetVoiceClocks();

private:
    void initConstants();                   // FUN_0045baac
    void setDocRate(float rate);            // FUN_0045bc34
    void computeSmoothing();                // FUN_0045c628
    void computeDcBlocker();                // FUN_0045c6dc
    void setAmpTarget(uint32_t v, DocVoice& vc, int osc, float level);  // FUN_0045c5ec
    int32_t stepOscillator(DocVoice& vc, int osc);          // one DOC step (inlined in FUN_0045c7f4)
};

// DOC frequency register for a pitch in 1/256 semitone (FUN_0045b8b4, unit docPitch),
// interpolating kDocPitchTable at 1/32 semitone resolution.
uint32_t docPitchToFreq(int32_t pitch);

namespace data {
extern const uint16_t kDocPitchTable[2049];  // 0x4c0278: DOC frequency per 1/16 semitone
extern const float kDocLevelTable[256];      // 0x4c127c: DCA level -> amplitude
}  // namespace data

}  // namespace sq8l
