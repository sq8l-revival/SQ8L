#include "Env.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "Fpu.h"
#include "Program.h"  // (port) tempo sync

namespace sq8l {

namespace {

// 80-bit constant at 0x45dcd8 (83.59257598): mantissa * 2^exponent.
constexpr uint64_t kControlRateMant = 0xa72f661e6cfd8450ull;
constexpr int kControlRateExp = 6 - 63;

// The asm uses 32-bit wrap-around integer arithmetic everywhere; do the same without UB.
inline int32_t add32(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}
inline int32_t sub32(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}
inline int32_t mul32(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}
inline int32_t shl32(int32_t a, int n) { return static_cast<int32_t>(static_cast<uint32_t>(a) << n); }

// "test x,x / jns / add x,(2^n-1) / sar x,n": signed division by 2^n rounding toward zero.
inline int32_t sar8(int32_t x) { return x / 256; }
inline int32_t sar9(int32_t x) { return x / 512; }

// Delphi IDIV: truncating; the original faults on /0 and INT_MIN/-1 (see docs/modules/env.md).
inline int32_t idiv32(int32_t a, int32_t b) {
    if (b == -1) return sub32(0, a);
    return a / b;
}

// x87 FISTP (Delphi Round, 0x4175c0): current rounding mode, integer indefinite when out of range.
inline int32_t fistp32(double x) {
    const double r = std::nearbyint(x);
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) return INT32_MIN;
    return static_cast<int32_t>(r);
}

// x87 at 53-bit precision, "FLD tbyte c; FDIVR x": x / c rounded once to double in the current
// rounding mode, for c = mant * 2^exp (mant normalized). Exact long division plus sticky bit.
double divByExtended(double x, uint64_t cMant, int cExp) {
    if (!(x > 0) || !std::isfinite(x)) return x / std::ldexp(static_cast<double>(cMant), cExp);
    int ex = 0;
    const double fr = std::frexp(x, &ex);  // x = fr * 2^ex, fr in [0.5, 1)
    const uint64_t mx = static_cast<uint64_t>(std::ldexp(fr, 53));  // in [2^52, 2^53)
    // N = mx * 2^67 (120 bits); q = N / cMant lies in (2^55, 2^57).
    uint64_t q = 0, rem = 0;
    for (int i = 119; i >= 0; --i) {
        const uint64_t bit = i >= 67 ? (mx >> (i - 67)) & 1u : 0u;
        const bool carry = (rem >> 63) != 0;
        rem = (rem << 1) | bit;
        q <<= 1;
        if (carry || rem >= cMant) {
            rem -= cMant;
            q |= 1;
        }
    }
    const int64_t qs = static_cast<int64_t>(q | (rem != 0 ? 1u : 0u));  // sticky bit
    // int64 -> double rounds to 53 bits in the current mode; the scaling is exact.
    return std::ldexp(static_cast<double>(qs), ex - 53 - 67 - cExp);
}

// FUN_00450d90: clamp to 0..127.
inline int32_t clamp127(int32_t v) { return v < 1 ? 0 : (v < 127 ? v : 127); }

}  // namespace

void Env::init(float controlRate) {
    // TObject.Create zero-fills the instance.
    *this = Env{};
    setRate(controlRate);
}

void Env::setRate(float controlRate) {
    rate = controlRate;
    rateScale = sgl(divByExtended(static_cast<double>(controlRate), kControlRateMant, kControlRateExp));
    reset();
}

void Env::reset() {
    ramp = Ramp::Idle;
    next = Next::Decay;
    active = 0;
    sustaining = 0;
    released = 0;
    releasePending = 0;
    finished = 0;
    for (int32_t& l : level) l = 0;
    for (int32_t& t : time) t = 0;
    levelScale = 0;
    timeKeyScale = 0;
    secondRelease = 0;
    value = 0;
    target = 0;
    step = 0;
    ticksLeft = 0;
    smoothing = 0;
    smoothed = 0;
    output = 0;
}

void Env::setSmoothing(int32_t amount) {
    if (amount <= 0) {
        smoothing = 0;  // smoothA/smoothB keep their old values
        return;
    }
    if (amount >= 256) amount = 255;
    const uint32_t d = static_cast<uint32_t>(255 - amount);
    const int32_t a = 255 - static_cast<int32_t>((d * d) >> 8);
    smoothing = a;
    smoothA = a;
    smoothB = 256 - a;
}

