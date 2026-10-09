// Interface between the master (CplugMaster) and the per-voice DSP modules.
//
// The original master owns, per voice slot v (0..15): 4 LFOs (Csq_lfo), 4 envelopes
// (Csq_env), 2 filters (CfilterSQ), 1 amplifier (Camp) and 1 mod follower used for
// glide (Cmod_foll); plus one oscillator engine (Cdoc) shared by all voices and
// indexed by slot. The filters are owned by sq8l::Master directly (FilterSQ); every
// other module is reached through this interface, one method per entry point the
// master calls (original address in the comment), so that the master can be
// verified independently of the module ports. Names follow docs/modules/*.md.
//
// Conventions:
//  * `voice` is the voice slot 0..15 (voice record master+4+voice*0xe8; module objects
//    of that record; for Cdoc the master passes voice+0x10, always equal to the slot).
//  * Arguments are listed in Delphi declaration order (= push order; Ghidra shows the
//    stack arguments reversed). Integers are the exact 32-bit register/stack values,
//    booleans the low byte of the original argument.
//  * Floating point arguments are Delphi Single (pushed as dword) -> float; values
//    passed/returned on the x87 stack (53-bit precision) -> double.
//  * Methods are called under the caller's rounding mode: inside Master::process (and
//    MIDI dispatched from it) round-toward-zero, otherwise the host's mode.
//  * Fields the master reads or writes directly inside module objects are exposed as
//    accessors (envActive, ampRampCount, docReleaseSamples) or as references to
//    layout-faithful parameter blocks (lfoParams, docVoiceParams). The references must
//    alias the storage the module uses, or the implementation copies them in before
//    its own calls (and back out if the module modifies them).
#pragma once

#include <cstdint>

#include "Tuning.h"  // (port) MTS-ESP retuning, see docRetune below

namespace sq8l {

// Input block of Csq_lfo at +0xb0..+0xe7 (14 dwords), written by the master from the
// program's LFO record `blk` (16 bytes at program+0xa2+i*0x10) before lfoStart (voice
// start: the fields marked S) and before every lfoTick (control update: all fields
// except levelAlt, which is written only when blk[0xe] bit 6 is set).
struct LfoParams {
    int32_t freq;        // +0xb0 S (blk[0] & 0x7f) << 8; control update: + FM modulation
    int32_t p04;         // +0xb4 S (int8)blk[2]
    int32_t p08;         // +0xb8 S (int8)blk[3]
    int32_t p0c;         // +0xbc S (int8)blk[4]
    int32_t p10;         // +0xc0 S (int8)blk[6]
    int32_t wave;        // +0xc4 S blk[5] & 0x3f
    int32_t level;       // +0xc8   (blk[0xe] & 0x3f) << 10 + modulation, 0 if blk[0xe] bit 6
    int32_t levelAlt;    // +0xcc   same value when blk[0xe] bit 6 is set
    int32_t depthMod;    // +0xd0   modulation of the depth
    int32_t p24;         // +0xd4 S bit 6 of blk[5]
    int32_t p28;         // +0xd8 S bit 4 of blk[0xd]
    int32_t p2c;         // +0xdc S bit 5 of blk[0xd]
    int32_t levelSel;    // +0xe0   blk[0xe] bit 6 (0/1)
    int32_t p34;         // +0xe4 S (blk[0xf] & 0x3f) << 2; control update: possibly modulated
};
static_assert(sizeof(LfoParams) == 0x38, "LfoParams layout");

// One oscillator inside the Cdoc voice parameter block.
struct MasterDocOsc {
    int32_t enabled;     // +0x00  -1 when the oscillator's DCA is on (program osc byte 0xf > 0)
    int32_t wave;        // +0x04  (int16) osc bytes 3..4
    int32_t level;       // +0x08  DCA level << 9 + DCA modulation * 4
    int32_t pitch;       // +0x0c  octave * 12 + semitone
    int32_t fine;        // +0x10  fine tune
    int32_t pitchMod;    // +0x14  glide + pitch bend + pitch modulation
};
static_assert(sizeof(MasterDocOsc) == 0x18, "MasterDocOsc layout");

// Cdoc voice parameter block (doc+0x20a0+voice*0x80, returned by FUN_0045c7e0).
// The master writes osc[], f64..f70, dcaSmoothing and dcBlock, then calls docUpdate;
// the other words are owned by Cdoc and never touched by the master.
struct MasterDocParams {
    MasterDocOsc osc[3];      // +0x00
    int32_t docOwned48[7];    // +0x48..+0x60 (Cdoc: docStartVoice / docSetPitchKey)
    int32_t f64;              // +0x64  (int8)program[0x171]
    int32_t f68;              // +0x68  program[0x170] > 0 ? -1 : 0
    int32_t f6c;              // +0x6c  program[0x192] bit 0 ? -1 : 0
    int32_t f70;              // +0x70  program[0x196] bit 0 ? -1 : 0
    int32_t docOwned74;       // +0x74  (Cdoc)
    int32_t dcaSmoothing;     // +0x78  DCA1-3 smoothing 1..4: override +0xfec or ((program[0x192] >> 1) & 3) + 1
    int32_t dcBlock;          // +0x7c  DC blocking 0/1 (override +0xff4 or program[0x192] bits 3-4)
};
static_assert(sizeof(MasterDocParams) == 0x80, "MasterDocParams layout");

class VoiceModules {
public:
    virtual ~VoiceModules() = default;

