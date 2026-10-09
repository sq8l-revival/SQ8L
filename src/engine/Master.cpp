#include "Master.h"
#include "CrMath.h"

#include <cmath>
#include <cstring>

#include "Fpu.h"
#include "Program.h"  // (port) programPolyphony

namespace sq8l {

namespace data {
extern const uint8_t kVelocityCurve1[128];
extern const uint8_t kVelocityCurve2[128];
extern const float kFadeInTime[4];
extern const float kAmpRampTime[8];
extern const int32_t kAmpSmoothing[2];
extern const uint16_t kGlideTime[128];
}  // namespace data

namespace {

// 80-bit constants from the original code, split hi/lo (see Fpu.h).
constexpr Ext kMilli = {0x1.0624dd2f1a9fcp-10, -0x1.8a00000000000p-66};        // 0x462330: 0.001
constexpr Ext kFadeOutTime = {0x1.47ae147ae147bp-8, -0x1.ec00000000000p-64};   // 0x462be8: 0.005
constexpr Ext kGlideRate = {0x1.4e5ecc3cd9fb1p+6, -0x1.d800000000000p-48};     // 0x462bf8, 0x462d98: 83.59257598
constexpr Ext kLevelScale = {0x1.0c62b1b039bbfp-6, -0x1.2b00000000000p-61};    // 0x464238: 0.0163809523809524
constexpr Ext kVolScale = {0x1.0204081020408p-7, 0x1.0200000000000p-63};       // 0x464244: 1/127
constexpr Ext kMuffleFreq = {0x1.31076d4fb76f5p+14, -0x1.2f00000000000p-41};   // 0x4643a4: 19521.856749406976
constexpr Ext kMuffleQ = {0x1.6b851eb851eb8p-1, 0x1.4780000000000p-55};        // 0x4643b0: 0.71

// x87 "FLD tbyte c; FDIVR x" at 53-bit precision: x / c rounded once (c = hi + lo).
// One Newton correction of the double quotient; exact for the operands used here
// (verified against the emulator).
double divExt(double x, Ext c) {
    const double q = x / c.hi;
    const double r = std::fma(-q, c.hi, x) - q * c.lo;
    return q + r / c.hi;
}

// x86 32-bit two's complement multiply (IMUL, low 32 bits).
inline int32_t mul32(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}

inline int32_t clampKey(int32_t k) { return k > 0 ? (k < 0x7f ? k : 0x7f) : 0; }  // FUN_00450d90

// Delphi Power(Base, Exponent) for a non-integral exponent: exp(e * ln(b)).
double delphiPower(double base, double exponent) { return crmath::exp(exponent * crmath::log(base)); }

// Modulation source table (0x4c4848, built by FUN_0044feec): entry for source
// s = -1..0x91 at [s + 1]: {1, i} = voice-internal value voice.mod[i] (s = 0..15),
// {0, n} = MIDI controller n (s = 16 + n, n = 0..0x81: CCs, 0x80 channel pressure,
// 0x81 pitch bend >> 6), {-1, -1} = none.
struct ModSourceEntry {
    int32_t type;
    int32_t index;
};
struct ModSourceTable {
    ModSourceEntry e[0x93];
    constexpr ModSourceTable() : e() {
        e[0] = {-1, -1};
        for (int i = 0; i < 16; i++) e[1 + i] = {1, i};
        for (int n = 0; n < 0x82; n++) e[17 + n] = {0, n};
    }
};
constexpr ModSourceTable kModSources;

}  // namespace

// ---------------------------------------------------------------- construction

Master::Master(VoiceModules& modules, int32_t sampleRate, const int32_t* overrides, MasterNotify* notify)
    : modules_(modules), notify_(notify) {
    if (overrides) loadOverrides(overrides);
    // Filters are created while master+0xf70 is still 0 (FUN_0045ef38 with sr 0).
    for (auto& pair : filters_)
        for (auto& f : pair) f.init(&filterTable_, 0.0f);
    setSampleRateInt(sampleRate);
    setControlRate(kDefaultControlRate);
    setVoices(kOriginalPlayableVoices, kOriginalFadeVoices);  // FUN_00463358(8, 8)
    // FUN_0046215c
    parser_.endBlock();
    lockCount_++;  // FUN_004621fc
    reset();
    lockCount_--;
}

void Master::loadOverrides(const int32_t ini[5]) {
    ovrVoiceSteal_ = ini[0];
    ovrDcaSmooth_ = ini[1];
    ovrDca4Smooth_ = ini[2];
    ovrMuffle_ = ini[3];
    ovrDcBlock_ = ini[4];
}

void Master::setCurrentProgram(const uint8_t* program, uint16_t number) {
    currentProgram_ = program;
    programNumber_ = number;
}

// ---------------------------------------------------------------- CSynth glue

int32_t Master::setSampleRate(float sampleRate) { return setSampleRateInt(fistp(sampleRate)); }

void Master::processEvents(const RawMidiEvent* ev, int32_t n) { parser_.processEvents(ev, n); }

// ---------------------------------------------------------------- setup

int32_t Master::setSampleRateInt(int32_t sr) {
    if (sr == sampleRate_) return 0;
    sampleRate_ = sr;
    sampleRateMs_ = sgl(mulExt(kMilli, sr));
    if (sr < 40000) {
        // The original also shows "Sample rate must be 44.1 kHz or above!".
        error_ = -1;
        return static_cast<int32_t>(0xffff53bc);
    }
    invSampleRate_ = sgl(1.0f / static_cast<double>(sr));
    lockCount_++;
    computeMuffle();
    setMuffle(muffleOn_);
    modules_.docSetSampleRate(static_cast<float>(sr));
    for (int v = 0; v < kMaxVoices; v++) {
        modules_.ampSetSampleRate(v, static_cast<float>(sr));
        for (auto& f : filters_[v]) f.setSampleRate(static_cast<float>(sr));
    }
    filterTable_.compute(static_cast<float>(sr));
    setControlRate(controlRate_);
    reset();
    error_ = 0;
    lockCount_--;
    return 0;
}

void Master::setControlRate(float rate) {
    if (!(rate >= 1.0f)) {
        rate = 1.0f;
    } else if (!(static_cast<double>(sampleRate_) > rate)) {
        rate = static_cast<float>(sampleRate_);
    }
    controlStep_ = fistp(sgl(static_cast<double>(sampleRate_) / rate * 512.0f)) - 0x400;
    if (rate == controlRate_) return;
    controlRate_ = rate;
    for (int i = 0; i < kMaxVoices; i++) {
        Voice& v = voices_[i];
        if (controlStep_ < v.ctrlCountdown) v.ctrlCountdown = controlStep_;
        modules_.follSetControlRate(i, rate);
        for (int j = 0; j < 4; j++) modules_.lfoSetControlRate(i, j, rate);
        for (int j = 0; j < 4; j++) modules_.envSetControlRate(i, j, rate);
    }
    // FUN_00417a78 -> FUN_00417a28(0.99, 1 / rate, rate): smoothing coefficients for a
    // time constant of one control period; stored but never used.
    const float inv = sgl(1.0f / static_cast<double>(rate));
    const double base = 1.0f - static_cast<double>(0.99f);
    const double expo = 1.0f / (static_cast<double>(inv) * rate + 1.0f);
    smoothB_ = sgl(delphiPower(base, expo));
    smoothA_ = sgl(1.0f - static_cast<double>(smoothB_));
}

void Master::setVoices(int32_t playable, int32_t fade) {
    lockCount_++;
    numPlay_ = playable;
    numFade_ = fade;
    portLayout_ = false;
    setNumVoices(playable + fade);
    lockCount_--;
}

// (port) see Master.h / VoiceSlots.h.
void Master::setPortLayout() {
    lockCount_++;
    numPlay_ = kMaxPlayableVoices;
    numFade_ = kMaxPlayableVoices;
    portLayout_ = true;
    setNumVoices(kMaxVoiceSlots);
    lockCount_--;
}

void Master::setPolyphonyOverride(int32_t voices) {
    if (voices < 0) voices = 0;
    polyOverride_ = voices > kMaxPlayableVoices ? kMaxPlayableVoices : voices;
}

int32_t Master::effectivePlayableVoices() const {
    if (!portLayout_) return numPlay_;
    int32_t n = polyOverride_ > 0 ? polyOverride_ : programPolyphony(currentProgram_);
    if (n < kMinPlayableVoices) n = kMinPlayableVoices;
    return n > numPlay_ ? numPlay_ : n;
}

void Master::setNumVoices(int32_t n) {
    lockCount_++;
    numVoices_ = n > kMaxVoices ? kMaxVoices : n;
    volume_ = 0.8f;  // 0x3f4ccccd
    reset();
    modules_.docSetNumVoices(numVoices_);
    lockCount_--;
}

void Master::panic() { reset(); }

void Master::reset() {
    lockCount_++;
    initialDelay_ = 0;
    resetVoices();
    modules_.docReset();
    std::memset(ctrl_, 0, sizeof(ctrl_));
    pitchBend_ = 0;
    std::memset(polyPressure_, 0, sizeof(polyPressure_));
    ctrl_[7 + 1] = 0x7f;
    ctrl_[8 + 1] = 0x40;
    ctrl_[74 + 1] = 0x40;
    ctrl_[75 + 1] = 0x40;
    lockCount_--;
}

void Master::resetVoices() {
    resetNoteStacks();
    muffleAge_ = 0;
    muffleVoice_ = -1;
    for (int32_t i = 0; i < numVoices_; i++) {
        clearVoice(i);
        listCount_ = 0;
        list_[i] = {nullptr, -1};
        slotMap_[i] = i;
    }
    // (port) the slots the current layout does not use: kept free so that listRemove and the
    // scans find nothing there, as with the original's arrays.
    for (int32_t i = numVoices_ > kOriginalVoiceSlots ? numVoices_ : kOriginalVoiceSlots; i < kMaxVoices; i++) {
        clearVoice(i);
        list_[i] = {nullptr, -1};
        slotMap_[i] = i;
    }
}

void Master::clearVoice(int32_t slot) {
    if (slot < 0 || slot >= kMaxVoices) return;
    Voice& v = voices_[slot];
    v.active = 0;
    v.age = 0;
    v.slot = slot;
    v.key = -1;
    v.fe4 = 0;
}

void Master::resetNoteStacks() {
    for (int16_t& k : noteStacks_) k = -1;
}

void Master::pushNoteStack(int16_t* stacks, uint8_t key, bool both) {
    if (!stacks || key > 0x7f) return;
    for (int i = 3; i != 0; i--) {
        stacks[i] = stacks[i - 1];
        if (both) stacks[4 + i] = stacks[4 + i - 1];
    }
    stacks[0] = key;
    if (both) stacks[4] = key;
}

void Master::removeHeldKey(int16_t* stacks, uint8_t key) {
    if (!stacks || key > 0x7f) return;
    int16_t* held = stacks + 4;
    int32_t found = -1;
    for (int32_t i = 0; i < 4; i++) {
        if (static_cast<uint16_t>(key) == static_cast<uint16_t>(held[i])) {
            found = i;
            break;
        }
    }
    if (found < 0) return;
    if (found <= 2)
        for (int32_t i = found; i != 3; i++) held[i] = held[i + 1];
    held[3] = -1;
}

int32_t Master::activeVoiceCount() {
    int32_t n = 0;
    // (port) the whole playable region: with the port layout it also counts the voices above
    // a lowered limit that are still sounding. fadeBase() == numPlay_ for the original.
    for (int32_t i = 0; i < fadeBase(); i++)
        if (voiceAt(i).active != 0) n++;
    return n;
}

void Master::editBufferEvent(int32_t code, const uint8_t* program) {
    switch (code) {
    case 0:  // FUN_0046328c
        resetNoteStacks();
        if (notify_) notify_->notifyProgram();
        if (notify_) notify_->notifyEditor(2, 1, 0);
        break;
    case 1:  // FUN_004632b8
        if (notify_) notify_->notifyProgram();
        break;
    case 2:  // FUN_004632c4
    case 3:  // FUN_004632cc
        resetNoteStacks();
        break;
    case 4:  // FUN_004632d4
        killProgram(program);
        break;
    case 5:  // FUN_004632dc
        releaseProgram(program);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- muffle filter

void Master::computeMuffle() {
    // High shelf (RBJ cookbook form) with A = 10^(-1/8), around 19.5 kHz * 2pi / sr.
    const float w = sgl(mulExt(kMuffleFreq, invSampleRate_));
    const float c = sgl(crmath::cos(static_cast<double>(w)));
    const float a = sgl(delphiPower(10.0, -0.125));
    const float beta = sgl(std::sqrt(divExt(a, kMuffleQ)));
    const double am1 = static_cast<double>(a) - 1.0f;
    const double ap1 = static_cast<double>(a) + 1.0f;
    const float a0 = sgl((ap1 - am1 * c) + beta);
    muffleB0_ = sgl(((am1 * c + ap1) + beta) * a / a0);
    muffleB1_ = sgl(-2.0f * static_cast<double>(a) * (ap1 * c + am1) / a0);
    muffleB2_ = sgl(((am1 * c + ap1) - beta) * a / a0);
    muffleA1_ = sgl((am1 - ap1 * c) * 2.0f / a0);
    muffleA2_ = sgl(((ap1 - am1 * c) - beta) / a0);
}

void Master::setMuffle(int32_t mode) {
    if (mode > 0) {
        muffleOn_ = 1;
        for (float& s : muffleState_) s = 0;
    } else {
        muffleOn_ = 0;
    }
}

void Master::muffle(float* acc) {
    float* s = muffleState_;  // x1L x1R x2L x2R y1L y1R y2L y2R
    for (int ch = 0; ch < 2; ch++) {
        double y = static_cast<double>(muffleB1_) * s[0 + ch] + static_cast<double>(muffleB2_) * s[2 + ch];
        y = y - static_cast<double>(muffleA1_) * s[4 + ch];
        y = y - static_cast<double>(muffleA2_) * s[6 + ch];
        y = y + static_cast<double>(muffleB0_) * acc[ch];
        s[2 + ch] = s[0 + ch];
        s[0 + ch] = acc[ch];
        s[6 + ch] = s[4 + ch];
        s[4 + ch] = sgl(y);
        acc[ch] = sgl(y);
    }
}

// ---------------------------------------------------------------- MIDI

void Master::midiNoteOn(uint8_t /*channel*/, uint8_t key, uint8_t velocity) {
    // (port) MTS-ESP: a key the master excludes from its scale does not sound.
    if (tuning_ != nullptr && tuning_->active() && tuning_->filteredKey(key)) return;
    NoteRecord rec;
    const int32_t mode = ovrVoiceSteal_ > 0 ? ovrVoiceSteal_ - 2
                                            : static_cast<int8_t>(currentProgram_[0x195]);
    rec.steal = mode != -1 ? 1 : 0;
    const int32_t noteId = static_cast<int32_t>(static_cast<uint32_t>(programNumber_) << 16);  // FUN_0046093c
    noteEvent(key + rec.transpose, velocity, noteId, &rec);
}

void Master::midiReset(int16_t) { reset(); }

void Master::midiControl(uint8_t channel, uint8_t data1, int16_t value, int16_t ctrl) {
    if (channel > 15 || ctrl < 0) return;
    if (ctrl == 0x7b) {  // all notes off: immediate kill
        for (int32_t i = 0; i < kMaxVoices; i++)
            if (voices_[i].active != 0) kill(i);
    } else if (ctrl == 0x81) {  // pitch bend: raw value kept, table gets value / 64
        pitchBend_ = value;
        value = static_cast<int16_t>(value / 64);
    } else if (ctrl == 0x82) {  // poly pressure
        polyPressure_[data1 & 0x7f] = value;
    }
    storeController(ctrl, value);
}

void Master::storeController(int32_t ctrl, int16_t value) {
    if (ctrl < -1 || ctrl > 0x81) return;
    ctrl_[ctrl + 1] = value;
}

int32_t Master::modSource(const Voice* v, int32_t src) const {
    if (src < -1 || src > 0x91) return 0;
    const ModSourceEntry& e = kModSources.e[src + 1];
    if (e.type == 1) return v ? v->mod[e.index] : 0;
    if (src == 0x81) return 0;
    if (e.index == 0x81) return v->bendActive != 0 ? controller(e.index) : 0;  // FUN_004637f0
    return e.index < 0 ? 0 : controller(e.index);
}

// ---------------------------------------------------------------- note handling

void Master::noteEvent(int32_t key, int32_t velocity, int32_t noteId, NoteRecord* rec) {
    int16_t* stacks = noteStacks_;
    const uint8_t* prog = currentProgram_;
    const uint8_t key8 = static_cast<uint8_t>(key);
    if (velocity > 0 && rec) {
        removeHeldKey(stacks, key8);
        const bool oscRestart = static_cast<int8_t>(prog[0x176]) > 0;
        if (!(static_cast<int8_t>(prog[0x172]) > 0)) {
            // poly
            const bool retrigger = static_cast<int8_t>(prog[0x174]) > 0;
            int32_t found = -1;
            // (port) the scans below run over the whole playable region so that a voice above
            // a lowered limit is still found, released and retargeted. fadeBase() == numPlay_
            // for the original, i.e. the original's scan.
            for (int32_t i = 0; i < fadeBase(); i++) {
                Voice& v = voiceAt(i);
                if (v.active != 0 && v.key == key && v.noteId == noteId) {
                    if (v.released == 0) release(slotMap_[i]);
                    if (found < 0) found = i;
                }
            }
            const int32_t target = (!retrigger || found < 0) ? allocate() : found;
            stealStart(target, noteId, true, rec, oscRestart, true, static_cast<uint8_t>(velocity), stacks[0], key);
            pushNoteStack(stacks, key8, false);
        } else {
            // mono
            bool wasReleased = false, wasMono = false;
            int32_t found = -1;
            for (int32_t i = 0; i < fadeBase(); i++) {
                Voice& v = voiceAt(i);
                if (v.active != 0 && v.noteId == noteId) {
                    wasReleased = v.released != 0;
                    wasMono = v.mono != 0;
                    found = i;
                    break;
                }
            }
            if (found < 0) {
                stealStart(allocate(), noteId, true, rec, oscRestart, true, static_cast<uint8_t>(velocity), stacks[4], key);
            } else if (wasReleased || !wasMono) {
                stealStart(found, noteId, true, rec, oscRestart, true, static_cast<uint8_t>(velocity), stacks[4], key);
            } else {  // legato
                startVoice(slotMap_[found], noteId, false, rec, false, false, static_cast<uint8_t>(velocity), stacks[4], key);
            }
            pushNoteStack(stacks, key8, true);
        }
    } else {
        for (int32_t i = 0; i < fadeBase(); i++) {
            Voice& v = voiceAt(i);
            if (v.active != 0 && v.released == 0 && v.key == key &&
                static_cast<int16_t>(v.noteId) == static_cast<int16_t>(noteId)) {
                if (v.mono == 0 || stacks[5] < 0 || stacks[5] == key) {
                    release(slotMap_[i]);
                } else {
                    retarget(slotMap_[i], stacks[5]);
                }
            }
        }
        removeHeldKey(stacks, key8);
    }
}

void Master::stealStart(int32_t mapIndex, int32_t noteId, bool newNote, NoteRecord* rec, bool oscRestart,
                        bool fullInit, uint8_t velocity, int32_t glideFrom, int32_t key) {
    if (mapIndex < 0 || mapIndex >= kMaxVoices) return;
    int32_t slot = slotMap_[mapIndex];
    rec->stolenFrom = &voices_[slot];
    if (rec->steal != 0) {
        const int32_t j = findStealTarget();
        if (j >= 0) {
            const int32_t fadeSlot = slotMap_[j];
            slotMap_[j] = slot;
            slotMap_[mapIndex] = fadeSlot;
            fadeOut(slot);
            slot = fadeSlot;
        }
    }
    startVoice(slot, noteId, newNote, rec, oscRestart, fullInit, velocity, glideFrom, key);
}

void Master::startVoice(int32_t slot, int32_t noteId, bool newNote, NoteRecord* rec, bool oscRestart,
                        bool fullInit, uint8_t velocity, int32_t glideFrom, int32_t key) {
    if (slot < 0 || numVoices_ <= slot) return;
    Voice& v = voices_[slot];
    v.program = currentProgram_;
    const uint8_t* prog = v.program;
    v.fe4 = 0;
    if (v.active == 0) listAdd(slot);
    v.stealFlag = rec->steal;
    v.fading = 0;
    v.fadeKill = 0;
    v.fadeRemaining = 0;
    v.fadeGain = 1.0f;
    v.fadeStep = 0;
    int32_t n = fistp(sgl(mulExt(kFadeOutTime, sampleRate_)));
    v.fadeOutLength = n;
    v.fadeOutStep = n > 0 ? sgl(1.0f / static_cast<double>(n)) : 0.0f;
    int32_t t = static_cast<int8_t>(prog[0x9b]);
    if (t > 3) t = 3;
    // Negative indices read the pointers stored before the table in the DLL: tiny
    // denormal-range floats, i.e. a zero length (verified for -1..-24).
    const float fadeInTime = t >= 0 ? data::kFadeInTime[t] : 0.0f;
    n = fistp(sgl(static_cast<double>(sampleRate_) * fadeInTime));
    v.fadeInLength = n;
    v.fadeInStep = n > 0 ? sgl(1.0f / static_cast<double>(n)) : 0.0f;

    bool legatoSteal = false;
    Voice* stolen = rec->stolenFrom;
    if (stolen) {
        v.stolenFrom = stolen;
        if (static_cast<int8_t>(prog[0x174]) > 0 &&
            static_cast<int16_t>(stolen->noteId) == static_cast<int16_t>(noteId) && stolen->key == key) {
            legatoSteal = true;
            fadeIn(v.slot);
        }
    } else {
        v.stolenFrom = nullptr;
    }
    v.noteId = noteId;
    int32_t lfoPhase;
    if (rec) {
        v.noteLevel = rec->level;
        v.panOffset = rec->pan;
        lfoPhase = rec->lfoPhase;
    } else {
        v.noteLevel = 0;
        v.panOffset = 0;
        lfoPhase = 0;
    }
    key = clampKey(key);
    glideFrom = glideFrom < 0 ? key : clampKey(glideFrom);
    v.age = 0xffffffffu;
    v.released = 0;
    v.mono = static_cast<int8_t>(prog[0x172]) > 0 ? -1 : 0;
    v.ctrlCountdown = 0;
    v.tickParity = 0;

    if (fullInit) {
        for (int i = 0; i < 4; i++) {
            const uint8_t* blk = prog + 0xa2 + i * 0x10;
            int32_t phase = static_cast<int8_t>(blk[1]);
            const bool resetPhase = phase >= 0;
            if (!resetPhase) phase = 0;
            LfoParams& L = modules_.lfoParams(slot, i);
            L.freq = (blk[0] & 0x7f) << 8;
            L.p04 = static_cast<int8_t>(blk[2]);
            L.p08 = static_cast<int8_t>(blk[3]);
            L.p0c = static_cast<int8_t>(blk[4]);
            L.wave = blk[5] & 0x3f;
            L.p10 = static_cast<int8_t>(blk[6]);
            L.p24 = (blk[5] >> 6) & 1;
            L.p28 = (blk[0xd] >> 4) & 1;
            L.p2c = (blk[0xd] >> 5) & 1;
            L.p34 = (blk[0xf] & 0x3f) << 2;
            modules_.lfoStart(slot, i, lfoPhase + phase, resetPhase);
        }
        for (int i = 0; i < 4; i++) {
            const uint8_t* eblk = prog + 0x6a + i * 0xe;
            const bool cycle = static_cast<int8_t>(prog[0x177]) > 0 || (eblk[0xc] & 1) != 0;
            if (!legatoSteal || !v.stolenFrom) {
                modules_.envStart(slot, i, eblk, key, velocity, true, cycle, -1);
            } else {
                modules_.envStart(slot, i, eblk, key, velocity, static_cast<int8_t>(prog[0x175]) > 0, cycle,
                                  vindex(*v.stolenFrom));
            }
        }
    }
    filters_[slot][0].setKeyParam(static_cast<int8_t>(prog[0x13c]) - 1);
    modules_.ampSetSaturation(slot, static_cast<int8_t>(prog[0x13a]));
    v.tickCount = newNote ? 0 : -1;
    if (newNote) {
        if (!legatoSteal) {
            filters_[slot][0].copyStateFrom(nullptr);
            modules_.ampStart(slot, -1);
        } else {
            const int other = vindex(*v.stolenFrom);
            filters_[slot][0].copyStateFrom(&filters_[other][0]);
            modules_.ampStart(slot, other);
        }
        v.dca4Mode = ovrDca4Smooth_ > 0 ? (ovrDca4Smooth_ - 1) & 1 : (prog[0x196] >> 1) & 1;
        int32_t i = prog[0x9b];
        if (i > 3) i = 3;
        modules_.ampSetRampTime(slot, data::kAmpRampTime[v.dca4Mode * 4 + i]);
    }
    std::memset(v.mod, 0, sizeof(v.mod));
    v.mod[8] = data::kVelocityCurve1[clampKey(velocity)];
    v.mod[9] = data::kVelocityCurve2[clampKey(velocity)];
    setKeyScaling(slot, key);
    modules_.docStartVoice(slot, key, oscRestart ? -1 : 0, newNote ? -1 : 0,
                       v.stolenFrom ? v.stolenFrom->slot : -1);
    v.bend = mul32(pitchBend_, static_cast<int8_t>(prog[0x190])) / 32;
    if (static_cast<int8_t>(prog[0x173]) > 0 && key != glideFrom) {
        v.glideActive = -1;
        v.glideFrom = glideFrom;
        const int32_t dist = std::abs(key - glideFrom) << 8;
        const double time = mulExt(kGlideRate, dist) / data::kGlideTime[static_cast<int8_t>(prog[0x173])];
        modules_.follSetSpeed(slot, sgl(time));
        modules_.follReset(slot, 0);
        modules_.follSetTarget(slot, (key - glideFrom) << 8);
    } else {
        v.glideActive = 0;
        modules_.follReset(slot, 0);
        glideFrom = key;
    }
    if (v.glideActive != 0) modules_.docSetPitchKey(slot, glideFrom);
    v.key = static_cast<int8_t>(key);
    v.active = -1;
}

void Master::retarget(int32_t slot, int32_t key) {
    if (slot < 0 || slot >= numVoices_) return;
    key = clampKey(key);
    Voice& v = voices_[slot];
    const uint8_t* prog = v.program;
    if (static_cast<int8_t>(prog[0x173]) > 0 && key != v.key) {
        v.glideActive = -1;
        const int32_t dist = std::abs(key - v.key) << 8;
        const double time = mulExt(kGlideRate, dist) / data::kGlideTime[static_cast<int8_t>(prog[0x173])];
        modules_.follSetSpeed(slot, sgl(time));
        modules_.follSetTarget(slot, (key - v.glideFrom) << 8);
    } else {
        v.glideActive = 0;
        modules_.follReset(slot, 0);
        modules_.docSetPitchKey(slot, key);
    }
    v.key = static_cast<int8_t>(key);
    setKeyScaling(slot, key);
}

void Master::setKeyScaling(int32_t slot, int32_t key) {
    Voice& v = voices_[slot];
    v.mod[10] = key;
    if (key <= 0x20) {
        v.mod[11] = -0x80;
    } else if (key >= 0x5f) {
        v.mod[11] = 0x7c;
    } else {
        v.mod[11] = (key - 0x40) * 4;
    }
}

void Master::release(int32_t slot) {
    if (slot < 0 || slot >= numVoices_) return;
    Voice& v = voices_[slot];
    if (v.active == 0 || v.released != 0) return;
    for (int i = 0; i < 4; i++) modules_.envRelease(slot, i, sustainPedal());
    v.released = -1;
}

void Master::kill(int32_t slot) {
    if (slot < 0 || slot >= numVoices_) return;
    modules_.docStopVoice(slot);
    Voice& v = voices_[slot];
    v.active = 0;
    v.age = 0;
    listRemove(slot);
}

void Master::killProgram(const uint8_t* program) {
    for (int32_t i = 0; i < kMaxVoices; i++)
        if (voices_[i].active != 0 && voices_[i].program == program) kill(i);
}

void Master::releaseProgram(const uint8_t* program) {
    for (int32_t i = 0; i < kMaxVoices; i++)
        if (voices_[i].active != 0 && voices_[i].program == program) release(i);
}

void Master::fadeOut(int32_t slot) {
    if (slot < 0 || slot >= numVoices_) return;
    Voice& v = voices_[slot];
    v.fading = -1;
    v.fadeKill = -1;
    v.fadeRemaining = v.fadeOutLength;
    if (v.fadeOutLength > 0) {
        v.fadeStep = -v.fadeOutStep;
        v.fadeGain = 1.0f;
    } else {
        v.fadeGain = 0;
        v.fadeStep = 0;
    }
}

void Master::fadeIn(int32_t slot) {
    if (slot < 0 || slot >= numVoices_) return;
    Voice& v = voices_[slot];
    if (v.fadeKill != 0) return;
    v.fadeRemaining = v.fadeInLength;
    if (v.fadeInLength > 0) {
        v.fading = -1;
        v.fadeStep = v.fadeInStep;
        v.fadeGain = 0;
    } else {
        v.fading = 0;
        v.fadeGain = 1.0f;
        v.fadeStep = 0;
    }
}

void Master::listAdd(int32_t slot) {
    if (listCount_ >= numVoices_) return;
    list_[listCount_] = {&voices_[slot], slot};
    listCount_++;
}

void Master::listRemove(int32_t slot) {
    int32_t found = -1;
    for (int32_t i = 0; i < kMaxVoices; i++) {
        if (slot == list_[i].slot) {
            found = i;
            break;
        }
    }
    if (found < 0) return;
    listCount_--;
    if (found < numVoices_ - 1 && found <= kMaxVoices - 2)
        for (int32_t j = found; j != kMaxVoices - 1; j++) list_[j] = list_[j + 1];
    list_[kMaxVoices - 1] = {nullptr, -1};
}

int32_t Master::allocate() {
    // (port) the playable voices of the program / of the override; numPlay_ without the port
    // layout, i.e. the original's scan.
    const int32_t limit = effectivePlayableVoices();
    if (limit <= 1) return 0;
    uint32_t best = 0xffffffffu;
    int32_t result = -1;
    for (int32_t i = 0; i < limit; i++) {
        const Voice& v = voiceAt(i);
        if (v.active == 0 || v.age == 0) {
            result = i;
            break;
        }
        if (v.released != 0 && v.age <= best) {
            best = v.age;
            result = i;
        }
    }
    if (result < 0) {
        result = 0;
        best = voiceAt(0).age;
        for (int32_t i = 1; i < limit; i++) {
            if (voiceAt(i).age <= best) {
                best = voiceAt(i).age;
                result = i;
            }
        }
    }
    if (voiceAt(result).active != 0 && voiceAt(result).age != 0) stealCount_++;  // (port)
    return result;
}

int32_t Master::findStealTarget() {
    // (port) fadeBase() == numPlay_ and the fade slots are numFade_ without the port layout,
    // i.e. the original's scan. With it the fade slots are as many as the playable voices in
    // use, like the original's 8 + 8: that is what keeps 8 voices bit-exact, because the
    // original re-steals the oldest fade slot once they are all busy instead of finding a
    // free one (the heavy-stealing regression cases).
    const int32_t fades = portLayout_ ? effectivePlayableVoices() : numFade_;
    int32_t result = fadeBase();
    uint32_t best = voiceAt(fadeBase()).age;
    if (voiceAt(fadeBase()).active != 0) {
        for (int32_t i = fadeBase() + 1; i <= fadeBase() + fades - 1; i++) {
            const Voice& v = voiceAt(i);
            if (v.active == 0) {
                result = i;
                break;
            }
            if (v.age <= best) {
                best = v.age;
                result = i;
            }
        }
    }
    return result;
}

// ---------------------------------------------------------------- control rate

void Master::controlUpdate(Voice& v) {
    const int vi = vindex(v);
    if (v.tickParity != 0) {
        modules_.docInterpolateLevels(v.slot);
        if (v.tickParity < 1) {
            v.tickParity++;
        } else {
            v.tickParity = 0;
        }
        return;
    }
    v.tickParity = 1;
    tickToggle_ = (tickToggle_ + 1) & 1;
    const uint8_t* prog = v.program;
    const uint8_t* P = prog + 0x22;
    auto word = [](const uint8_t* p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); };
    auto s8 = [](uint8_t b) { return static_cast<int32_t>(static_cast<int8_t>(b)); };

    v.mod[12] = polyPressure_[v.key & 0x7f];
    int32_t pitch = v.glideActive != 0 ? modules_.follTick(vi) : 0;

    // Pitch bend mode (program[0x191]): 1 = while not released, 2 = newest key only,
    // 3 = both, other = always; 4..6 = 1..3 without storing the new bend in the voice.
    int32_t mode = s8(P[0x16f]);
    bool storeBend = true;
    if (mode >= 4) {
        mode -= 3;
        storeBend = false;
    }
    bool bendOn;
    switch (mode) {
    case 1:
        bendOn = v.released == 0;
        break;
    case 2:
        bendOn = v.key == noteStacks_[0];
        break;
    case 3:
        bendOn = v.released == 0 && v.key == noteStacks_[0];
        break;
    default:
        bendOn = true;
        break;
    }
    int32_t bend;
    if (bendOn) {
        bend = mul32(pitchBend_, s8(P[0x16e])) / 32;
        if (storeBend) v.bend = bend;
        v.bendActive = -1;
    } else {
        bend = v.bend;
        v.bendActive = 0;
    }
    pitch += bend;

    // (port) MTS-ESP: whole semitones ride the pitch path like the bend above, the residual
    // the voice's resampler clock. osc 0's wave (S + 3 below, i = 0) selects the wavesample
    // the retune is anchored on. See docs/modules/tuning.md.
    if (tuning_ != nullptr) pitch += modules_.docRetune(v.slot, *tuning_, v.key, word(P + 3));

    for (int i = 0; i < 4; i++) v.mod[4 + i] = modules_.envTick(vi, i, sustainPedal());

    for (int i = 0; i < 4; i++) {
        const uint8_t* blk = P + 0x80 + i * 0x10;
        LfoParams& L = modules_.lfoParams(vi, i);
        L.freq = (blk[0] & 0x7f) << 8;
        L.p04 = s8(blk[2]);
        L.p08 = s8(blk[3]);
        L.p0c = s8(blk[4]);
        L.wave = blk[5] & 0x3f;
        L.p10 = s8(blk[6]);
        L.p24 = (blk[5] >> 6) & 1;
        L.p28 = (blk[0xd] >> 4) & 1;
        L.p2c = (blk[0xd] >> 5) & 1;
        int32_t level = 0;
        L.p34 = 0;
        L.depthMod = 0;
        int32_t val = modSource(&v, word(blk + 0xa));
        int32_t m = (blk[0xd] >> 2) & 3;
        if (m < 2) {
            if (val > 0 || m > 0) L.freq += mul32(s8(blk[0xc]), val) * 2;
        } else if (m == 2) {
            level = mul32(s8(blk[0xc]), val) * 4;
        } else {
            L.p34 = mul32(s8(blk[0xc]), val);
        }
        val = modSource(&v, word(blk + 7));
        m = blk[0xd] & 3;
        if (m < 2) {
            if (val > 0 || m > 0) L.depthMod += mul32(s8(blk[9]), val) / 0x3f;
        } else if (m == 2) {
            level += mul32(s8(blk[9]), val) * 8;
        } else {
            L.p34 = mul32(s8(blk[9]), val) * 2;
        }
        level += (blk[0xe] & 0x3f) << 10;
        if (!(blk[0xe] & 0x40)) {
            L.level = level;
            L.levelSel = 0;
        } else {
            L.level = 0;
            L.levelAlt = level;
            L.levelSel = 1;
        }
        if (L.p34 == 0) {
            L.p34 = (blk[0xf] & 0x3f) << 2;
        } else {
            L.p34 = (((blk[0xf] & 0x3f) << 8) + L.p34) / 64;
        }
        v.mod[i] = modules_.lfoTick(vi, i);
    }

    // MAT1-3: (src1*amt1 + src2*amt2 + src3*amt3) * ((src4 - 127) * amt4 + 8001) / 504063
    for (int i = 0; i < 3; i++) {
        const uint8_t* M = P + 0xc0 + i * 0xc;
        int32_t sum = mul32(modSource(&v, word(M)), s8(M[2]));
        sum += mul32(modSource(&v, word(M + 3)), s8(M[5]));
        sum += mul32(modSource(&v, word(M + 6)), s8(M[8]));
        const int32_t scale = mul32(modSource(&v, word(M + 9)) - 0x7f, s8(M[0xb])) + 0x1f41;
        if (sum != 0 || scale != 0) {
            v.mod[13 + i] = mul32(sum, scale) / 0x7b0ff;
        } else {
            v.mod[13 + i] = 0;
        }
    }

    MasterDocParams& D = modules_.docVoiceParams(v.slot);
    D.f64 = s8(P[0x14f]);
    D.f68 = s8(P[0x14e]) > 0 ? -1 : 0;
    D.f6c = (P[0x170] & 1) ? -1 : 0;
    D.f70 = (P[0x174] & 1) ? -1 : 0;
    D.dcaSmoothing = ovrDcaSmooth_ > 0 ? ovrDcaSmooth_ : ((P[0x170] >> 1) & 3) + 1;
    const int32_t dcb = ovrDcBlock_ > 0 ? ovrDcBlock_ - 1 : (P[0x170] >> 3) & 3;
    if (dcb == 0) {
        D.dcBlock = s8(P[0x14e]) > 0 ? 1 : 0;
    } else if (dcb == 1) {
        D.dcBlock = 1;
    } else {
        D.dcBlock = 0;
    }
    for (int i = 0; i < 3; i++) {
        MasterDocOsc& O = D.osc[i];
        const uint8_t* S = P + i * 0x18;
        int32_t fm = mul32(modSource(&v, word(S + 5)), s8(S[7]));
        fm += mul32(modSource(&v, word(S + 8)), s8(S[0xa]));
        int32_t am = mul32(modSource(&v, word(S + 0x10)), s8(S[0x12]));
        am += mul32(modSource(&v, word(S + 0x13)), s8(S[0x15]));
        O.enabled = s8(S[0xf]) > 0 ? -1 : 0;
        O.wave = word(S + 3);
        O.pitch = s8(S[0]) * 12 + s8(S[1]);
        O.fine = s8(S[2]);
        O.pitchMod = pitch + fm;
        O.level = am * 4 + (s8(S[0xe]) << 9);
    }
    modules_.docUpdate(v.slot);

    // Filter: cutoff = FREQ*2 + (mods + key tracking + (CC74 - 64) * 192) / 64
    FilterSQ& flt = filters_[vi][0];
    flt.setKeyParam(s8(prog[0x13c]) - 1);
    const uint8_t* F = P + 0xe4;
    int32_t fmod = mul32(modSource(&v, word(F + 4)), s8(F[6]));
    fmod += mul32(modSource(&v, word(F + 7)), s8(F[9]));
    fmod += mul32(v.key, s8(F[2])) * 2;
    fmod += (modSource(&v, 0x5a) - 0x40) * 64 * 3;
    flt.setParams(s8(F[0]) * 2 + fmod / 64, s8(F[1]), v.tickCount == 0);

    // Amp
    modules_.ampSetSaturation(vi, s8(P[0x118]));
    const uint8_t* A = P + 0x110;
    const int32_t levelMod = mul32(modSource(&v, word(A + 2)), s8(A[4]));
    int32_t panMod = mul32(modSource(&v, word(A + 5)), s8(A[7]));
    panMod += modSource(&v, 0x18) - 0x40;  // CC8
    int32_t lvl = fistp(sgl(mulExt(kLevelScale, s8(A[0])) * v.noteLevel));
    lvl += static_cast<int32_t>(static_cast<uint32_t>(modSource(&v, 0x1b)) >> 1);  // CC11
    lvl = mul32(lvl, mul32(modules_.envShaped(vi, 3), 2));
    lvl /= 64;
    const double env = lvl;
    const int32_t vol = modSource(&v, 0x17) + levelMod / 64;  // CC7
    const int32_t level = fistp(sgl(mulExt(kVolScale, vol) * env));
    modules_.ampSetSmoothing(vi, data::kAmpSmoothing[v.dca4Mode]);
    modules_.ampSetLevelPan(vi, level, s8(prog[0x133]) + v.panOffset + panMod / 128, flt.level, false);

    // The newest voice decides the muffle mode.
    if (v.age >= muffleAge_ || v.slot == muffleVoice_) {
        muffleAge_ = v.age;
        muffleVoice_ = v.slot;
        const int32_t m = ovrMuffle_ > 0 ? ovrMuffle_ - 1 : (static_cast<uint32_t>(s8(prog[0x13b])) >> 2) & 1;
        if (muffleOn_ != m) setMuffle(m);
    }
    if (v.tickCount == 1) modules_.ampSetRampTime(vi, data::kAmpRampTime[v.dca4Mode * 4 + 3]);
    if (v.tickCount >= 0 && v.tickCount < 0x7fffffff) v.tickCount++;
}

// ---------------------------------------------------------------- audio

bool Master::processVoice(Voice& v, float* acc) {
    const int vi = vindex(v);
    if (v.age > 0) v.age--;
    if (v.ctrlCountdown <= 0) {
        v.ctrlCountdown += controlStep_;
        if (modules_.envActive(vi, 3)) controlUpdate(v);
        if (!modules_.envActive(vi, 3)) modules_.docStopVoice(v.slot);
    } else {
        v.ctrlCountdown -= 0x400;
    }
    if (!modules_.envActive(vi, 3) && !(modules_.ampRampCount(vi) > -modules_.docReleaseSamples())) return true;
    double x = modules_.docRender(v.slot);
    x = filters_[vi][0].process(x);
    bool finished = false;
    if (v.fading == 0) {
        modules_.ampProcess(vi, x, acc);
    } else {
        modules_.ampProcessFaded(vi, x, acc, v.fadeGain);
        if (v.fadeRemaining > 0) {
            v.fadeRemaining--;
            v.fadeGain = sgl(static_cast<double>(v.fadeGain) + v.fadeStep);
        } else {
            if (v.fadeKill != 0) finished = true;
            v.fading = 0;
        }
    }
    return finished;
}

void Master::process(float* outL, float* outR, int32_t n, bool replacing) {
    inProcess_ = 1;
    {
        RoundTowardZero rz;
        if (outL && outR) {
            if (error_ != 0 || lockCount_ > 0 || numVoices_ <= 0) {
                if (replacing && n > 0) {
                    std::memset(outL, 0, sizeof(float) * n);
                    std::memset(outR, 0, sizeof(float) * n);
                }
            } else {
                parser_.beginBlock();
                int32_t finished[kMaxVoices];
                for (int32_t& f : finished) f = -1;
                for (int32_t s = 0; s < n; s++) {
                    parser_.tick();
                    if (listCount_ > 0) {
                        float acc[2] = {0.0f, 0.0f};
                        int32_t nFinished = 0;
                        const int32_t count = listCount_;
                        for (int32_t i = 0; i < count; i++)
                            if (processVoice(*list_[i].voice, acc)) finished[nFinished++] = list_[i].slot;
                        for (int32_t j = 0; j < nFinished; j++) {
                            kill(finished[j]);
                            finished[j] = -1;
                        }
                        if (muffleOn_ > 0) muffle(acc);
                        if (replacing) {
                            outL[s] = sgl(static_cast<double>(acc[0]) * volume_);
                            outR[s] = sgl(static_cast<double>(acc[1]) * volume_);
                        } else {
                            outL[s] = sgl(static_cast<double>(acc[0]) * volume_ + outL[s]);
                            outR[s] = sgl(static_cast<double>(acc[1]) * volume_ + outR[s]);
                        }
                    } else if (replacing) {
                        outL[s] = 0;
                        outR[s] = 0;
                    }
                }
            }
        }
        parser_.endBlock();
    }
    inProcess_ = 0;
}

}  // namespace sq8l
