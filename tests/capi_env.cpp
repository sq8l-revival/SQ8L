// C API for the differential tests of the envelope (Csq_env) and mod follower (Cmod_foll).
//
// Objects are loaded from / saved to the byte layout of the original Delphi objects. Pointer
// fields are mapped: the envelope's method pointers (+0x68/+0x70 code, +0x6c/+0x74 self) to
// enums, the shape table pointer (+0x5c) to the original table address 0x4c1688.
#include <cfenv>
#include <cstdint>
#include <cstring>

#include "Env.h"
#include "ModFollower.h"

#include "capi_export.h"

namespace {

template <typename T>
void get(const uint8_t* src, uint32_t off, T& v) { std::memcpy(&v, src + off, sizeof(T)); }
template <typename T>
void put(uint8_t* dst, uint32_t off, const T& v) { std::memcpy(dst + off, &v, sizeof(T)); }

// x87 control word rounding control (bits 10..11) -> C rounding mode.
class RoundingScope {
public:
    explicit RoundingScope(int rc) : saved_(std::fegetround()) {
        static const int modes[4] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
        std::fesetround(modes[rc & 3]);
    }
    ~RoundingScope() { std::fesetround(saved_); }
    RoundingScope(const RoundingScope&) = delete;
    RoundingScope& operator=(const RoundingScope&) = delete;

private:
    int saved_;
};

constexpr uint32_t kShapeTablesAddr = 0x4c1688;

// Indexed by Env::Ramp / Env::Next (0 = nil method pointer).
constexpr uint32_t kRampCode[] = {0, 0x45e0bc, 0x45dfd4, 0x45e030, 0x45e08c};
constexpr uint32_t kNextCode[] = {0, 0x45e0c0, 0x45e0f8, 0x45e130, 0x45e160, 0x45e17c};

// Plain fields of Csq_env (pointer fields handled separately).
template <typename F, typename Obj, typename Bytes>
void envFields(F&& f, Obj& o, Bytes b) {
    for (int i = 0; i < 5; i++) f(b, 0x04 + 4 * i, o.level[i]);
    for (int i = 0; i < 4; i++) f(b, 0x18 + 4 * i, o.time[i]);
    f(b, 0x28, o.levelScale);
    f(b, 0x2c, o.timeKeyScale);
    f(b, 0x30, o.secondRelease);
    f(b, 0x31, o.cycle);
    f(b, 0x34, o.value);
    f(b, 0x38, o.target);
    f(b, 0x3c, o.step);
    f(b, 0x40, o.ticksLeft);
    f(b, 0x44, o.output);
    f(b, 0x48, o.smoothing);
    f(b, 0x4c, o.smoothed);
    f(b, 0x50, o.smoothA);
    f(b, 0x54, o.smoothB);
    f(b, 0x58, o.shape);
    f(b, 0x60, o.active);
    f(b, 0x61, o.sustaining);
    f(b, 0x62, o.released);
    f(b, 0x63, o.releasePending);
    f(b, 0x64, o.finished);
    f(b, 0x78, o.rate);
    f(b, 0x7c, o.rateScale);
}

template <typename F, typename Obj, typename Bytes>
void follFields(F&& f, Obj& o, Bytes b) {
    f(b, 0x04, o.rate);
    f(b, 0x08, o.speed);
    f(b, 0x0c, o.stepScale);
    f(b, 0x10, o.stepSize);
    f(b, 0x14, o.value);
    f(b, 0x18, o.step);
    f(b, 0x1c, o.target);
}

sq8l::Env* env(void* p) { return static_cast<sq8l::Env*>(p); }
sq8l::ModFollower* foll(void* p) { return static_cast<sq8l::ModFollower*>(p); }

}  // namespace

// ---------------------------------------------------------------- tables
SQ8L_API const uint16_t* sq8l_env_time_table() { return sq8l::data::kEnvTimeTable; }
SQ8L_API const int8_t* sq8l_env_shape_tables() { return sq8l::data::kEnvShapeTables; }
SQ8L_API const uint8_t* sq8l_velocity_curve(int exponential) {
    return exponential ? sq8l::data::kVelocityCurveExp : sq8l::data::kVelocityCurveLinear;
}

// ---------------------------------------------------------------- envelope
SQ8L_API void* sq8l_env_new() { return new sq8l::Env; }
SQ8L_API void sq8l_env_free(void* e) { delete env(e); }

