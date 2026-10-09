// C API: sq8l::Master with a forwarding VoiceModules for the differential tests.
//
// Every module call made by the C++ master is forwarded to a test callback
// (method id, voice, index, io[]) that checks it against the calls the original
// master made in the emulator and supplies the original's results. The master state
// can be loaded from / saved to the byte layout of the original CplugMaster.
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "Fpu.h"
#include "Master.h"

#include "capi_export.h"

namespace sq8l {

// Method ids shared with tests/test_master.py.
enum Method : int32_t {
    M_DOC_RESET = 1, M_DOC_NUMVOICES, M_DOC_SR, M_DOC_NOTEON, M_DOC_SETKEY, M_DOC_STOP,
    M_DOC_TICKODD, M_DOC_UPDATE, M_DOC_RENDER, M_DOC_TAIL,
    M_LFO_RATE, M_LFO_START, M_LFO_TICK,
    M_ENV_RATE, M_ENV_START, M_ENV_RELEASE, M_ENV_TICK, M_ENV_LEVEL, M_ENV_ACTIVE,
    M_AMP_SR, M_AMP_DRIVE, M_AMP_START, M_AMP_SMOOTH, M_AMP_CURVE, M_AMP_LEVEL, M_AMP_PROCESS,
    M_AMP_FADED, M_AMP_COUNTER,
    M_FOLL_RATE, M_FOLL_TIME, M_FOLL_TARGET, M_FOLL_VALUE, M_FOLL_TICK,
};

using Callback = void (*)(int32_t method, int32_t voice, int32_t index, double* io);

constexpr uint32_t kProgramSize = 0x21c;

struct Programs {
    std::map<uint32_t, std::unique_ptr<uint8_t[]>> byGuest;
    uint8_t* get(uint32_t guest) {
        if (!guest) return nullptr;
        auto& p = byGuest[guest];
        if (!p) {
            p.reset(new uint8_t[kProgramSize]);
            std::memset(p.get(), 0, kProgramSize);
        }
        return p.get();
    }
    uint32_t guestOf(const uint8_t* ptr) const {
        if (!ptr) return 0;
        for (auto& [g, p] : byGuest)
            if (ptr >= p.get() && ptr < p.get() + kProgramSize) return g + static_cast<uint32_t>(ptr - p.get());
        return 0xdeadbeef;
    }
};

struct Forwarder final : VoiceModules {
    Callback cb = nullptr;
    bool muted = true;
    Programs* programs = nullptr;
    LfoParams lfo[Master::kMaxVoices][4] = {};
    MasterDocParams doc[Master::kMaxVoices] = {};

    double io[16] = {};
    double call(int32_t m, int32_t v, int32_t i, std::initializer_list<double> args) {
        int k = 0;
        for (double a : args) io[k++] = a;
        // The original's modules have 16 voice slots: the port's extra slots (more polyphony)
        // only get set-up calls (sample/control rate), which stay local.
        if (!muted && cb && v < kOriginalVoiceSlots) cb(m, v, i, io);
        return io[0];
    }
    int32_t icall(int32_t m, int32_t v, int32_t i, std::initializer_list<double> args) {
        return static_cast<int32_t>(call(m, v, i, args));
    }

