// C API: final amplifier (sq8l::Amp, original Camp) for tests/test_amp.py.
//
// Objects are loaded from / saved to the byte layout of the original Delphi object
// (0x74 bytes, VMT excluded) so captured states can be replayed.
#include <cstdint>
#include <cstring>

#include "Amp.h"
#include "Fpu.h"

#include "capi_export.h"

namespace sq8l {

// Gives the tests access to the internal helpers, which are traced separately.
struct AmpTestAccess {
    static void updateLevel(Amp& a, int32_t level, int32_t pan, bool imm) { a.updateLevel(level, pan, imm); }
    static void setRampLength(Amp& a, int32_t n) { a.setRampLength(n); }
    static void refreshRampLength(Amp& a) { a.refreshRampLength(); }
    static void setConstants(Amp& a) { a.setConstants(); }
};

}  // namespace sq8l

namespace {

using sq8l::Amp;
using sq8l::AmpTestAccess;

template <typename T>
void get(const uint8_t* src, uint32_t off, T& v) { std::memcpy(&v, src + off, sizeof(T)); }
template <typename T>
void put(uint8_t* dst, uint32_t off, const T& v) { std::memcpy(dst + off, &v, sizeof(T)); }

template <typename F, typename Obj, typename Bytes>
void ampFields(F&& f, Obj& o, Bytes b) {
    f(b, 0x04, o.volL);
    f(b, 0x08, o.volR);
    f(b, 0x0c, o.stepL);
    f(b, 0x10, o.stepR);
    f(b, 0x14, o.rampCount);
    f(b, 0x18, o.satIndex);
    f(b, 0x1c, o.satThreshold);
    f(b, 0x20, o.satCubic);
    f(b, 0x24, o.satLinear);
    f(b, 0x28, o.satGain);
    f(b, 0x2c, o.clip[0]);
    f(b, 0x30, o.clip[1]);
    f(b, 0x34, o.sqrt2);
    f(b, 0x38, o.satScale);
    f(b, 0x3c, o.sixth);
    f(b, 0x40, o.targetL);
    f(b, 0x44, o.targetR);
    f(b, 0x48, o.rampLength);
    f(b, 0x4c, o.rampStep);
    f(b, 0x50, o.level);
    f(b, 0x54, o.pan);
    f(b, 0x58, o.smoothing);
    f(b, 0x5c, o.levelAcc);
    f(b, 0x60, o.smoothA);
    f(b, 0x64, o.smoothB);
    f(b, 0x68, o.rampTime);
    f(b, 0x6c, o.gain);
    f(b, 0x70, o.sampleRate);
}

Amp& A(void* p) { return *static_cast<Amp*>(p); }

// Runs fn under the rounding mode the original ran in (x87 CW 0xC00 bits).
template <typename Fn>
void inMode(int32_t towardZero, Fn&& fn) {
    if (towardZero) {
        sq8l::RoundTowardZero rz;
        fn();
    } else {
        sq8l::RoundToNearest rn;
        fn();
    }
}

}  // namespace

SQ8L_API const float* sq8l_amp_tables() { return &sq8l::ampTables().pan[0][0]; }
SQ8L_API const float* sq8l_amp_shape() { return sq8l::data::kAmpShape; }

SQ8L_API void* sq8l_amp_new() { return new Amp; }
SQ8L_API void sq8l_amp_free(void* a) { delete static_cast<Amp*>(a); }

SQ8L_API void sq8l_amp_load(void* a, const uint8_t* orig) {
    ampFields([](const uint8_t* b, uint32_t off, auto& v) { get(b, off, v); }, A(a), orig);
}

SQ8L_API void sq8l_amp_save(void* a, uint8_t* orig) {
    ampFields([](uint8_t* b, uint32_t off, auto& v) { put(b, off, v); }, A(a), orig);
}

SQ8L_API void sq8l_amp_init(void* a) {
    sq8l::RoundToNearest rn;
    A(a).init();
}

SQ8L_API void sq8l_amp_process(void* a, double x, float* out) {
    sq8l::RoundTowardZero rz;
    A(a).process(x, out);
}

SQ8L_API void sq8l_amp_process_fade(void* a, double x, float* out, float fade) {
    sq8l::RoundTowardZero rz;
    A(a).processFade(x, out, fade);
}

SQ8L_API void sq8l_amp_set_level_pan(void* a, int32_t level, int32_t pan, float gain, int32_t imm, int32_t tz) {
    inMode(tz, [&] { A(a).setLevelPan(level, pan, gain, imm != 0); });
}

SQ8L_API void sq8l_amp_set_level_pan_unity(void* a, int32_t level, int32_t pan, int32_t imm, int32_t tz) {
    inMode(tz, [&] { A(a).setLevelPan(level, pan, imm != 0); });
}

SQ8L_API void sq8l_amp_start(void* a, const void* from, int32_t tz) {
    inMode(tz, [&] { A(a).start(static_cast<const Amp*>(from)); });
}

SQ8L_API void sq8l_amp_set_saturation(void* a, int32_t sat, int32_t tz) {
    inMode(tz, [&] { A(a).setSaturation(sat); });
}

SQ8L_API void sq8l_amp_set_smoothing(void* a, int32_t amount) { A(a).setSmoothing(amount); }

SQ8L_API void sq8l_amp_set_ramp_time(void* a, float t, int32_t tz) {
    inMode(tz, [&] { A(a).setRampTime(t); });
}

SQ8L_API void sq8l_amp_set_sample_rate(void* a, float sr, int32_t tz) {
    inMode(tz, [&] { A(a).setSampleRate(sr); });
}

SQ8L_API void sq8l_amp_update_level(void* a, int32_t level, int32_t pan, int32_t imm) {
    AmpTestAccess::updateLevel(A(a), level, pan, imm != 0);
}

SQ8L_API void sq8l_amp_set_ramp_length(void* a, int32_t n, int32_t tz) {
    inMode(tz, [&] { AmpTestAccess::setRampLength(A(a), n); });
}

SQ8L_API void sq8l_amp_refresh_ramp_length(void* a, int32_t tz) {
    inMode(tz, [&] { AmpTestAccess::refreshRampLength(A(a)); });
}

SQ8L_API void sq8l_amp_set_constants(void* a) { AmpTestAccess::setConstants(A(a)); }
