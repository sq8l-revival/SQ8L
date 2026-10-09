#include "Doc.h"
#include "CrMath.h"

#include <cmath>
#include <cstddef>
#include <cstring>

#include "Fpu.h"
#include "WaveRom.h"

namespace sq8l {

// The structs mirror the original object layout byte for byte.
static_assert(sizeof(DocOscParams) == 0x18, "DocOscParams layout");
static_assert(sizeof(DocVoiceParams) == 0x80, "DocVoiceParams layout");
static_assert(offsetof(DocVoiceParams, init) == 0x48, "DocVoiceParams layout");
static_assert(offsetof(DocVoiceParams, linkedSlot) == 0x58, "DocVoiceParams layout");
static_assert(offsetof(DocVoiceParams, amMode) == 0x64, "DocVoiceParams layout");
static_assert(offsetof(DocVoiceParams, dcBlock) == 0x7c, "DocVoiceParams layout");
static_assert(sizeof(DocOsc) == 0x70, "DocOsc layout");
static_assert(offsetof(DocOsc, halted) == 0x24, "DocOsc layout");
static_assert(offsetof(DocOsc, ampTarget) == 0x2c, "DocOsc layout");
static_assert(offsetof(DocOsc, bank) == 0x3c, "DocOsc layout");
static_assert(offsetof(DocOsc, acc) == 0x5c, "DocOsc layout");
static_assert(offsetof(DocOsc, amBug) == 0x6c, "DocOsc layout");
static_assert(sizeof(DocVoice) == 0x200, "DocVoice layout");
static_assert(offsetof(DocVoice, amMode) == 0x150, "DocVoice layout");
static_assert(offsetof(DocVoice, phase) == 0x164, "DocVoice layout");
static_assert(offsetof(DocVoice, hist) == 0x16c, "DocVoice layout");
static_assert(offsetof(DocVoice, coef) == 0x180, "DocVoice layout");
static_assert(offsetof(DocVoice, dcBlock) == 0x190, "DocVoice layout");
static_assert(offsetof(DocVoice, waveKey) == 0x1a4, "DocVoice layout");
static_assert(offsetof(DocVoice, smoothMode) == 0x1ac, "DocVoice layout");

namespace {

// 80-bit constants of the original code, split hi/lo (see Fpu.h).
constexpr Ext kReleaseTime = {0x1.1eb851eb851ecp-4, -0x1.eb80000000000p-58};    // 0.07 s
constexpr Ext kSmoothTime = {0x1.0624dd2f1a9fcp-9, -0x1.8a00000000000p-65};     // 0.002 s
constexpr Ext kAmSmoothTime = {0x1.a36e2eb1c432dp-13, -0x1.6a00000000000p-67};  // 0.0002 s
constexpr Ext kDcOmega = {0x1.78fdb9effea47p+6, -0x1.ee00000000000p-49};        // 2 pi 15 Hz
constexpr Ext kDcTwoQ = {0x1.6a0a5269595ffp+0, -0x1.2d00000000000p-55};         // 1.41422

// Interpolation order thresholds on the fastest oscillator's table step rate (0x4c167c).
constexpr float kInterpThreshold[3] = {576717.0f, 1631846.0f, 6918636.0f};

constexpr uint32_t kRomMask = 0x3ffff;
constexpr uint32_t kPhaseMask = 0x3fffffff;

float bitsToFloat(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

// x87 "FLD x; FDIV tbyte c" at 53-bit precision: x / (hi + lo) rounded once (to within the
// last bit; only ever stored to a Single afterwards).
double divExt(double x, Ext c) {
    const double q = x / c.hi;
    const double r = std::fma(-q, c.hi, x) - q * c.lo;
    return q + r / c.hi;
}

// Delphi Power(Base, Exponent) for a non-integral exponent: exp(e * ln(b)).
double delphiPower(double base, double exponent) { return crmath::exp(exponent * crmath::log(base)); }

// 32-bit two's complement addition (x86 ADD; no signed-overflow UB in C++).
int32_t add32(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

// x87 FISTP of an already rounded value: out of range -> integer indefinite (0x80000000).
int32_t x87Int(double r) {
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) return INT32_MIN;
    return static_cast<int32_t>(r);
}

// Delphi Trunc(Single) (0x417640) and Round(Single) (0x4175c0, current rounding mode).
int32_t delphiTrunc(float x) { return x87Int(std::trunc(static_cast<double>(x))); }
int32_t delphiRound(float x) { return x87Int(std::nearbyint(static_cast<double>(x))); }

}  // namespace

// ------------------------------------------------------------------ unit docPitch

uint32_t docPitchToFreq(int32_t pitch) {
    // Pitches above the table are folded down by octaves (12 semitones = 0xc00).
    while (pitch >= 0x8000) pitch -= 0xc00;
    uint32_t i = pitch <= 0 ? 0u : static_cast<uint32_t>(pitch) >> 3;  // 1/32 semitone
    const bool odd = (i & 1) != 0;
    i >>= 1;  // 1/16 semitone table index
    if (i >= 0x800) return data::kDocPitchTable[0x7ff];
    if (!odd) return data::kDocPitchTable[i];
    return (static_cast<uint32_t>(data::kDocPitchTable[i]) + data::kDocPitchTable[i + 1]) >> 1;
}

// ------------------------------------------------------------------ construction

Doc::Doc(uint32_t n, uint32_t cap) {
    capacity = cap < kMaxVoices ? cap : kMaxVoices;
    numVoices = 0;
    setNumVoices(n);
    // The original decompresses the wave ROM here (CpackDlt36 -> +0x2008): data::kWaveRom.
    initConstants();
    setDocRate(kDocRate);
}

void Doc::initConstants() {
    dither[0] = bitsToFloat(0x2edbe6ff);   // 1e-10
    dither[1] = bitsToFloat(0xaedbe6ff);   // -1e-10
    phaseScale = bitsToFloat(0x30800000);  // 2^-30
    jumpThreshold = bitsToFloat(0x3a83126f);
    amScale = bitsToFloat(0x3b9acf38);
    amOffset = bitsToFloat(0x3ecccccd);
    sampleScale = bitsToFloat(0x3c010204);
    k0_25 = 0.25f;
    k0_5 = 0.5f;
    k0_75 = 0.75f;
    k1_5 = 1.5f;
    k1_75 = 1.75f;
    k2 = 2.0f;
    k2_5 = 2.5f;
    k3 = 3.0f;
    k4 = 4.0f;
    k1_6 = bitsToFloat(0x3e2aaaab);
    k1_9 = bitsToFloat(0x3de38e39);
    k1_18 = bitsToFloat(0x3d638e39);
    k5_3 = bitsToFloat(0x3fd55555);
    k11_36 = bitsToFloat(0x3e9c71c7);
    k11_6 = bitsToFloat(0x3feaaaab);
    k85_36 = bitsToFloat(0x40171c72);
}

void Doc::setNumVoices(uint32_t n) { numVoices = n < capacity ? n : capacity; }

void Doc::setDocRate(float rate) {
    docRate = rate;
    computeSmoothing();
    resetVoiceClocks();  // (port) the per-voice poles follow the nominal ones
}

void Doc::computeSmoothing() {
    // pole = 0.01 ^ (1 / (T * docRate + 1)) for T = 2 ms (DCA) and 0.2 ms (AM path).
    const double e = 1.0f / (mulExt(kSmoothTime, docRate) + 1.0f);
    smoothPole = sgl(delphiPower(0.01, e));
    smoothGain = sgl(1.0f - static_cast<double>(smoothPole));
    const double eAm = 1.0f / (mulExt(kAmSmoothTime, docRate) + 1.0f);
    amSmoothPole = sgl(delphiPower(0.01, eAm));
    amSmoothGain = sgl(1.0f - static_cast<double>(amSmoothPole));
}

void Doc::setSampleRate(float sr) {
    sampleRate = sr;
    invSampleRate = sgl(1.0f / static_cast<double>(sr));
    phaseInc = static_cast<uint32_t>(
        delphiTrunc(sgl(static_cast<double>(docRate) / sr * 1073741824.0f)));
    releaseSamples = delphiRound(sgl(mulExt(kReleaseTime, sr)));
    computeDcBlocker();
    resetVoiceClocks();  // (port) the per-voice increments follow the nominal one
}

void Doc::computeDcBlocker() {
    // RBJ high-pass, f0 = 15 Hz, Q = 1/1.41422.
    const float w = sgl(mulExt(kDcOmega, invSampleRate));
    const float cs = sgl(crmath::cos(static_cast<double>(w)));
    const float alpha = sgl(divExt(crmath::sin(static_cast<double>(w)), kDcTwoQ));
    const float a0 = sgl(1.0f + static_cast<double>(alpha));
    dcB0 = sgl((1.0f + static_cast<double>(cs)) * 0.5f / a0);
    dcB1 = sgl(-(1.0f + static_cast<double>(cs)) / a0);
    dcB2 = sgl((1.0f + static_cast<double>(cs)) * 0.5f / a0);
    dcA1 = sgl(-(2.0f * static_cast<double>(cs)) / a0);
    dcA2 = sgl((1.0f - static_cast<double>(alpha)) / a0);
}

// ------------------------------------------------------------------ voice management

void Doc::resetAll() {
    for (uint32_t v = 0; v < capacity; v++) resetVoice(v);
}

void Doc::resetVoice(uint32_t v) {
    if (v >= capacity) return;
    param[v] = DocVoiceParams{};
    DocVoice& vc = voice[v];
    vc = DocVoice{};
    vc.smoothMode = 2;
    setVoiceClock(v, 1.0);  // (port) a reset slot is never left on a scaled clock
    for (int i = 0; i < 3; i++) {
        DocOsc& o = vc.osc[i];
        o.levelCur = levelToAmp(0);
        o.levelPrev = o.levelCur;
        o.amp = o.levelCur;
        setAmpTarget(v, vc, i, o.levelCur);
        o.halted = -1;
    }
}

void Doc::startVoice(uint32_t v, int32_t key, int32_t linkedVoice, int32_t newNote, int32_t resetPhase) {
    if (v >= numVoices) return;
    DocVoiceParams& p = param[v];
    p.init = -1;
    p.resetPhase = resetPhase;
    p.newNote = newNote;
    p.forceRecalc = -1;
    p.linkedSlot = (linkedVoice < 0 || linkedVoice >= static_cast<int32_t>(capacity)) ? 0 : linkedVoice + 1;
    p.waveKey = key;
    p.pitchKey = key;
    for (DocOscParams& op : p.osc) op.pitchMod = 0;
    setVoiceClock(v, 1.0);  // (port) the first control tick sets the real ratio
}

void Doc::setKeys(uint32_t v, int32_t key, bool waveKeyToo) {
    if (v >= numVoices) return;
    DocVoiceParams& p = param[v];
    if (waveKeyToo) {
        p.waveKey = key;
        p.forceRecalc = -1;
    }
    p.pitchKey = key;
    update(v);
}

void Doc::stopVoice(uint32_t v) {
    if (v >= numVoices) return;
    for (DocOsc& o : voice[v].osc) o.halted = -1;
    voice[v].interpOrder = 0;
}

DocVoiceParams* Doc::params(uint32_t v) { return v < numVoices ? &param[v] : nullptr; }

// ------------------------------------------------------------------ parameter update

float Doc::levelToAmp(int32_t level) const {
    if (level <= 0) return data::kDocLevelTable[0];
    int32_t i = level >> 7;
    if (i > 255) i = 255;
    const int32_t frac = level & 0x7f;
    if (frac <= 0) return data::kDocLevelTable[i];
    const float f = sgl(static_cast<double>(frac) * sampleScale);
    const int32_t j = i + 1 > 255 ? 255 : i + 1;
    const double a = data::kDocLevelTable[i];
    return sgl((static_cast<double>(data::kDocLevelTable[j]) - a) * f + a);
}

void Doc::setAmpTarget(uint32_t v, DocVoice& vc, int osc, float level) {
    const VoiceClock& c = clock[v];
    const float gain = (osc == 1 && vc.amMode == 1) ? c.amSmoothGain : c.smoothGain;
    vc.osc[osc].ampTarget = sgl(static_cast<double>(level) * gain);
}

// ------------------------------------------------------------------ (port) voice clock

int32_t Doc::basePitch(int32_t key, int32_t wave) const {
    int32_t p = 0;
    uint8_t waveReg = 0, page = 0;
    computePitch(key, key, &p, &waveReg, &page, wave, 0, 0);
    return p;
}

void Doc::setVoiceClock(uint32_t v, double ratio) {
    if (v >= kMaxVoices) return;
    VoiceClock& c = clock[v];
    if (!(ratio > 0.0)) ratio = 1.0;  // NaN or non-positive: fall back to nominal
    if (ratio == 1.0) {
        // Copy, never recompute: the untuned path has to stay bit-exact.
        c.phaseInc = phaseInc;
        c.smoothPole = smoothPole;
        c.smoothGain = smoothGain;
        c.amSmoothPole = amSmoothPole;
        c.amSmoothGain = amSmoothGain;
        c.quantizedOffset = 0;
        c.valid = true;
        return;
    }
    // floor(x + 0.5), not nearbyint: this runs inside process(), which is in round-toward-zero,
    // and the result should not depend on the FPU mode the caller happens to be in.
    double inc = std::floor(static_cast<double>(phaseInc) * ratio + 0.5);
    if (!(inc >= 1.0)) inc = 1.0;  // a frozen resampler would hold one DOC sample forever
    const double incMax = static_cast<double>((1u << 30) - 1u);  // render(): one step per sample
    if (inc > incMax) inc = incMax;
    c.phaseInc = static_cast<uint32_t>(inc);

    // The poles only need the clock to about 0.1%, so quantize the residual to 1/64 semitone
    // (4 units of 1/256) and recompute only when that changes: delphiPower runs CORE-MATH's
    // correctly rounded exp/log, far too slow for every control tick of every voice.
    const int32_t q = static_cast<int32_t>(std::floor(3072.0 * std::log2(ratio) / 4.0 + 0.5));
    if (c.valid && c.quantizedOffset == q) return;
    c.quantizedOffset = q;
    const double rate =
        static_cast<double>(docRate) * std::exp2(static_cast<double>(q) * 4.0 / 3072.0);
    const double e = 1.0f / (mulExt(kSmoothTime, rate) + 1.0f);
    c.smoothPole = sgl(delphiPower(0.01, e));
    c.smoothGain = sgl(1.0f - static_cast<double>(c.smoothPole));
    const double eAm = 1.0f / (mulExt(kAmSmoothTime, rate) + 1.0f);
    c.amSmoothPole = sgl(delphiPower(0.01, eAm));
    c.amSmoothGain = sgl(1.0f - static_cast<double>(c.amSmoothPole));
    c.valid = true;
}

void Doc::resetVoiceClocks() {
    for (uint32_t v = 0; v < kMaxVoices; v++) setVoiceClock(v, 1.0);
}

void Doc::computePitch(int32_t waveKey, int32_t pitchKey, int32_t* pitch, uint8_t* waveReg,
                       uint8_t* page, int32_t wave, int32_t fine, int32_t semitone) const {
    // Wavesample record (FUN_00455f0c): page, wavetable register, semitone and fine offsets.
    uint8_t pg = 0, wr = 0;
    int32_t semiOfs = 0, fineOfs = 0;
    if (static_cast<uint32_t>(wave) < static_cast<uint32_t>(kNumWaves)) {
        int32_t k = add32(waveKey, semitone);
        k = k < 0 ? 0 : (k > 127 ? 127 : k);
        const uint8_t* rec = data::kWaveSampleRecords + 4 * data::kWaveKeyMap[wave * 16 + (k >> 3)];
        pg = rec[0];
        wr = rec[1];
        semiOfs = static_cast<int8_t>(rec[2]);
        fineOfs = rec[3];
    }
    *page = pg;
    // Bring the semitone offset into 12..35 by octaves, moving the DOC resolution instead
    // (FUN_0045c470; byte arithmetic on the whole register, as in the original).
    int32_t s = add32(add32(semitone, semiOfs), 12);
    if (s < 12) {
        for (int n = 0; n < 3; n++) {
            wr++;
            s = add32(s, 12);
            if (s >= 12) break;
        }
    } else if (s >= 36) {
        wr--;
        s -= 12;
    }
    *waveReg = wr;
    *pitch = static_cast<int32_t>((static_cast<uint32_t>(pitchKey) + static_cast<uint32_t>(s)) * 256u +
                                  static_cast<uint32_t>(fine) * 8u + static_cast<uint32_t>(fineOfs));
}

void Doc::update(uint32_t v) {
    if (v >= numVoices) return;
    DocVoice& vc = voice[v];
    DocVoiceParams& p = param[v];

    if (p.amMode == 1) {  // AM needs both oscillators of the pair at full level
        p.osc[0].enabled = -1;
        p.osc[0].level = 0x7f00;
        p.osc[1].enabled = -1;
        p.osc[1].level = 0x7f00;
    }
    vc.amMode = p.amMode;
    if (p.resetPhase != 0) vc.flag154 = 0;
    vc.flag158 = p.flag70 != 0 ? 1 : 0;
    if (p.newNote != 0) {
        if (p.dcBlock > 0) {
            vc.dcX1 = vc.dcX2 = vc.dcY1 = vc.dcY2 = 0;  // FUN_0045c7bc
            vc.dcBlock = p.dcBlock;
        } else {
            vc.dcBlock = 0;
        }
    }
    const bool keyChanged = p.forceRecalc != 0 || vc.waveKey != p.waveKey || vc.pitchKey != p.pitchKey;
    if (keyChanged) {
        vc.waveKey = p.waveKey;
        vc.pitchKey = p.pitchKey;
    }
    vc.smoothMode = p.init != 0 ? 0 : p.levelSmoothing;

    uint8_t osc0WaveReg = 0;  // wavetable register of osc 0, for the AM bug
    for (int i = 0; i < 3; i++) {
        const DocOscParams& op = p.osc[i];
        DocOsc& o = vc.osc[i];
        // Osc 0 keeps running (silently) when it is the sync or AM source of an enabled osc 1.
        const bool source = i == 0 && p.osc[1].enabled != 0 && (p.sync != 0 || p.amMode != 0);
        if (op.enabled == 0 && !source) {
            o.enabled = 0;
            o.valid = 0;
            o.semitone = 0;
            o.fine = 0;
            o.pitch = 0;
            o.modPitch = 0;
            o.pitchMod = 0;
            o.level = 0;
            o.halted = -1;
            continue;
        }

        const bool recalc = keyChanged || o.valid == 0 || op.wave != o.wave || op.semitone != o.semitone ||
                            op.fine != o.fine || (i == 1 && (p.sync != 0) != (o.mode == 2));
        int32_t pitch;
        uint8_t page = 0, waveReg = 0;
        if (recalc) {
            o.wave = op.wave;
            o.semitone = op.semitone;
            o.fine = op.fine;
            computePitch(vc.waveKey, vc.pitchKey, &pitch, &waveReg, &page, op.wave, op.fine, op.semitone);
        } else {
            pitch = o.pitch;
            // The original reads an uninitialised stack byte here (see doc.md); osc 0's bank
            // bit 0 is what it held when osc 0 was last recalculated.
            waveReg = static_cast<uint8_t>((vc.osc[0].bank & 1u) << 7);
        }

        int32_t mode;
        int32_t noPhaseCopy = p.resetPhase;
        const bool oneShot = static_cast<uint32_t>(op.wave) >= 0x36;
        if (!recalc) {
            mode = o.mode;
        } else if (i != 1) {
            mode = oneShot ? 1 : 0;
            if (oneShot) noPhaseCopy = -1;
        } else if (p.sync != 0) {
            mode = 2;
        } else if (p.amMode == 1) {
            mode = 0;
            if (oneShot) noPhaseCopy = -1;
        } else {
            mode = oneShot ? 1 : 0;
            if (oneShot) noPhaseCopy = -1;
        }
        bool amSel = false;
        if (i == 0) {
            osc0WaveReg = waveReg;
        } else if (i == 1) {
            amSel = p.amMode == 1;
        }

        o.enabled = op.enabled;
        o.valid = -1;
        o.pitch = pitch;
        int32_t mp = add32(pitch, op.pitchMod);
        if (mp < 0) mp = 0;
        o.modPitch = mp;
        o.pitchMod = op.pitchMod;
        o.level = op.level;
        o.freq = docPitchToFreq(add32(mp, -16));
        if (noPhaseCopy != 0) {
            o.acc = 0;
            o.wrapped = 0;
        } else if (p.init != 0 && p.linkedSlot > 0 && p.linkedSlot <= static_cast<int32_t>(capacity)) {
            const DocOsc& src = voice[p.linkedSlot - 1].osc[i];
            o.acc = src.acc;
            o.wrapped = src.wrapped;
        }
        o.halted = 0;
        if (recalc) {
            o.page = page;
            o.mode = mode;
            o.amBug = (amSel && p.amBug != 0) ? -1 : 0;
            // SQ-80 AM bug: osc 1 takes bank bit 0 from osc 0's wavetable register.
            const uint8_t bankBit0From = o.amBug != 0 ? osc0WaveReg : waveReg;
            o.bank = ((waveReg & 0x40u) >> 5) | (bankBit0From >> 7);
            o.sizeLog2 = ((waveReg >> 3) & 7) + 8;
            o.tableSize = 1 << o.sizeLog2;
            o.resolution = waveReg & 7;
            o.address = (o.bank << 16) + (o.page << 8);
            const uint8_t accBits = static_cast<uint8_t>(o.resolution + 0x11);
            o.accMask = (1u << (accBits & 31)) - 1u;
            o.shift = static_cast<int32_t>(accBits) - o.sizeLog2;
        }

        const float amp = levelToAmp(op.level);
        if (p.init != 0) {
            setAmpTarget(v, vc, i, amp);
            o.levelCur = amp;
            o.levelPrev = amp;
        } else if (vc.smoothMode == 1) {  // EMU: one control step behind, see interpolateLevels
            setAmpTarget(v, vc, i, o.levelCur);
            o.levelPrev = o.levelCur;
            o.levelCur = amp;
        } else if (vc.smoothMode == 2) {  // FAST
            setAmpTarget(v, vc, i, amp);
            o.levelPrev = o.levelCur;
            o.levelCur = amp;
        } else {
            setAmpTarget(v, vc, i, amp);
        }
    }

    // Resampler polynomial order from the fastest table step rate of the enabled oscillators.
    uint32_t maxRate = 0;
    for (const DocOsc& o : vc.osc) {
        if (o.enabled == 0) continue;
        const uint8_t sh = static_cast<uint8_t>(static_cast<uint8_t>(o.sizeLog2) - 1 - static_cast<uint8_t>(o.resolution));
        const uint32_t rate = o.freq << (sh & 31);
        if (maxRate < rate) maxRate = rate;
    }
    int32_t order = 4;
    for (int j = 0; j < 3; j++) {
        if (static_cast<double>(maxRate) < kInterpThreshold[j]) {
            order = j + 1;
            break;
        }
    }
    vc.interpOrder = order;
    p.init = 0;
    p.resetPhase = 0;
    p.newNote = 0;
    p.forceRecalc = 0;
}

void Doc::interpolateLevels(uint32_t v) {
    if (v >= numVoices) return;
    DocVoice& vc = voice[v];
    if (vc.smoothMode <= 0) return;
    for (int i = 0; i < 3; i++) {
        const DocOsc& o = vc.osc[i];
        if (o.valid == 0) continue;
        if (vc.smoothMode == 1) {  // EMU: halfway between the previous and the current level
            setAmpTarget(v, vc, i, sgl((static_cast<double>(o.levelCur) + o.levelPrev) * 0.5f));
        } else if (vc.smoothMode == 2) {  // FAST: extrapolate half a step, clamped to 0..1
            float x = sgl((3.0f * static_cast<double>(o.levelCur) - o.levelPrev) * 0.5f);
            if (!(x >= 0.0f)) {
                x = 0.0f;
            } else if (x > 1.0f) {
                x = 1.0f;
            }
            setAmpTarget(v, vc, i, x);
        }
    }
}

// ------------------------------------------------------------------ audio

int32_t Doc::stepOscillator(DocVoice& vc, int osc) {
    DocOsc& o = vc.osc[osc];
    if (o.halted != 0) return 0;
    const uint32_t b = data::kWaveRom[((o.acc >> (o.shift & 31)) + o.address) & kRomMask];
    if (b == 0) {  // a zero sample halts the oscillator (DOC 5503)
        o.halted = 1;
        return 0;
    }
    if (o.wrapped != 0) {
        o.wrapped = 0;
        if (osc == 0 && vc.osc[1].mode == 2) vc.osc[1].acc = 0;  // hard sync
        o.halted = o.mode == 1 ? 1 : 0;                          // one-shot ends after a pass
    }
    uint32_t a = o.acc + o.freq;
    if (a >= o.accMask) {
        a &= o.accMask;
        o.wrapped = 1;
    }
    o.acc = a;
    return static_cast<int32_t>(b) - 128;
}

double Doc::render(uint32_t v) {
    if (v >= numVoices) return 0.0;
    DocVoice& vc = voice[v];
    const VoiceClock& ck = clock[v];  // (port) this voice's clock and its smoothing poles

    if (vc.phase <= vc.prevPhase) {
        // The resampler phase wrapped: compute the next DOC output sample.
        const int32_t s0 = stepOscillator(vc, 0);
        const int32_t s1 = stepOscillator(vc, 1);
        const int32_t s2 = stepOscillator(vc, 2);
        DocOsc* o = vc.osc;
        double sum;
        if (vc.amMode == 0) {
            const double t0 = o[0].enabled != 0 ? static_cast<double>(s0) * sampleScale * o[0].amp : 0.0;
            sum = t0 + static_cast<double>(s1) * sampleScale * o[1].amp;
            sum = sum + static_cast<double>(s2) * sampleScale * o[2].amp;
            o[0].amp = sgl(static_cast<double>(ck.smoothPole) * o[0].amp + o[0].ampTarget);
            o[1].amp = sgl(static_cast<double>(ck.smoothPole) * o[1].amp + o[1].ampTarget);
            o[2].amp = sgl(static_cast<double>(ck.smoothPole) * o[2].amp + o[2].ampTarget);
        } else {
            // AM: osc 0 is silent; its sample drives osc 1's amplitude.
            sum = static_cast<double>(s1) * sampleScale * o[1].amp +
                  static_cast<double>(s2) * sampleScale * o[2].amp;
            o[1].amp = sgl((static_cast<double>(s0) * amScale + amOffset) * ck.amSmoothGain +
                           static_cast<double>(ck.amSmoothPole) * o[1].amp);
            o[2].amp = sgl(static_cast<double>(ck.smoothPole) * o[2].amp + o[2].ampTarget);
        }
        vc.hist[0] = vc.hist[1];
        vc.hist[1] = vc.hist[2];
        vc.hist[2] = vc.hist[3];
        vc.hist[3] = sgl(sum + dither[vc.ditherIndex & 1]);
        vc.ditherIndex = (vc.ditherIndex + 1) & 1;

        // Interpolation polynomial through the history (orders 3/4 keep the slope continuous
        // with the previous segment unless it is negligible).
        const double h0 = vc.hist[0], h1 = vc.hist[1], h2 = vc.hist[2], h3 = vc.hist[3];
        float* c = vc.coef;
        switch (vc.interpOrder) {
            case 0:
                break;
            case 1:
                c[0] = sgl(h1 - h0);
                break;
            case 2:
                c[0] = sgl(h1 * k2 - (h0 * k1_5 + h2 * k0_5));
                c[1] = sgl((h0 + h2) * k0_5 - h1);
                break;
            case 3: {
                const double slope = static_cast<double>(c[0]) + static_cast<double>(c[1]) * k2 +
                                     static_cast<double>(c[2]) * k3;
                c[0] = sgl(slope);
                const double c1 = h1 * k2 - (h0 * k1_75 + h2 * k0_25);
                const double c2 = (h0 * k0_75 + h2 * k0_25) - h1;
                if (!(std::fabs(slope) >= jumpThreshold)) {
                    c[1] = sgl(c1);
                    c[2] = sgl(c2);
                } else {
                    c[1] = sgl(c1 - static_cast<double>(c[0]) * k1_5);
                    c[2] = sgl(c2 + static_cast<double>(c[0]) * k0_5);
                }
                break;
            }
            default: {
                const double slope = static_cast<double>(c[0]) + static_cast<double>(c[1]) * k2 +
                                     static_cast<double>(c[2]) * k3 + static_cast<double>(c[3]) * k4;
                c[0] = sgl(slope);
                const double c1 = (h1 * k3 - (h0 * k85_36 + h2 * k0_75)) + h3 * k1_9;
                const double c2 = ((h0 * k5_3 + h2) - h1 * k2_5) - h3 * k1_6;
                const double c3 = (h1 * k0_5 - (h0 * k11_36 + h2 * k0_25)) + h3 * k1_18;
                if (!(std::fabs(slope) >= jumpThreshold)) {
                    c[1] = sgl(c1);
                    c[2] = sgl(c2);
                    c[3] = sgl(c3);
                } else {
                    const double s = c[0];
                    c[1] = sgl(c1 - s * k11_6);
                    c[2] = sgl(c2 + s);
                    c[3] = sgl(c3 - s * k1_6);
                }
                break;
            }
        }
    }

    // Evaluate the polynomial at the resampler phase t in [0, 1).
    const double t = static_cast<double>(static_cast<int32_t>(vc.phase)) * phaseScale;
    double out = vc.hist[0];
    double tp = t;
    for (int32_t j = 0; j < vc.interpOrder; j++) {
        out = out + static_cast<double>(vc.coef[j]) * tp;
        tp = tp * t;
    }
    vc.prevPhase = vc.phase;
    vc.phase = (vc.phase + ck.phaseInc) & kPhaseMask;

    if (vc.dcBlock != 0) {
        double y = static_cast<double>(dcB0) * out + static_cast<double>(dcB1) * vc.dcX1;
        y = y + static_cast<double>(dcB2) * vc.dcX2;
        y = y - static_cast<double>(dcA1) * vc.dcY1;
        y = y - static_cast<double>(dcA2) * vc.dcY2;
        vc.dcX2 = vc.dcX1;
        vc.dcY2 = vc.dcY1;
        vc.dcX1 = sgl(out);
        vc.dcY1 = sgl(y);
        return y;
    }
    return out;
}

}  // namespace sq8l
