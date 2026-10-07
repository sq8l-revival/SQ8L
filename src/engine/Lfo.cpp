#include "Lfo.h"

#include <cfenv>
#include <cmath>
#include <cstring>

#include "Fpu.h"

namespace sq8l {

namespace {

// 80-bit constants of the original.
// 83.59257598 at 0x45d0f0 (the "native" control rate): mantissa * 2^-57.
constexpr uint64_t kNativeRateMant = 0xA72F661E6CFD8450ull;
constexpr int kNativeRateExp = -57;
// 8/65 at 0x45d818 (EMU delay steps per tick when the delay is changed mid-note).
constexpr Ext kEmuStepsPerTick = {0x1.f81f81f81f820p-4, -0x1.f800000000000p-58};
// 0.01 at 0x45d0fc (minimum control rate).
constexpr double kMinRate = 0x1.47ae147ae147bp-7;  // > the 80-bit value, no float in between

// Wrapping 32-bit integer arithmetic (x86 semantics).
inline int32_t add32(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); }
inline int32_t sub32(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b)); }
inline int32_t mul32(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b)); }
inline int32_t neg32(int32_t a) { return static_cast<int32_t>(0u - static_cast<uint32_t>(a)); }
inline int32_t shl32(int32_t a, uint32_t n) { return static_cast<int32_t>(static_cast<uint32_t>(a) << (n & 31)); }

// FISTP m32 (Delphi Round via FUN_004175c0): current rounding mode, and the
// "integer indefinite" 0x80000000 when the result does not fit (invalid masked).
inline int32_t fistp32(double x) {
    const double r = std::nearbyint(x);
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) return INT32_MIN;
    return static_cast<int32_t>(r);
}

// Exact division of a 128-bit value by a divisor that fits in 32 bits, as 32-bit limbs
// with 64-bit intermediates: rem < d, so (rem << 32) | limb never overflows. Spelled out
// rather than with unsigned __int128, which cl.exe does not have and for which clang
// calls compiler-rt helpers (__udivti3) that are not linked in MSVC-ABI builds.
struct U128 {
    uint64_t hi, lo;
};

U128 divBy32(U128 n, uint32_t d, bool& remNonZero) {
    const uint32_t limb[4] = {static_cast<uint32_t>(n.lo), static_cast<uint32_t>(n.lo >> 32),
                              static_cast<uint32_t>(n.hi), static_cast<uint32_t>(n.hi >> 32)};
    uint32_t q[4];
    uint64_t rem = 0;
    for (int i = 3; i >= 0; i--) {
        const uint64_t cur = (rem << 32) | limb[i];
        q[i] = static_cast<uint32_t>(cur / d);
        rem = cur % d;
    }
    remNonZero = rem != 0;
    return {(static_cast<uint64_t>(q[3]) << 32) | q[2], (static_cast<uint64_t>(q[1]) << 32) | q[0]};
}

// x87 "FLD tbyte K; FDIV dword r" at 53-bit precision: the exact quotient of the
// 80-bit constant K = mant * 2^exp by a float, rounded once to a double with the
// current rounding mode. (Computing hi/r + lo/r in double could round differently.)
double divExtByFloat(uint64_t mant, int exp, float r) {
    if (std::isnan(r)) return r;
    if (std::isinf(r)) return std::signbit(r) ? -0.0 : 0.0;
    if (r == 0) return std::signbit(r) ? -HUGE_VAL : HUGE_VAL;
    int e;
    const double f = std::frexp(static_cast<double>(std::fabs(r)), &e);  // |r| = f * 2^e
    const uint64_t rm = static_cast<uint64_t>(std::ldexp(f, 24));       // 24-bit integer
    const int rexp = e - 24;
    const U128 num = {mant >> 24, mant << 40};  // mant * 2^40, 104 bits
    bool remNonZero = false;
    const U128 q = divBy32(num, static_cast<uint32_t>(rm), remNonZero);
    int bits = 0;
    for (uint64_t t = q.hi; t; t >>= 1) bits++;
    if (bits)
        bits += 64;
    else
        for (uint64_t t = q.lo; t; t >>= 1) bits++;
    const int drop = bits - 53;  // mant has bit 63 set, so q is in (2^79, 2^81) and drop is 27 or 28
    const uint64_t low = q.lo & ((UINT64_C(1) << drop) - 1);
    const uint64_t half = UINT64_C(1) << (drop - 1);
    uint64_t m = (q.hi << (64 - drop)) | (q.lo >> drop);
    const bool negative = std::signbit(r);
    const bool inexact = low != 0 || remNonZero;
    switch (std::fegetround()) {
    case FE_TONEAREST:
        if (low > half || (low == half && (remNonZero || (m & 1)))) m++;
        break;
    case FE_UPWARD:
        if (inexact && !negative) m++;
        break;
    case FE_DOWNWARD:
        if (inexact && negative) m++;
        break;
    default:  // FE_TOWARDZERO
        break;
    }
    const double v = std::ldexp(static_cast<double>(m), exp - 40 - rexp + drop);
    return negative ? -v : v;
}

