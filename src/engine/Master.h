// SQ8L voice master (original unit plugCore, class CplugMaster) and the VST-facing glue
// of CSynth (unit uPlug_SQ8L): voice allocation/stealing, mono/poly/legato note
// handling, control-rate modulation (mod sources, matrices, LFO/env/osc/filter/amp
// parameter updates), per-sample voice rendering, "muffle" shelving filter, output.
//
// The DSP modules are reached through VoiceModules (see VoiceModules.h); the two
// filters per voice (FilterSQ) and their coefficient table are owned here.
// See docs/modules/master.md for the layout, semantics and quirks.
#pragma once

#include <cstdint>

#include "FilterSQ.h"
#include "MidiParser.h"
#include "Voice.h"
#include "VoiceModules.h"
#include "VoiceSlots.h"

namespace sq8l {

// Notifications the original sends to its host object (method pointers stored at
// master+0xfd0 and +0xfd8, set by CSynth). Optional; no DSP effect.
class MasterNotify {
public:
    virtual ~MasterNotify() = default;
    virtual void notifyProgram() {}                                   // FUN_00461fb8 -> CSynth 0x487c28
    virtual void notifyEditor(int32_t, int32_t, int32_t) {}            // FUN_00461fd8 -> CSynth 0x487bb8
};

// Note record built by the note callback (LAB_00463388) on its stack.
struct NoteRecord {
    int16_t transpose = 0;        // +0x00  added to the key (always 0)
    int16_t r02 = 0;              // +0x02
    int16_t level = 0x3f;         // +0x04  -> voice.noteLevel
    int16_t pan = 0;              // +0x06  -> voice.panOffset
    int16_t lfoPhase = 0;         // +0x08  added to every LFO start phase
    int16_t r0a = 0;              // +0x0a
    Voice* stolenFrom = nullptr;  // +0x0c  set by stealStart
    int32_t steal = 0;            // +0x10  1 = voice stealing with fades enabled
};

class Master final : public MidiListener {
public:
    static constexpr int kMaxVoices = kMaxVoiceSlots;  // the original: 16 (see VoiceSlots.h)
    static constexpr float kDefaultControlRate = 83.592575f;  // 0x42a72f66

    // FUN_00461cac minus the construction of the modules (done by the caller, who also
    // provides the program record with setCurrentProgram before any note arrives).
    // `overrides`: the [synth] settings as for loadOverrides (nullptr = all 0), loaded
    // first like the original (FUN_00462044). `modules` must outlive the master.
    // Runs under the caller's rounding mode.
    Master(VoiceModules& modules, int32_t sampleRate, const int32_t* overrides = nullptr,
           MasterNotify* notify = nullptr);

    // ---------------------------------------------------------------- CSynth glue
    int32_t setSampleRate(float sampleRate);          // CSynth_v000: FUN_00462214(Round(sr))
    void processEvents(const RawMidiEvent* ev, int32_t n);  // CSynth_v040 -> CMidiParser
    // CSynth_v006 (replacing) / CSynth_v005 (accumulating) -> FUN_004645c8. Runs in
    // round-toward-zero like the original (x87 CW | 0xC00).
    void process(float* outL, float* outR, int32_t n, bool replacing);

    // ---------------------------------------------------------------- master API
    int32_t setSampleRateInt(int32_t sampleRate);    // FUN_00462214: 0 or 0xffff53bc (sr < 40000)
    void setControlRate(float rate);                  // FUN_00462380
    void setVoices(int32_t playable, int32_t fade);   // FUN_00463358 (the original uses 8 + 8)
    // (port) The layout the plugin uses: kMaxPlayableVoices playable slots (0..63) and the
    // same number of fade slots at kFadeSlotBase (64..127), so a voice of any program can
    // always fade out. The playable voices actually allocated are the per-instance override
    // or, with no override, the current program's (ofs::Polyphony) - a limit read at every
    // note on, so a program change or an edit never resets anything. Resets like setVoices.
    void setPortLayout();
    // (port) Per-instance playable voices (OPTIONS -> Polyphony), 0 = set by the program.
    // Clamped to 1..kMaxPlayableVoices; takes effect at the next note on.
    void setPolyphonyOverride(int32_t voices);
    int32_t polyphonyOverride() const { return polyOverride_; }
    // (port) The voices notes are allocated from now: numPlay_ without the port layout.
    int32_t effectivePlayableVoices() const;
    uint32_t stealCount() const { return stealCount_; }  // (port) see stealCount_
    void panic();                                     // FUN_00462174 (GUI panic button, MIDI reset)
    // FUN_004620e4: the 5 [synth] settings by config index (FUN_00452c5c(cfg, i), i.e. the
    // SQ8L.ini keys voiceStealMode, muffleMode, oscDcaMode, dca4Mode, dcbMode in that
    // order) -> +0xfe4, +0xfec, +0xff0, +0xfe8, +0xff4. 0 = set by the program, else
    // option + 1. Note the key names do not match the use of the fields (see master.md).
    void loadOverrides(const int32_t ini[5]);
    // The edit buffer's current program (CeditBuf+0xa94) and program number (+0xb74):
    // read at note on. The record must stay valid while voices play it.
    void setCurrentProgram(const uint8_t* program, uint16_t number);
    // Edit-buffer notifications (callback LAB_00463230): 0 program changed, 1 refresh,
    // 2/3 reset note stacks, 4 kill / 5 release the voices playing `program`.
    void editBufferEvent(int32_t code, const uint8_t* program);
    int32_t activeVoiceCount();                       // FUN_00463328 (playable voices)