    void docReset() override { call(M_DOC_RESET, -1, -1, {}); }
    void docSetNumVoices(int32_t n) override { call(M_DOC_NUMVOICES, -1, -1, {double(n)}); }
    void docSetSampleRate(float sr) override { call(M_DOC_SR, -1, -1, {sr}); }
    void docStartVoice(int v, int32_t key, int32_t oscRestart, int32_t newNote, int32_t stolen) override {
        call(M_DOC_NOTEON, v, -1, {double(key), double(oscRestart), double(newNote), double(stolen)});
    }
    void docSetPitchKey(int v, int32_t key) override { call(M_DOC_SETKEY, v, -1, {double(key)}); }
    // (port) MTS-ESP has no counterpart in the original, and the master only calls this when a
    // tuning snapshot is set, which the differential tests never do. Nothing to record.
    int32_t docRetune(int, const Tuning&, int32_t, int32_t) override { return 0; }
    void docStopVoice(int v) override { call(M_DOC_STOP, v, -1, {}); }
    void docInterpolateLevels(int v) override { call(M_DOC_TICKODD, v, -1, {}); }
    MasterDocParams& docVoiceParams(int v) override { return doc[v]; }
    void docUpdate(int v) override { call(M_DOC_UPDATE, v, -1, {}); }
    double docRender(int v) override { return call(M_DOC_RENDER, v, -1, {0.0}); }
    int32_t docReleaseSamples() override { return icall(M_DOC_TAIL, -1, -1, {0.0}); }

    void lfoSetControlRate(int v, int l, float rate) override { call(M_LFO_RATE, v, l, {rate}); }
    LfoParams& lfoParams(int v, int l) override { return lfo[v][l]; }
    void lfoStart(int v, int l, int32_t phase, bool reset) override {
        call(M_LFO_START, v, l, {double(phase), reset ? 1.0 : 0.0});
    }
    int32_t lfoTick(int v, int l) override { return icall(M_LFO_TICK, v, l, {0.0}); }

    void envSetControlRate(int v, int e, float rate) override { call(M_ENV_RATE, v, e, {rate}); }
    void envStart(int v, int e, const uint8_t* block, int32_t key, int32_t vel, bool restart, bool cycle,
                  int32_t other) override {
        call(M_ENV_START, v, e,
             {double(programs->guestOf(block)), double(key), double(vel), restart ? 1.0 : 0.0, cycle ? 1.0 : 0.0,
              double(other)});
    }
    void envRelease(int v, int e, bool sustain) override { call(M_ENV_RELEASE, v, e, {sustain ? 1.0 : 0.0}); }
    int32_t envTick(int v, int e, bool sustain) override { return icall(M_ENV_TICK, v, e, {sustain ? 1.0 : 0.0}); }
    int32_t envShaped(int v, int e) override { return icall(M_ENV_LEVEL, v, e, {0.0}); }
    bool envActive(int v, int e) override { return icall(M_ENV_ACTIVE, v, e, {0.0}) != 0; }

    void ampSetSampleRate(int v, float sr) override { call(M_AMP_SR, v, -1, {sr}); }
    void ampSetSaturation(int v, int32_t a) override { call(M_AMP_DRIVE, v, -1, {double(a)}); }
    void ampStart(int v, int32_t other) override { call(M_AMP_START, v, -1, {double(other)}); }
    void ampSetRampTime(int v, float s) override { call(M_AMP_SMOOTH, v, -1, {s}); }
    void ampSetSmoothing(int v, int32_t c) override { call(M_AMP_CURVE, v, -1, {double(c)}); }
    void ampSetLevelPan(int v, int32_t level, int32_t pan, float gain, bool flag) override {
        call(M_AMP_LEVEL, v, -1, {double(level), double(pan), gain, flag ? 1.0 : 0.0});
    }
    void ampProcess(int v, double x, float* acc) override {
        call(M_AMP_PROCESS, v, -1, {x, acc[0], acc[1]});
        acc[0] = static_cast<float>(io[1]);
        acc[1] = static_cast<float>(io[2]);
    }
    void ampProcessFaded(int v, double x, float* acc, float gain) override {
        call(M_AMP_FADED, v, -1, {x, acc[0], acc[1], gain});
        acc[0] = static_cast<float>(io[1]);
        acc[1] = static_cast<float>(io[2]);
    }
    int32_t ampRampCount(int v) override { return icall(M_AMP_COUNTER, v, -1, {0.0}); }