// Returns 0 on success, a bit mask of unmappable pointer fields otherwise.
SQ8L_API int sq8l_env_load(void* e, const uint8_t* orig, uint32_t addr) {
    sq8l::Env& o = *env(e);
    envFields([](const uint8_t* b, uint32_t off, auto& v) { get(b, off, v); }, o, orig);
    int err = 0;
    uint32_t p = 0;
    get(orig, 0x5c, p);
    if (p == 0) {
        o.shapeTable = nullptr;
    } else if (p >= kShapeTablesAddr && p < kShapeTablesAddr + 8 * 256) {
        o.shapeTable = sq8l::data::kEnvShapeTables + (p - kShapeTablesAddr);
    } else {
        o.shapeTable = nullptr;
        err |= 1;
    }
    uint32_t code = 0, data = 0;
    get(orig, 0x68, code);
    get(orig, 0x6c, data);
    err |= data == (code ? addr : 0) ? 0 : 2;
    err |= 4;
    for (int i = 0; i < 5; i++) {
        if (kRampCode[i] == code) {
            o.ramp = static_cast<sq8l::Env::Ramp>(i);
            err &= ~4;
        }
    }
    get(orig, 0x70, code);
    get(orig, 0x74, data);
    err |= data == (code ? addr : 0) ? 0 : 8;
    err |= 16;
    for (int i = 0; i < 6; i++) {
        if (kNextCode[i] == code) {
            o.next = static_cast<sq8l::Env::Next>(i);
            err &= ~16;
        }
    }
    return err;
}

SQ8L_API void sq8l_env_save(void* e, uint8_t* orig, uint32_t addr) {
    sq8l::Env& o = *env(e);
    envFields([](uint8_t* b, uint32_t off, auto& v) { put(b, off, v); }, o, orig);
    const uint32_t p = o.shapeTable == nullptr
                           ? 0u
                           : kShapeTablesAddr + static_cast<uint32_t>(o.shapeTable - sq8l::data::kEnvShapeTables);
    put(orig, 0x5c, p);
    put(orig, 0x68, kRampCode[static_cast<int>(o.ramp)]);
    put(orig, 0x6c, o.ramp == sq8l::Env::Ramp::None ? 0u : addr);
    put(orig, 0x70, kNextCode[static_cast<int>(o.next)]);
    put(orig, 0x74, o.next == sq8l::Env::Next::None ? 0u : addr);
}

SQ8L_API void sq8l_env_init(void* e, float rate, int rc) {
    RoundingScope r(rc);
    env(e)->init(rate);
}

SQ8L_API void sq8l_env_set_rate(void* e, float rate, int rc) {
    RoundingScope r(rc);
    env(e)->setRate(rate);
}

// prev: another envelope object, this one (same voice restruck) or null.
SQ8L_API void sq8l_env_start(void* e, const uint8_t* rec, int32_t key, int32_t vel, int32_t restart,
                             int32_t cycle, void* prev, int rc) {
    RoundingScope r(rc);
    env(e)->start(rec, key, vel, static_cast<uint8_t>(restart), static_cast<uint8_t>(cycle), env(prev));
}

SQ8L_API void sq8l_env_release(void* e, int32_t pedal, int rc) {
    RoundingScope r(rc);
    env(e)->release(pedal != 0);
}

SQ8L_API int32_t sq8l_env_tick(void* e, int32_t pedal, int rc) {
    RoundingScope r(rc);
    return env(e)->tick(pedal != 0);
}

SQ8L_API int32_t sq8l_env_shaped(void* e) { return env(e)->shaped(); }

// ---------------------------------------------------------------- mod follower
SQ8L_API void* sq8l_foll_new() { return new sq8l::ModFollower; }
SQ8L_API void sq8l_foll_free(void* f) { delete foll(f); }

SQ8L_API void sq8l_foll_load(void* f, const uint8_t* orig) {
    follFields([](const uint8_t* b, uint32_t off, auto& v) { get(b, off, v); }, *foll(f), orig);
}

SQ8L_API void sq8l_foll_save(void* f, uint8_t* orig) {
    follFields([](uint8_t* b, uint32_t off, auto& v) { put(b, off, v); }, *foll(f), orig);
}

SQ8L_API void sq8l_foll_init(void* f, float rate, int rc) {
    RoundingScope r(rc);
    foll(f)->init(rate);
}

SQ8L_API void sq8l_foll_set_rate(void* f, float rate, int rc) {
    RoundingScope r(rc);
    foll(f)->setRate(rate);
}

SQ8L_API void sq8l_foll_set_speed(void* f, float speed, int rc) {
    RoundingScope r(rc);
    foll(f)->setSpeed(speed);
}

SQ8L_API void sq8l_foll_set_target(void* f, int32_t t) { foll(f)->setTarget(t); }
SQ8L_API void sq8l_foll_reset(void* f, int32_t v) { foll(f)->reset(v); }
SQ8L_API int32_t sq8l_foll_tick(void* f) { return foll(f)->tick(); }

SQ8L_API float sq8l_glide_speed(int32_t keyDelta, int32_t glideTime, int rc) {
    RoundingScope r(rc);
    return sq8l::ModFollower::glideSpeed(keyDelta, glideTime);
}