uint32_t lowByte(int32_t v) { return static_cast<uint32_t>(v) & 0xFF; }

}  // namespace

void Lfo::construct(float rate) {
    // TObject.Create zero-fills the instance.
    *this = Lfo();
    emuPeriod = 0x1c80;
    controlRate = rate;
    defaultRate = 1;
    resetState();
}

void Lfo::resetState() {
    output = 0;
    running = 0;
    phase = 0;
    phaseInc = 0;
    freqSeen = 0;
    humanOffset = 0;
    level = 0;
    levelTarget = 0;
    levelStep = 0;
    ticks = 0;
    delayMode = 0;
    reverse = 0;
    oneShot = 0;
    wave = 0;
    human = 0;
    humanIndex = 0;
    humanStep = 0;
    humanCount = 0;
    emuClock = 0;
    am = 0;
}

void Lfo::setControlRate(float rate) {
    // The original compares the 80-bit native rate with the Single argument:
    // never equal for a real float, so this branch is only taken for a NaN
    // (unordered compare sets ZF). Faithful, but dead in practice.
    if (std::isnan(rate)) {
        defaultRate = 1;
        controlRate = 0x1.4e5eccp+6f;  // 0x42a72f66 = 83.592575
        rateRatio = 1.0f;
        rateRatioHigh = 1.0f;
        rateRatioLow = 0x1.24924ap-3f;  // 0x3e124925
        emuPeriod = 0x1c80;
    } else {
        defaultRate = 0;
        if (rate < kMinRate) rate = 0x1.47ae14p-7f;  // 0x3c23d70a = 0.01f
        controlRate = rate;
        rateRatio = sgl(divExtByFloat(kNativeRateMant, kNativeRateExp, rate));
        rateRatioHigh = rateRatio;
        rateRatioLow = sgl(static_cast<double>(rateRatioHigh) / 7.0f);
        emuPeriod = fistp32(sgl(7296.0f * static_cast<double>(rateRatio)));
    }
    resetState();
}

void Lfo::setDelayMode(uint8_t mode) {
    level = static_cast<int32_t>(static_cast<uint32_t>(level) >> (levelShift & 31));
    delayMode = mode == 1 ? 1 : 0;
    levelShift = data::kLfoLevelShift[delayMode];
    amShift = data::kLfoAmShift[delayMode];
    level = shl32(level, levelShift);
}

void Lfo::setSmoothing(int32_t amount) {
    if (amount < 1) {
        smoothCur = 0;
        return;
    }
    smoothCur = amount;
    if (amount >= 0x100) amount = 0xff;
    const uint32_t t = static_cast<uint32_t>(0xff - amount);
    const int32_t a = 0xff - static_cast<int32_t>((t * t) >> 8);
    smoothA = a;
    smoothB = 0x100 - a;
}