    void follSetControlRate(int v, float rate) override { call(M_FOLL_RATE, v, -1, {rate}); }
    void follSetSpeed(int v, float t) override { call(M_FOLL_TIME, v, -1, {t}); }
    void follSetTarget(int v, int32_t t) override { call(M_FOLL_TARGET, v, -1, {double(t)}); }
    void follReset(int v, int32_t x) override { call(M_FOLL_VALUE, v, -1, {double(x)}); }
    int32_t follTick(int v) override { return icall(M_FOLL_TICK, v, -1, {0.0}); }
};

struct Box {
    Programs programs;
    Forwarder fwd;
    std::unique_ptr<Master> master;
    uint32_t guestBase = 0;  // address of the original master object (for pointer fields)
};

template <typename T>
void get(const uint8_t* src, uint32_t off, T& v) { std::memcpy(&v, src + off, sizeof(T)); }
template <typename T>
void put(uint8_t* dst, uint32_t off, const T& v) { std::memcpy(dst + off, &v, sizeof(T)); }

// Field map of CfilterSQ (excluding vmt and the table pointer), as in capi.cpp.
template <typename F, typename Obj, typename Bytes>
void filterFields(F&& f, Obj& o, Bytes b) {
    f(b, 0x04, o.sampleRate);
    f(b, 0x08, o.mode);
    f(b, 0x0c, o.cutoff);
    f(b, 0x10, o.reso);
    f(b, 0x14, o.level);
    f(b, 0x18, o.gain);
    f(b, 0x1c, o.keyParam);
    for (int i = 0; i < 5; i++) f(b, 0x28 + 4 * i, o.stage[i]);
    f(b, 0x3c, o.g);
    f(b, 0x40, o.k);
    f(b, 0x44, o.p);
    f(b, 0x48, o.pA);
    f(b, 0x4c, o.pB);
    f(b, 0x50, o.kA);
    f(b, 0x54, o.kB);
    f(b, 0x58, o.gainA);
    f(b, 0x5c, o.gainB);
    f(b, 0x60, o.kTarget);
    f(b, 0x64, o.pTarget);
    f(b, 0x68, o.gainTarget);
    f(b, 0x6c, o.resoComp);
    f(b, 0x70, o.half);
    f(b, 0x74, o.sixth);
    f(b, 0x78, o.smoothTime);
}

struct MasterTestAccess {
    static uint32_t voiceGuest(Box* b, const Voice* v, const Master& m) {
        return v ? b->guestBase + 4 + 0xe8 * static_cast<uint32_t>(v - m.voices_) : 0;
    }
    static Voice* voiceHost(Box* b, uint32_t guest, Master& m) {
        if (!guest) return nullptr;
        const uint32_t off = guest - b->guestBase - 4;
        return &m.voices_[off / 0xe8];
    }

    // Plain (non-pointer) fields of the voice record.
    template <typename F, typename V, typename Bytes>
    static void voiceFields(F&& f, V& v, Bytes b, uint32_t o) {
        f(b, o + 0x00, v.active);
        f(b, o + 0x04, v.age);
        f(b, o + 0x08, v.ctrlCountdown);
        f(b, o + 0x0c, v.tickParity);
        f(b, o + 0x10, v.slot);
        f(b, o + 0x14, v.key);
        f(b, o + 0x18, v.released);
        f(b, o + 0x1c, v.mono);
        f(b, o + 0x20, v.noteId);
        f(b, o + 0x2c, v.stealFlag);
        f(b, o + 0x30, v.fading);
        f(b, o + 0x34, v.fadeKill);
        f(b, o + 0x38, v.fadeRemaining);
        f(b, o + 0x3c, v.fadeGain);
        f(b, o + 0x40, v.fadeStep);
        f(b, o + 0x44, v.fadeOutLength);
        f(b, o + 0x48, v.fadeOutStep);
        f(b, o + 0x4c, v.fadeInLength);
        f(b, o + 0x50, v.fadeInStep);
        f(b, o + 0x80, v.dca4Mode);
        f(b, o + 0x84, v.glideActive);
        f(b, o + 0x88, v.glideFrom);
        f(b, o + 0x90, v.bend);
        f(b, o + 0x94, v.bendActive);
        for (int i = 0; i < 16; i++) f(b, o + 0x98 + 4 * i, v.mod[i]);
        f(b, o + 0xd8, v.noteLevel);
        f(b, o + 0xdc, v.panOffset);
        f(b, o + 0xe0, v.tickCount);
        f(b, o + 0xe4, v.fe4);
    }