    // MidiListener (callbacks registered with the parser in FUN_00461cac)
    void midiNoteOn(uint8_t channel, uint8_t key, uint8_t velocity) override;  // LAB_00463388
    void midiControl(uint8_t channel, uint8_t data1, int16_t value, int16_t ctrl) override;  // FUN_00463714
    void midiReset(int16_t value) override;           // LAB_00463380 -> FUN_00462174

    const Voice& voice(int i) const { return voices_[i]; }
    const FilterSQ& filter(int voice, int i) const { return filters_[voice][i]; }
    float volume() const { return volume_; }

private:
    friend struct MasterTestAccess;

    int vindex(const Voice& v) const { return static_cast<int>(&v - voices_); }
    Voice& voiceAt(int32_t mapIndex) { return voices_[slotMap_[mapIndex]]; }  // FUN_00462ffc
    int16_t controller(int32_t n) const { return ctrl_[n + 1]; }
    bool sustainPedal() const { return static_cast<uint8_t>(controller(64)) != 0; }  // FUN_004637c4

    void reset();                                     // FUN_00462174
    void resetVoices();                               // FUN_00462570
    void clearVoice(int32_t slot);                    // FUN_00462544
    void resetNoteStacks();                           // FUN_00462488
    void pushNoteStack(int16_t* stacks, uint8_t key, bool both);  // FUN_004624a8
    void removeHeldKey(int16_t* stacks, uint8_t key);              // FUN_004624f4
    void setNumVoices(int32_t n);                     // FUN_004632e4
    void computeMuffle();                             // FUN_00464250
    void setMuffle(int32_t mode);                     // FUN_004643c8
    void muffle(float* acc);                          // FUN_004644f4

    void noteEvent(int32_t key, int32_t velocity, int32_t noteId, NoteRecord* rec);  // FUN_00463420
    void stealStart(int32_t mapIndex, int32_t noteId, bool newNote, NoteRecord* rec, bool oscRestart,
                    bool fullInit, uint8_t velocity, int32_t glideFrom, int32_t key);  // FUN_00462c04
    void startVoice(int32_t slot, int32_t noteId, bool newNote, NoteRecord* rec, bool oscRestart,
                    bool fullInit, uint8_t velocity, int32_t glideFrom, int32_t key);  // FUN_004625d4
    void retarget(int32_t slot, int32_t key);         // FUN_00462ca0 (mono: back to a held key)
    void setKeyScaling(int32_t slot, int32_t key);    // FUN_00462da4
    void release(int32_t slot);                       // FUN_00462de4
    void kill(int32_t slot);                          // FUN_00462e30
    void killProgram(const uint8_t* program);         // FUN_00462e6c
    void releaseProgram(const uint8_t* program);      // FUN_00462e9c
    void fadeOut(int32_t slot);                       // FUN_00462ecc
    void fadeIn(int32_t slot);                        // FUN_00462f18
    void listAdd(int32_t slot);                       // FUN_00462f64
    void listRemove(int32_t slot);                    // FUN_00462f90
    int32_t allocate();                               // FUN_0046300c (index into slotMap_)
    int32_t findStealTarget();                        // FUN_004630cc (index into slotMap_)
    void storeController(int32_t ctrl, int16_t value);  // FUN_004637d4
    int32_t modSource(const Voice* v, int32_t src) const;  // FUN_00463808
    void controlUpdate(Voice& v);                     // FUN_00463890
    bool processVoice(Voice& v, float* acc);          // FUN_00464410: true = finished