void Lfo::trigger(int32_t phaseSteps, bool resetPhase) {
    running = -1;
    if (resetPhase) phase = 0;
    if (phaseSteps != 0) phase += static_cast<uint32_t>(phaseSteps) << 24;
    setDelayMode(static_cast<uint8_t>(delayModeIn));
    level1Seen = level1In;
    level2Seen = level2In;
    delaySeen = delayIn;
    level = shl32(level1In, levelShift);
    levelTarget = shl32(level2In, levelShift);
    if (delayIn > 0 && level1In != level2In) {
        levelStep = level1In < level2In ? delayIn : neg32(delayIn);
    } else {
        levelStep = 0;
    }
    if (defaultRate == 0 && delayMode > 0) {
        levelStep = fistp32(sgl(static_cast<double>(levelStep) * rateRatio));
    }
    ticks = 0;
    emuClock = 0;
    am = 0;
    freqSeen = INT32_MIN;
    humanOffset = 0;
    reverse = reverseIn;
    oneShot = oneShotIn;
    wave = 0;
    freqShift = 0;
    humanIndex &= 0xff;
    if (humanIn < 2) {
        humanStep = 0;
        humanCount = 0;
        if (humanIndex >= 0x80) humanIndex -= 0x80;
    }
    output = 0;
    smoothCur = 0;
}

int32_t Lfo::freqToIncrement(int32_t freq) const {
    uint32_t f = static_cast<uint32_t>(freq);
    const bool negative = freq < 0;
    if (negative) f = 0u - f;
    int32_t r;
    if (static_cast<int32_t>(f) < 0x700) {
        // Linear below FREQ 7.
        r = fistp32(sgl(static_cast<double>(static_cast<int32_t>(f << 14)) * rateRatioLow));
    } else if (static_cast<int32_t>(f) < 0x8600) {
        r = fistp32(sgl(static_cast<double>(static_cast<int32_t>((f - 0x600) << 14)) * rateRatioHigh));
    } else {
        r = fistp32(sgl(536870912.0f * static_cast<double>(rateRatioHigh)));
    }
    if ((reverse > 0) != negative) r = neg32(r);
    return r;
}