void Env::start(const uint8_t* rec, int32_t key, int32_t velocity, uint8_t restart, uint8_t cycleFlag,
                const Env* prev) {
    const int32_t vel = clamp127(velocity);
    cycle = cycleFlag;
    for (int i = 0; i < 5; i++) {
        const int32_t c = static_cast<int8_t>(rec[i]);
        level[i] = c < 0 ? (c - 1) * 2 : c * 2;
    }
    for (int i = 0; i < 4; i++) time[i] = rec[EnvRecord::kT1 + i] & 0x3f;

    int32_t k = shl32(sub32(key, 0x24), 1);
    if (k < 0) k = 0;
    timeKeyScale = sar8(mul32(static_cast<int8_t>(rec[EnvRecord::kTK]), k));
    secondRelease = static_cast<int8_t>(rec[EnvRecord::kT4]) >= 0x40 ? 1 : 0;

    const int32_t lv = static_cast<int8_t>(rec[EnvRecord::kLV]);
    const uint32_t curve = lv < 0x40 ? data::kVelocityCurveLinear[vel] : data::kVelocityCurveExp[vel];
    levelScale = add32(static_cast<int32_t>((static_cast<uint32_t>(lv) * curve) >> 5), shl32(0x3f - lv, 2));

    shape = static_cast<int32_t>((rec[EnvRecord::kFlags] >> 1) & 7) - 1;
    shapeTable = (shape >= 0 && shape < 8) ? data::kEnvShapeTables + 256 * shape : nullptr;

    // T1V: shortens T1 (MODE-T1V = T1) or reduces the smoothing (MODE-T1V = SMT).
    const uint32_t t1vVel = static_cast<uint32_t>(mul32(static_cast<int8_t>(rec[EnvRecord::kT1V]), vel));
    int32_t t1Offset = 0;
    if ((rec[EnvRecord::kFlags] & 0x10) == 0) {
        t1Offset = static_cast<int32_t>(t1vVel >> 7);
        setSmoothing(static_cast<int32_t>(rec[EnvRecord::kSMTH]) << 2);
    } else {
        setSmoothing(sub32(static_cast<int32_t>(rec[EnvRecord::kSMTH]) << 2, static_cast<int32_t>(t1vVel >> 5)));
    }

    ramp = Ramp::Idle;
    next = Next::Decay;
    smoothed = 0;
    output = 0;
    if (restart != 0) {
        value = shl32(level[0], 8);
        smoothed = shl32(level[0], 8);
        output = level[0];
    } else if (prev != nullptr) {
        // prev may be this very envelope (same voice restruck): smoothed/output were just zeroed.
        value = prev->value;
        smoothed = prev->smoothed;
        output = prev->output;
    }
    active = 1;
    sustaining = 0;
    released = 0;
    releasePending = 0;
    finished = 0;
    // (port) a synced T1 is exactly its note value: T1V (T1 mode) and TK do not apply
    sync = (rec[EnvRecord::kFlags] & ofs::Env::SyncBit) != 0 ? 1 : 0;
    if (sync != 0) t1Offset = 0;
    startSegment(sar8(mul32(level[1], levelScale)), sub32(time[0], t1Offset));
}

void Env::startSegment(int32_t tgt, int32_t timeIndex, bool fixedTime) {
    target = tgt;
    if (timeIndex < 0) timeIndex = 0;
    else if (timeIndex > 63) timeIndex = 63;
    int32_t n;
    if (sync != 0 && !fixedTime) {
        // (port) tempo sync; a time past the last note value plays the last
        const int32_t d = kSyncDivisions - 1 - std::min(timeIndex, kSyncDivisions - 1);
        n = static_cast<int32_t>(std::lround(kSyncQuarters[d] * syncTicksPerQuarter));
    } else {
        // FILD word; FMUL single [+0x7c]; FSTP single; Round()
        const float ticks = sgl(static_cast<double>(data::kEnvTimeTable[timeIndex]) * rateScale);
        n = fistp32(ticks);
    }
    ticksLeft = n > 0 ? n : 1;
    int32_t diff = sub32(tgt, sar8(value));
    if (diff == 0) {
        ramp = Ramp::Hold;
    } else if (diff > 0) {
        ramp = Ramp::Rise;
    } else {
        ramp = Ramp::Fall;
        diff = sub32(0, diff);
    }
    step = idiv32(shl32(diff, 8), ticksLeft);
}