    // ------------------------------------------------------------ Cdoc (oscillators)
    virtual void docReset() = 0;                                    // FUN_0045bc18 (panic/reset)
    virtual void docSetNumVoices(int32_t n) = 0;                    // FUN_0045bc20
    virtual void docSetSampleRate(float sampleRate) = 0;            // FUN_0045bc4c
    // FUN_0045bcd0(doc, voice, key, resetPhase, newNote, linkedVoice): resetPhase and
    // newNote are -1/0 (block +0x4c / +0x50); linkedVoice is the slot the note took
    // over (block +0x58 -> that voice's oscillators; -1 = none).
    virtual void docStartVoice(int voice, int32_t key, int32_t resetPhase, int32_t newNote,
                               int32_t linkedVoice) = 0;
    virtual void docSetPitchKey(int voice, int32_t key) = 0;        // FUN_0045c460(doc, voice, key, 0)
    // (port) MTS-ESP, no counterpart in the original: set the voice's resampler clock for the
    // residual retune and return the whole-semitone part in 1/256 semitone units for the
    // caller to add to its pitch. 0 and a nominal clock when no retune applies. `wave` is
    // oscillator 0's wave, which selects the wavesample the retune is anchored on.
    // See docs/modules/tuning.md.
    virtual int32_t docRetune(int voice, const Tuning& t, int32_t key, int32_t wave) = 0;
    virtual void docStopVoice(int voice) = 0;                       // FUN_0045bd78
    virtual void docInterpolateLevels(int voice) = 0;               // FUN_0045c378 (odd control ticks)
    virtual MasterDocParams& docVoiceParams(int voice) = 0;          // FUN_0045c7e0
    virtual void docUpdate(int voice) = 0;                          // FUN_0045bda4 (after the params are written)
    virtual double docRender(int voice) = 0;                        // FUN_0045c7f4: one sample (ST0)
    virtual int32_t docReleaseSamples() = 0;                        // read of doc+0x209c

    // ------------------------------------------------------------ Csq_lfo (4 per voice)
    virtual void lfoSetControlRate(int voice, int lfo, float rate) = 0;  // FUN_0045d020
    virtual LfoParams& lfoParams(int voice, int lfo) = 0;                // lfo+0xb0
    // FUN_0045d2a4 (trigger): resetPhase clears the phase, then phase << 24 is added.
    virtual void lfoStart(int voice, int lfo, int32_t phase, bool resetPhase) = 0;
    virtual int32_t lfoTick(int voice, int lfo) = 0;                     // FUN_0045db2c: output

    // ------------------------------------------------------------ Csq_env (4 per voice)
    virtual void envSetControlRate(int voice, int env, float rate) = 0;  // FUN_0045dcb8
    // FUN_0045dd2c(env, block, key, velocity, restart, cycle, prev): block is the
    // program's 14-byte envelope record (program+0x6a+env*0xe); restart=false continues
    // from envelope `env` of otherVoice (legato steal; -1 = none, i.e. nil); cycle = full
    // cycle (release ignored).
    virtual void envStart(int voice, int env, const uint8_t* block, int32_t key, int32_t velocity,
                          bool restart, bool cycle, int32_t otherVoice) = 0;
    virtual void envRelease(int voice, int env, bool sustainPedal) = 0;  // FUN_0045dfb4
    virtual int32_t envTick(int voice, int env, bool sustainPedal) = 0;  // FUN_0045e210: output
    virtual int32_t envShaped(int voice, int env) = 0;                    // FUN_0045e1d4 (env 3 only)
    virtual bool envActive(int voice, int env) = 0;                       // read of env+0x60 (env 3 only)

    // ------------------------------------------------------------ Camp (1 per voice)
    virtual void ampSetSampleRate(int voice, float sampleRate) = 0;       // FUN_0045e7b8
    virtual void ampSetSaturation(int voice, int32_t amount) = 0;         // FUN_0045e89c ((int8)program[0x13a])
    virtual void ampStart(int voice, int32_t fromVoice) = 0;              // FUN_0045e554 (from amp, -1 = nil)
    virtual void ampSetRampTime(int voice, float seconds) = 0;            // FUN_0045e7f8
    virtual void ampSetSmoothing(int voice, int32_t value) = 0;           // FUN_0045e848
    // FUN_0045e69c(amp, level, pan, gain, immediate); gain is filter[0].level (Single).
    virtual void ampSetLevelPan(int voice, int32_t level, int32_t pan, float gain, bool immediate) = 0;
    // FUN_0045e8ec / FUN_0045e95c: x is the filtered sample (ST0); the amp adds the
    // stereo result into acc[0] (left) and acc[1] (right).
    virtual void ampProcess(int voice, double x, float* acc) = 0;
    virtual void ampProcessFaded(int voice, double x, float* acc, float fade) = 0;
    virtual int32_t ampRampCount(int voice) = 0;                          // read of amp+0x14

    // ------------------------------------------------------------ Cmod_foll (glide, 1 per voice)
    virtual void follSetControlRate(int voice, float rate) = 0;           // FUN_0045eba0
    virtual void follSetSpeed(int voice, float speed) = 0;                // FUN_0045ebe8
    virtual void follSetTarget(int voice, int32_t target) = 0;            // FUN_0045ec1c
    virtual void follReset(int voice, int32_t value) = 0;                 // FUN_0045ec68
    virtual int32_t follTick(int voice) = 0;                              // FUN_0045ec90: output
};

}  // namespace sq8l