void Lfo::updateParams() {
    am = amIn;

    // Amplitude fade (delay) setup, redone when L1, L2, DELAY or the mode change.
    if (level1In != level1Seen || level2In != level2Seen || delayIn != delaySeen || delayModeIn != delayMode) {
        setDelayMode(static_cast<uint8_t>(delayModeIn));
        levelTarget = shl32(level2In, levelShift);
        const int32_t l1 = level1In;
        if (level2In == l1 || delayIn <= 0) {
            levelStep = 0;
            if (delayIn <= 0) level = shl32(level1In, levelShift);
        } else if (l1 <= level2In) {
            // Rising: continue from where the fade would be after `ticks` steps.
            if (delayMode <= 0) {
                const int32_t t = mul32(ticks, delayIn);
                const int32_t d = fistp32(sgl(mulExt(kEmuStepsPerTick, static_cast<double>(t))));
                level = add32(d, shl32(l1, levelShift));
            } else {
                level = add32(shl32(l1, levelShift), mul32(ticks, delayIn));
            }
            if (levelTarget > level) {
                if (defaultRate == 0 && delayMode > 0) {
                    levelStep = fistp32(sgl(static_cast<double>(delayIn) * rateRatio));
                } else {
                    levelStep = delayIn;
                }
            } else {
                level = levelTarget;
                levelStep = 0;
            }
        } else {
            // Falling.
            if (delayMode <= 0) {
                const int32_t t = mul32(ticks, delayIn);
                const int32_t d = fistp32(sgl(mulExt(kEmuStepsPerTick, static_cast<double>(t))));
                level = sub32(shl32(l1, levelShift), d);
            } else {
                level = sub32(shl32(l1, levelShift), mul32(ticks, delayIn));
            }
            if (levelTarget < level) {
                if (defaultRate == 0 && delayMode > 0) {
                    levelStep = neg32(fistp32(sgl(static_cast<double>(delayIn) * rateRatio)));
                } else {
                    levelStep = neg32(delayIn);
                }
            } else {
                level = levelTarget;
                levelStep = 0;
            }
        }
        level1Seen = level1In;
        level2Seen = level2In;
        delaySeen = delayIn;
    }

    // Waveform selection.
    if (waveIn != wave) {
        wave = waveIn;
        if (waveIn < kLfoOscWaveFirst) {
            freqShift = 0;
        } else if (waveIn < kLfoShapeFirst) {
            if (!waveSource) {
                wave = 0;
            } else {
                const int32_t i = waveIn - kLfoOscWaveFirst;
                int32_t sizeLog2 = 0;
                if (i < 0 || i > 0x45) {
                    waveData = nullptr;
                } else {
                    const uint8_t* p = data::kLfoWaveParams + 4 * i;
                    const WaveLocation loc = waveSource(static_cast<int32_t>(p[1]) + waveKeyIn, p[0]);
                    waveData = loc.data;
                    sizeLog2 = loc.sizeLog2;
                }
                if (!waveData) {
                    wave = 0;
                    freqShift = 0;
                } else {
                    waveMask = (1u << (static_cast<uint32_t>(sizeLog2) & 31)) - 1;
                    waveShift = 30 - sizeLog2;
                    freqShift = static_cast<int32_t>(data::kLfoWaveParams[4 * i + 2]) + freqShiftIn;
                }
            }
        } else {
            freqShift = 0;
            const int32_t i = waveIn - kLfoShapeFirst;
            if (i < 8) {
                waveData = reinterpret_cast<const uint8_t*>(data::kLfoShapeTables) + 256 * i;
            } else {
                wave = 0;
            }
        }
    }

    freqSeen = freqIn;
    reverse = reverseIn;
    if (oneShotIn != oneShot) {
        oneShot = oneShotIn;
        running = -1;
    }

    // Phase increment, with humanization (uses the mode of the previous tick).
    if (human <= 0) {
        phaseInc = freqToIncrement(freqIn);
    } else if (human == 1) {
        // SQ80 emulation: no humanization below FREQ 7.
        const uint32_t base = static_cast<uint32_t>(freqIn) >> 8;
        const uint32_t humanized = static_cast<uint32_t>(add32(freqIn, humanOffset)) >> 8;
        if (static_cast<int32_t>(base) < 7) {
            phaseInc = freqToIncrement(freqIn);
        } else if (static_cast<int32_t>(humanized) < 7) {
            phaseInc = 0;
        } else {
            phaseInc = freqToIncrement(add32(freqIn, humanOffset));
        }
    } else {
        phaseInc = freqToIncrement(add32(freqIn, humanOffset));
    }
    if (freqShift > 0) {
        const uint32_t n = static_cast<uint32_t>(freqShift) & 31;
        if (phaseInc >= 0) {
            phaseInc = static_cast<int32_t>(static_cast<uint32_t>(phaseInc) >> n);
        } else {
            phaseInc = neg32(static_cast<int32_t>(static_cast<uint32_t>(neg32(phaseInc)) >> n));
        }
    }

    phaseOffset[0] = shl32(phaseIn, 14);
    if (twinIn <= 0) {
        twin = 0;
    } else {
        phaseOffset[1] = shl32(twinPhaseIn, 14);
        twin = 1;
    }
    if (reverse > 0 || phaseInc < 0) {
        phaseOffset[0] = neg32(phaseOffset[0]);
        phaseOffset[1] = neg32(phaseOffset[1]);  // also when twin is off (flips every tick)
    }

    if (humanIn != human) {
        human = humanIn;
        humanOffset = 0;
        humanStep = 0;
        humanCount = 0;
    }
    if (smoothIn != smoothCur) setSmoothing(smoothIn);
}