    template <typename F, typename M, typename Bytes>
    static void masterFields(F&& f, M& m, Bytes b) {
        f(b, 0xe84, m.numVoices_);
        f(b, 0xf08, m.listCount_);
        for (int i = 0; i < 16; i++) f(b, 0xe8c + 8 * i, m.list_[i].slot);
        for (int i = 0; i < 16; i++) f(b, 0xf0c + 4 * i, m.slotMap_[i]);
        f(b, 0xf4c, m.numPlay_);
        f(b, 0xf50, m.numFade_);
        for (int i = 0; i < 8; i++) f(b, 0xf54 + 2 * i, m.noteStacks_[i]);
        f(b, 0xf64, m.volume_);
        f(b, 0xf68, m.muffleVoice_);
        f(b, 0xf6c, m.muffleAge_);
        f(b, 0xf70, m.sampleRate_);
        f(b, 0xf74, m.invSampleRate_);
        f(b, 0xf78, m.sampleRateMs_);
        f(b, 0xf7c, m.controlRate_);
        f(b, 0xf80, m.controlStep_);
        f(b, 0xf84, m.initialDelay_);
        f(b, 0xf88, m.error_);
        f(b, 0xf8c, m.lockCount_);
        f(b, 0xf94, m.muffleOn_);
        f(b, 0xf98, m.muffleB0_);
        f(b, 0xf9c, m.muffleB1_);
        f(b, 0xfa0, m.muffleB2_);
        f(b, 0xfa4, m.muffleA1_);
        f(b, 0xfa8, m.muffleA2_);
        for (int i = 0; i < 8; i++) f(b, 0xfac + 4 * i, m.muffleState_[i]);
        f(b, 0xfe4, m.ovrVoiceSteal_);
        f(b, 0xfe8, m.ovrMuffle_);
        f(b, 0xfec, m.ovrDcaSmooth_);
        f(b, 0xff0, m.ovrDca4Smooth_);
        f(b, 0xff4, m.ovrDcBlock_);
        f(b, 0x11004, m.tickToggle_);
        f(b, 0x11008, m.smoothB_);
        f(b, 0x1100c, m.smoothA_);
        for (int i = 0; i < 131; i++) f(b, 0x11010 + 16 * i, m.ctrl_[i]);
        for (int i = 0; i < 128; i++) f(b, 0x11840 + 16 * i, m.polyPressure_[i]);
        f(b, 0x12040, m.pitchBend_);
        f(b, 0x12050, m.inProcess_);
    }

    static void load(Box* b, const uint8_t* bytes) {
        Master& m = *b->master;
        auto getter = [](const uint8_t* src, uint32_t off, auto& v) { get(src, off, v); };
        masterFields(getter, m, bytes);
        for (int i = 0; i < 16; i++) {
            const uint32_t o = 4 + 0xe8 * i;
            Voice& v = m.voices_[i];
            voiceFields(getter, v, bytes, o);
            uint32_t g;
            get(bytes, o + 0x24, g);
            v.program = b->programs.get(g);
            get(bytes, o + 0x28, g);
            v.stolenFrom = voiceHost(b, g, m);
            get(bytes, 0xe88 + 8 * i, g);
            m.list_[i].voice = voiceHost(b, g, m);
        }
        std::memcpy(&m.filterTable_, bytes + 0x1004, sizeof(FilterTable));
    }