void Env::runRamp() {
    // The original checks Assigned(+0x68) first (nil only before construction completes).
    switch (ramp) {
    case Ramp::None:
    case Ramp::Idle:
        return;
    case Ramp::Rise:
        value = add32(value, step);
        if (sar8(value) > target) value = add32(shl32(target, 8), value & 0xff);
        rampEnd();
        return;
    case Ramp::Fall:
        value = sub32(value, step);
        if (sar8(value) < target) value = add32(shl32(target, 8), value & 0xff);
        rampEnd();
        return;
    case Ramp::Hold:
        rampEnd();
        return;
    }
}

void Env::rampEnd() {
    ticksLeft = sub32(ticksLeft, 1);
    if (ticksLeft > 0) return;
    value = add32(shl32(target, 8), value & 0xff);
    runNext();
}

void Env::runNext() {
    switch (next) {
    case Next::None:  // Assigned(+0x70) is false
        return;
    case Next::Decay: {  // 0x45e0c0
        next = Next::Decay2;
        int32_t t = sub32(time[1], sync != 0 ? 0 : timeKeyScale);  // (port) synced: no TK
        if (t < 0) t = 0;
        startSegment(sar8(mul32(level[2], levelScale)), t);
        return;
    }
    case Next::Decay2: {  // 0x45e0f8
        next = Next::Sustain;
        int32_t t = sub32(time[2], sync != 0 ? 0 : timeKeyScale);
        if (t < 0) t = 0;
        startSegment(sar8(mul32(level[3], levelScale)), t);
        return;
    }
    case Next::Sustain:  // 0x45e130
        if (value == 0) {
            finish();
        } else if (released != 0 || cycle != 0) {
            beginRelease();
        } else {
            sustaining = 1;
            ramp = Ramp::Idle;
        }
        return;
    case Next::SecondRelease:  // 0x45e160
        sustaining = 0;
        next = Next::Finish;
        startSegment(0, 0x27, true);
        return;
    case Next::Finish:  // 0x45e17c
        finish();
        return;
    }
}

void Env::finish() {
    ramp = Ramp::Idle;
    finished = 1;
    sustaining = 0;
}

void Env::beginRelease() {
    if (secondRelease != 0) {
        // "R": first fall to half the current level minus 12 in T4, then to 0 in time 39.
        next = Next::SecondRelease;
        startSegment(sub32(sar9(value), 0xc), time[3]);
        return;
    }
    next = Next::Finish;
    startSegment(0, time[3]);
}

void Env::release(bool pedalHeld) {
    if (cycle != 0) return;
    if (!pedalHeld) {
        released = 1;
        releasePending = 0;
        beginRelease();
    } else {
        releasePending = 1;
    }
}

int32_t Env::tick(bool pedalHeld) {
    if (active == 0) return 0;
    if (!pedalHeld && releasePending != 0) release(false);
    runRamp();
    int32_t out;
    if (smoothing > 0) {
        smoothed = sar8(add32(mul32(smoothed, smoothA), mul32(value, smoothB)));
        if (finished != 0 && sar8(smoothed) == 0) {
            active = 0;
            finished = 0;
            out = 0;
        } else {
            out = sar8(smoothed);
        }
    } else if (finished == 0) {
        out = sar8(value);
    } else {
        active = 0;
        finished = 0;
        out = 0;
    }
    output = out;
    if (shapeTable != nullptr) out = shaped();
    return out;
}

int32_t Env::shaped() const {
    if (shapeTable == nullptr) return output;
    // The original raises a divide fault when levelScale == 0 (LV >= 64 at some velocities);
    // the product below is 0 for any table entry then, so return 0.
    if (levelScale == 0) return 0;
    const int32_t idx = add32(idiv32(shl32(output, 8), levelScale), 0x80) & 0xff;
    return sar8(mul32(shapeTable[idx], levelScale));
}

}  // namespace sq8l