    VoiceModules& modules_;
    MasterNotify* notify_;
    MidiParser parser_{this};                         // +0xffc
    const uint8_t* currentProgram_ = nullptr;         // editbuf+0xa94
    uint16_t programNumber_ = 0;                      // editbuf+0xb74

    // Comments give the field offset in the original CplugMaster object.
    Voice voices_[kMaxVoices];                        // +0x0004
    int32_t numVoices_ = 0;                           // +0x0e84
    struct ListEntry {
        Voice* voice;
        int32_t slot;
    };
    ListEntry list_[kMaxVoices] = {};                 // +0x0e88 sounding voices, in start order
    int32_t listCount_ = 0;                           // +0x0f08
    int32_t slotMap_[kMaxVoices] = {};                // +0x0f0c playable [0, numPlay) + fade slots
    int32_t numPlay_ = 0;                             // +0x0f4c
    int32_t numFade_ = 0;                             // +0x0f50
    // (port) first fade slot in slotMap_: numPlay_ like the original, kFadeSlotBase with the
    // port layout, where the playable region keeps its size and only the allocator's limit
    // (effectivePlayableVoices) changes. Everything reaches the voices through slotMap_, so
    // this moves the fade slots without touching anything else.
    int32_t fadeBase() const { return portLayout_ ? kFadeSlotBase : numPlay_; }  // derived, never stale
    bool portLayout_ = false;
    int32_t polyOverride_ = 0;                        // 0 = set by the program
    int16_t noteStacks_[8] = {};                      // +0x0f54 [0..3] last keys, [4..7] held keys (mono)
    float volume_ = 0;                                // +0x0f64
    int32_t muffleVoice_ = 0;                         // +0x0f68 slot of the newest voice (muffle owner)
    uint32_t muffleAge_ = 0;                          // +0x0f6c its age
    int32_t sampleRate_ = 0;                          // +0x0f70
    float invSampleRate_ = 0;                         // +0x0f74
    float sampleRateMs_ = 0;                          // +0x0f78 sr * 0.001 (unused)
    float controlRate_ = 0;                           // +0x0f7c
    int32_t controlStep_ = 0;                         // +0x0f80 Round(sr / rate * 512) - 1024
    int32_t initialDelay_ = 0;                        // +0x0f84 (copied to AEffect.initialDelay)
    int32_t error_ = 0;                               // +0x0f88 -1: sample rate < 40000
    int32_t lockCount_ = 0;                           // +0x0f8c process outputs silence while > 0
    int32_t muffleOn_ = 0;                            // +0x0f94
    float muffleB0_ = 0, muffleB1_ = 0, muffleB2_ = 0;  // +0x0f98..+0x0fa0
    float muffleA1_ = 0, muffleA2_ = 0;               // +0x0fa4, +0x0fa8
    // +0x0fac: x1L x1R x2L x2R y1L y1R y2L y2R
    float muffleState_[8] = {};
    // [synth] emulation overrides (FUN_004620e4): 0 = set by the program, else option + 1
    int32_t ovrVoiceSteal_ = 0;                       // +0x0fe4 voice stealing (ini index 0)
    int32_t ovrMuffle_ = 0;                           // +0x0fe8 muffle (ini index 3)
    int32_t ovrDcaSmooth_ = 0;                        // +0x0fec DCA1-3 smoothing, Cdoc +0x78 (ini index 1)
    int32_t ovrDca4Smooth_ = 0;                       // +0x0ff0 DCA4 smoothing, voice.dca4Mode (ini index 2)
    int32_t ovrDcBlock_ = 0;                          // +0x0ff4 DC blocking, Cdoc +0x7c (ini index 4)
    FilterTable filterTable_{};                       // +0x1004
    FilterSQ filters_[kMaxVoices][2];                 // voice +0x74 / +0x78 objects
    int32_t tickToggle_ = 0;                          // +0x11004
    float smoothB_ = 0;                               // +0x11008 (computed, never used)
    float smoothA_ = 0;                               // +0x1100c (computed, never used)
    int16_t ctrl_[131] = {};                          // +0x11010 + (n + 1) * 16, n = -1..0x81
    // (port) notes that took over a sounding voice (allocate): with more voices they would
    // have found a free one. Not in the original; tests/render_check.cpp --voices uses it.
    uint32_t stealCount_ = 0;
    int16_t polyPressure_[128] = {};                  // +0x11840 + key * 16
    int16_t pitchBend_ = 0;                           // +0x12040 raw -8192..8191
    uint8_t inProcess_ = 0;                           // +0x12050
};

}  // namespace sq8l