    static void save(Box* b, uint8_t* bytes) {
        Master& m = *b->master;
        auto putter = [](uint8_t* dst, uint32_t off, auto& v) { put(dst, off, v); };
        masterFields(putter, m, bytes);
        for (int i = 0; i < 16; i++) {
            const uint32_t o = 4 + 0xe8 * i;
            Voice& v = m.voices_[i];
            voiceFields(putter, v, bytes, o);
            put(bytes, o + 0x24, b->programs.guestOf(v.program));
            put(bytes, o + 0x28, voiceGuest(b, v.stolenFrom, m));
            put(bytes, 0xe88 + 8 * i, voiceGuest(b, m.list_[i].voice, m));
        }
        std::memcpy(bytes + 0x1004, &m.filterTable_, sizeof(FilterTable));
    }

    static FilterSQ& filter(Box* b, int v, int i) { return b->master->filters_[v][i]; }
    static void controlUpdate(Box* b, int v) { b->master->controlUpdate(b->master->voices_[v]); }
    static bool processVoice(Box* b, int v, float* acc) {
        return b->master->processVoice(b->master->voices_[v], acc);
    }
    static void reset(Box* b) { b->master->reset(); }
    static void setMuffle(Box* b, int32_t mode) { b->master->setMuffle(mode); }
    static void computeMuffle(Box* b) { b->master->computeMuffle(); }
    static void setNumVoices(Box* b, int32_t n) { b->master->setNumVoices(n); }
};

}  // namespace sq8l

using sq8l::Box;
using sq8l::MasterTestAccess;

namespace {
struct Mode {
    explicit Mode(int32_t rz) : saved_(std::fegetround()) { std::fesetround(rz ? FE_TOWARDZERO : FE_TONEAREST); }
    ~Mode() { std::fesetround(saved_); }
    int saved_;
};
}  // namespace

// Creates the master. The constructor's module calls go to the callback only when
// `record` is set (otherwise they are dropped).
SQ8L_API void* sq8l_master_new(sq8l::Callback cb, int32_t sampleRate, int32_t record, const int32_t* overrides) {
    auto* b = new Box;
    b->fwd.cb = cb;
    b->fwd.programs = &b->programs;
    b->fwd.muted = record == 0;
    {
        sq8l::RoundToNearest rn;
        b->master.reset(new sq8l::Master(b->fwd, sampleRate, overrides));
    }
    b->fwd.muted = false;
    return b;
}

SQ8L_API void sq8l_master_free(void* b) { delete static_cast<Box*>(b); }

SQ8L_API void sq8l_master_set_guest_base(void* b, uint32_t base) { static_cast<Box*>(b)->guestBase = base; }

SQ8L_API void sq8l_master_set_program(void* b, uint32_t guest, const uint8_t* bytes) {
    std::memcpy(static_cast<Box*>(b)->programs.get(guest), bytes, sq8l::kProgramSize);
}

SQ8L_API void sq8l_master_set_current(void* b, uint32_t guest, int32_t number) {
    auto* box = static_cast<Box*>(b);
    box->master->setCurrentProgram(box->programs.get(guest), static_cast<uint16_t>(number));
}

SQ8L_API void sq8l_master_load(void* b, const uint8_t* bytes) { MasterTestAccess::load(static_cast<Box*>(b), bytes); }
SQ8L_API void sq8l_master_save(void* b, uint8_t* bytes) { MasterTestAccess::save(static_cast<Box*>(b), bytes); }

SQ8L_API void sq8l_master_load_filter(void* b, int32_t v, int32_t i, const uint8_t* bytes) {
    sq8l::filterFields([](const uint8_t* src, uint32_t off, auto& x) { sq8l::get(src, off, x); },
                       MasterTestAccess::filter(static_cast<Box*>(b), v, i), bytes);
}
SQ8L_API void sq8l_master_save_filter(void* b, int32_t v, int32_t i, uint8_t* bytes) {
    sq8l::filterFields([](uint8_t* dst, uint32_t off, auto& x) { sq8l::put(dst, off, x); },
                       MasterTestAccess::filter(static_cast<Box*>(b), v, i), bytes);
}

