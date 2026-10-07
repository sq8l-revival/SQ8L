// The complete SQ8L engine: master + DSP modules + edit buffer + library, with the
// behaviour of the original VST wrapper class CSynth (unit uPlug_SQ8L).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Amp.h"
#include "Doc.h"
#include "EditBuffer.h"
#include "Env.h"
#include "Lfo.h"
#include "Master.h"
#include "MidiParser.h"
#include "ModFollower.h"
#include "Settings.h"
#include "SoundLibrary.h"
#include "VoiceModules.h"

namespace sq8l {

// VoiceModules implemented with the real module ports (one Doc, per voice slot 4 LFOs,
// 4 envelopes, 1 amp, 1 mod follower), constructed like the original master does.
class SynthModules final : public VoiceModules {
public:
    SynthModules();

    void docReset() override { doc.resetAll(); }
    void docSetNumVoices(int32_t n) override { doc.setNumVoices(static_cast<uint32_t>(n)); }
    void docSetSampleRate(float sr) override { doc.setSampleRate(sr); }
    void docStartVoice(int v, int32_t key, int32_t resetPhase, int32_t newNote, int32_t linkedVoice) override {
        doc.startVoice(static_cast<uint32_t>(v), key, linkedVoice, newNote, resetPhase);
    }
    void docSetPitchKey(int v, int32_t key) override { doc.setPitchKey(static_cast<uint32_t>(v), key); }
    void docStopVoice(int v) override { doc.stopVoice(static_cast<uint32_t>(v)); }
    void docInterpolateLevels(int v) override { doc.interpolateLevels(static_cast<uint32_t>(v)); }
    MasterDocParams& docVoiceParams(int v) override;
    void docUpdate(int v) override { doc.update(static_cast<uint32_t>(v)); }
    double docRender(int v) override { return doc.render(static_cast<uint32_t>(v)); }
    int32_t docReleaseSamples() override { return doc.releaseSamples; }

    void lfoSetControlRate(int v, int i, float rate) override { lfo[v][i].setControlRate(rate); }
    LfoParams& lfoParams(int v, int i) override;
    void lfoStart(int v, int i, int32_t phase, bool reset) override { lfo[v][i].trigger(phase, reset); }
    int32_t lfoTick(int v, int i) override { return lfo[v][i].tick(); }

    void envSetControlRate(int v, int i, float rate) override { env[v][i].setRate(rate); }
    void envStart(int v, int i, const uint8_t* block, int32_t key, int32_t velocity, bool restart, bool cycle,
                  int32_t otherVoice) override {
        env[v][i].start(block, key, velocity, restart ? 1 : 0, cycle ? 1 : 0,
                        otherVoice >= 0 ? &env[otherVoice][i] : nullptr);
    }
    void envRelease(int v, int i, bool pedal) override { env[v][i].release(pedal); }
    int32_t envTick(int v, int i, bool pedal) override { return env[v][i].tick(pedal); }
    int32_t envShaped(int v, int i) override { return env[v][i].shaped(); }
    bool envActive(int v, int i) override { return env[v][i].active != 0; }

    void ampSetSampleRate(int v, float sr) override { amp[v].setSampleRate(sr); }
    void ampSetSaturation(int v, int32_t a) override { amp[v].setSaturation(a); }
    void ampStart(int v, int32_t from) override { amp[v].start(from >= 0 ? &amp[from] : nullptr); }
    void ampSetRampTime(int v, float s) override { amp[v].setRampTime(s); }
    void ampSetSmoothing(int v, int32_t x) override { amp[v].setSmoothing(x); }
    void ampSetLevelPan(int v, int32_t level, int32_t pan, float gain, bool immediate) override {
        amp[v].setLevelPan(level, pan, gain, immediate);
    }
    void ampProcess(int v, double x, float* acc) override { amp[v].process(x, acc); }
    void ampProcessFaded(int v, double x, float* acc, float fade) override { amp[v].processFade(x, acc, fade); }
    int32_t ampRampCount(int v) override { return amp[v].rampCount; }

    void follSetControlRate(int v, float rate) override { foll[v].setRate(rate); }
    void follSetSpeed(int v, float speed) override { foll[v].setSpeed(speed); }
    void follSetTarget(int v, int32_t t) override { foll[v].setTarget(t); }
    void follReset(int v, int32_t x) override { foll[v].reset(x); }
    int32_t follTick(int v) override { return foll[v].tick(); }

    Doc doc;
    Lfo lfo[kMaxVoiceSlots][4];
    Env env[kMaxVoiceSlots][4];
    Amp amp[kMaxVoiceSlots];
    ModFollower foll[kMaxVoiceSlots];
};

class Synth {
public:
    static constexpr int kNumPrograms = SoundLibrary::kNumPrograms;

    // library: shared library (the original keeps one per DLL); nullptr = own factory library.
    // notify: receives the master's host/editor notifications (program loaded etc.).
    explicit Synth(float sampleRate = 44100.0f, SoundLibrary* library = nullptr,
                   const Settings* settings = nullptr, MasterNotify* notify = nullptr);

    // ---- VST2 dispatcher semantics (CSynth)
    void setSampleRate(float sr);                         // effSetSampleRate (CSynth_v000)
    void processEvents(const RawMidiEvent* ev, int32_t n);  // effProcessEvents (CSynth_v040)
    void process(float* outL, float* outR, int32_t n, bool replacing = true);  // CSynth_v006 / v005
    void setProgram(int32_t index);                       // effSetProgram (CSynth_v010/v011)
    int32_t program() const;                              // effGetProgram
    std::string programName() const;                      // effGetProgramName (edit buffer name)
    std::string programNameIndexed(int32_t index) const;  // effGetProgramNameIndexed
    void setProgramName(const std::string& name);         // effSetProgramName
    std::vector<uint8_t> getChunk();                      // effGetChunk (CSynth_v018)
    int32_t setChunk(const uint8_t* data, size_t size);   // effSetChunk (CSynth_v019): 0 ok, -1 error

    Master& master() { return *master_; }
    // (port) Playable voices. The program's (EMU -> VOICES, ofs::Polyphony) unless this
    // instance overrides them with OPTIONS -> Polyphony: 1..64, 0 = set by the program. The
    // override belongs to the instance, so the plugin saves it in its own state, not in
    // SQ8L.ini. Nothing resets: the value is a limit read at the next note on.
    void setPolyphonyOverride(int voices);
    int polyphonyOverride() const;
    int polyphony() const;
    EditBuffer& editBuffer() { return *edit_; }
    SoundLibrary& library() { return *library_; }
    SynthModules& modules() { return *modules_; }

private:
    void syncProgram();

    std::unique_ptr<SoundLibrary> ownLibrary_;
    SoundLibrary* library_;
    Settings settings_;
    std::unique_ptr<SynthModules> modules_;
    std::unique_ptr<EditBuffer> edit_;
    std::unique_ptr<Master> master_;
    bool chunkLoaded_ = false;  // CSynth +0xb4: ignore the next setProgram after setChunk
};

}  // namespace sq8l