int32_t Lfo::step() {
    if (running == 0) return output;

    // Wave value(s) for phase + offset; byte arithmetic as in the original.
    uint8_t s[2] = {0, 0};
    for (int32_t j = 0; j <= twin && j < 2; j++) {
        const uint32_t ph = static_cast<uint32_t>(phaseOffset[j]) + phase;
        const uint8_t b = static_cast<uint8_t>(ph >> 22);
        const bool positive = static_cast<int8_t>(b) >= 0;
        uint8_t v;
        switch (static_cast<uint32_t>(wave)) {
        case kLfoTri:
            v = static_cast<uint8_t>(((positive ? b : static_cast<uint8_t>(~b)) - 0x40) * 2);
            break;
        case kLfoSaw:
            v = static_cast<uint8_t>(b + 0x80);
            break;
        case kLfoSquare:
            v = positive ? 0x7f : 0x00;
            break;
        case kLfoNoise:
            v = static_cast<uint8_t>(data::kLfoNoise[static_cast<int8_t>(b) + 128]);
            break;
        case kLfoBipolar:
            v = positive ? 0x7f : 0x81;
            break;
        default:
            if (wave < kLfoShapeFirst) {
                const uint8_t r = waveData[(ph >> (static_cast<uint32_t>(waveShift) & 31)) & waveMask];
                v = r != 0 ? static_cast<uint8_t>(r - 0x80) : 0;
            } else {
                v = waveData[(ph >> 22) & 0xff];
            }
            break;
        }
        s[j] = v;
    }
    int8_t v = static_cast<int8_t>(s[0]);
    if (twin > 0) {
        const int32_t d = static_cast<int32_t>(static_cast<int8_t>(s[0])) - static_cast<int8_t>(s[1]);
        v = d <= -127 ? -127 : (d >= 127 ? 127 : static_cast<int8_t>(d));
    }

    // Smoothing (one-pole, before amplitude).
    if (smoothCur > 0) {
        int32_t e = add32(mul32(smoothState, smoothA), shl32(mul32(v, smoothB), 8));
        if (e < 0) e = add32(e, 0xff);
        e >>= 8;
        smoothState = e;
        if (e < 0) e = add32(e, 0xff);
        e >>= 8;
        v = static_cast<int8_t>(lowByte(e));
    }

    // Amplitude: level (delay fade) plus amplitude modulation.
    const int32_t amp = add32(shl32(am, amShift), level);
    int32_t out;
    if (amp <= 0) {
        out = 0;
    } else if (delayMode == 0) {
        int32_t p = mul32(v, amp);
        if (p < 0) p = add32(p, 0xff);
        out = p >> 8;
    } else {
        int32_t p = mul32(v, amp);
        if (p < 0) p = add32(p, 0x7ff);
        out = p >> 11;
    }

    phase += static_cast<uint32_t>(phaseInc);
    if (oneShot > 0 && phase > 0x40000000u) running = 0;

    // EMU steps: every emuPeriod/1024 ticks (the SQ80's coarse LFO update).
    bool emuStep;
    if (emuClock <= 0) {
        emuClock = add32(emuClock, emuPeriod);
        emuStep = true;
    } else {
        emuClock = sub32(emuClock, 0x400);
        emuStep = false;
    }
    if (delayMode > 0 || emuStep) {
        if (levelStep != 0) {
            level = add32(level, levelStep);
            if (levelStep > 0 ? levelTarget <= level : levelTarget >= level) {
                level = levelTarget;
                levelStep = 0;
            }
        }
        if (ticks < 0x7fffffff) ticks++;
    }

    // Humanization: random walk of the frequency offset, one table entry per EMU step.
    if (human > 0 && emuStep) {
        if (human > 1 || levelStep == 0) {
            const int8_t c = data::kLfoHumanize[static_cast<int8_t>(lowByte(humanIndex)) + 128];
            if (c != 0) {
                if (humanCount > 0) humanOffset = add32(humanOffset, mul32(humanStep, humanCount));
                humanStep = c * 256;
                humanCount = human > 2 ? shl32(1, static_cast<uint32_t>(human - 2)) : 1;
            }
            if (humanCount > 0) {
                humanOffset = add32(humanOffset, humanStep);
                humanCount--;
            }
        }
        humanIndex = add32(humanIndex, 1);
    }

    output = out;
    return out;
}

}  // namespace sq8l