SQ8L_API sq8l::LfoParams* sq8l_master_lfo_params(void* b, int32_t v, int32_t i) {
    return &static_cast<Box*>(b)->fwd.lfo[v][i];
}
SQ8L_API sq8l::MasterDocParams* sq8l_master_doc_params(void* b, int32_t v) {
    return &static_cast<Box*>(b)->fwd.doc[v];
}

// ---- entry points (rz: 1 = round toward zero, 0 = nearest)
SQ8L_API void sq8l_master_note_on(void* b, int32_t key, int32_t vel, int32_t rz) {
    Mode m(rz);
    static_cast<Box*>(b)->master->midiNoteOn(0, static_cast<uint8_t>(key), static_cast<uint8_t>(vel));
}
SQ8L_API void sq8l_master_control(void* b, int32_t ch, int32_t data1, int32_t value, int32_t ctrl, int32_t rz) {
    Mode m(rz);
    static_cast<Box*>(b)->master->midiControl(static_cast<uint8_t>(ch), static_cast<uint8_t>(data1),
                                              static_cast<int16_t>(value), static_cast<int16_t>(ctrl));
}
SQ8L_API void sq8l_master_reset(void* b, int32_t rz) {
    Mode m(rz);
    MasterTestAccess::reset(static_cast<Box*>(b));
}
SQ8L_API void sq8l_master_control_update(void* b, int32_t v, int32_t rz) {
    Mode m(rz);
    MasterTestAccess::controlUpdate(static_cast<Box*>(b), v);
}
SQ8L_API int32_t sq8l_master_process_voice(void* b, int32_t v, float* acc, int32_t rz) {
    Mode m(rz);
    return MasterTestAccess::processVoice(static_cast<Box*>(b), v, acc) ? 1 : 0;
}
SQ8L_API int32_t sq8l_master_set_sample_rate(void* b, int32_t sr, int32_t rz) {
    Mode m(rz);
    return static_cast<Box*>(b)->master->setSampleRateInt(sr);
}
SQ8L_API int32_t sq8l_master_set_sample_rate_f(void* b, float sr, int32_t rz) {
    Mode m(rz);
    return static_cast<Box*>(b)->master->setSampleRate(sr);
}
SQ8L_API void sq8l_master_set_control_rate(void* b, float rate, int32_t rz) {
    Mode m(rz);
    static_cast<Box*>(b)->master->setControlRate(rate);
}
SQ8L_API void sq8l_master_set_voices(void* b, int32_t play, int32_t fade, int32_t rz) {
    Mode m(rz);
    static_cast<Box*>(b)->master->setVoices(play, fade);
}
SQ8L_API void sq8l_master_edit_event(void* b, int32_t code, uint32_t guestProgram, int32_t rz) {
    Mode m(rz);
    auto* box = static_cast<Box*>(b);
    box->master->editBufferEvent(code, guestProgram ? box->programs.get(guestProgram) : nullptr);
}
SQ8L_API void sq8l_master_load_overrides(void* b, const int32_t* ini) {
    static_cast<Box*>(b)->master->loadOverrides(ini);
}
SQ8L_API int32_t sq8l_master_active_count(void* b) { return static_cast<Box*>(b)->master->activeVoiceCount(); }

SQ8L_API void sq8l_master_process_events(void* b, const sq8l::RawMidiEvent* ev, int32_t n) {
    static_cast<Box*>(b)->master->processEvents(ev, n);
}
SQ8L_API void sq8l_master_process(void* b, float* outL, float* outR, int32_t n, int32_t replacing) {
    static_cast<Box*>(b)->master->process(outL, outR, n, replacing != 0);
}
