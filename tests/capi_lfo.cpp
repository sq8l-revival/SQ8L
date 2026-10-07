// C API: Csq_lfo (sq8l::Lfo) for the differential tests.
//
// Objects are loaded from / saved to the byte layout of the original Delphi
// object (0xf0 bytes). Pointers into the emulator are translated: the wave
// pointer (+0x6c) points either into the Cdoc's wave ROM copy or into the
// shaping tables (0x4c1688); the callback (+0xa8/+0xac) maps to
// sq8l::lfoWaveLocation when assigned.
#include <cfenv>
#include <cstdint>
#include <cstring>

#include "Lfo.h"
#include "WaveRom.h"

#include "capi_export.h"

namespace {

template <typename T>
void get(const uint8_t* src, uint32_t off, T& v) { std::memcpy(&v, src + off, sizeof(T)); }
template <typename T>
void put(uint8_t* dst, uint32_t off, const T& v) { std::memcpy(dst + off, &v, sizeof(T)); }

struct LfoHandle {
    sq8l::Lfo lfo;
    uint32_t rawWavePtr = 0;  // original +0x6c when it maps to neither table
};

uint32_t gRomBase = 0;
uint32_t gShapeBase = 0x4C1688;

const uint8_t* shapeData() { return reinterpret_cast<const uint8_t*>(sq8l::data::kLfoShapeTables); }

// Field map of Csq_lfo (excluding vmt, wave pointer and callback).
template <typename F, typename Obj, typename Bytes>
void lfoFields(F&& f, Obj& o, Bytes b) {
    f(b, 0x04, o.output);
    f(b, 0x08, o.running);
    f(b, 0x0c, o.smoothCur);
    f(b, 0x10, o.smoothState);
    f(b, 0x14, o.smoothA);
    f(b, 0x18, o.smoothB);
    f(b, 0x1c, o.phase);
    f(b, 0x20, o.phaseInc);
    f(b, 0x24, o.phaseOffset[0]);
    f(b, 0x28, o.phaseOffset[1]);
    f(b, 0x2c, o.freqShift);
    f(b, 0x30, o.twin);
    f(b, 0x34, o.freqSeen);
    f(b, 0x38, o.humanOffset);
    f(b, 0x3c, o.reverse);
    f(b, 0x40, o.oneShot);
    f(b, 0x44, o.level1Seen);
    f(b, 0x48, o.level2Seen);
    f(b, 0x4c, o.delaySeen);
    f(b, 0x50, o.delayMode);
    f(b, 0x54, o.level);
    f(b, 0x58, o.levelTarget);
    f(b, 0x5c, o.levelStep);
    f(b, 0x60, o.ticks);
    f(b, 0x64, o.am);
    f(b, 0x68, o.wave);
    f(b, 0x70, o.waveMask);
    f(b, 0x74, o.waveShift);
    f(b, 0x78, o.human);
    f(b, 0x7c, o.humanIndex);
    f(b, 0x80, o.humanStep);
    f(b, 0x84, o.humanCount);
    f(b, 0x88, o.emuClock);
    f(b, 0x8c, o.emuPeriod);
    f(b, 0x90, o.levelShift);
    f(b, 0x91, o.amShift);
    f(b, 0x94, o.controlRate);
    f(b, 0x98, o.rateRatio);
    f(b, 0x9c, o.rateRatioLow);
    f(b, 0xa0, o.rateRatioHigh);
    f(b, 0xa4, o.defaultRate);
    f(b, 0xb0, o.freqIn);
    f(b, 0xb4, o.humanIn);
    f(b, 0xb8, o.waveIn);
    f(b, 0xbc, o.level1In);
    f(b, 0xc0, o.level2In);
    f(b, 0xc4, o.delayIn);
    f(b, 0xc8, o.phaseIn);
    f(b, 0xcc, o.twinPhaseIn);
    f(b, 0xd0, o.amIn);
    f(b, 0xd4, o.delayModeIn);
    f(b, 0xd8, o.reverseIn);
    f(b, 0xdc, o.oneShotIn);
    f(b, 0xe0, o.twinIn);
    f(b, 0xe4, o.smoothIn);
    f(b, 0xe8, o.waveKeyIn);
    f(b, 0xec, o.freqShiftIn);
}

// Run fn with the x87 rounding control rc (FPCW bits 10-11).
template <typename Fn>
auto withRounding(int32_t rc, Fn fn) {
    static const int modes[4] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    const int saved = std::fegetround();
    std::fesetround(modes[rc & 3]);
    auto r = fn();
    std::fesetround(saved);
    return r;
}

}  // namespace

SQ8L_API void sq8l_lfo_set_bases(uint32_t romBase, uint32_t shapeBase) {
    gRomBase = romBase;
    gShapeBase = shapeBase;
}

SQ8L_API void* sq8l_lfo_new() { return new LfoHandle; }

SQ8L_API void sq8l_lfo_free(void* h) { delete static_cast<LfoHandle*>(h); }

SQ8L_API void sq8l_lfo_load(void* hp, const uint8_t* orig) {
    auto* h = static_cast<LfoHandle*>(hp);
    sq8l::Lfo& o = h->lfo;
    lfoFields([](const uint8_t* b, uint32_t off, auto& v) { get(b, off, v); }, o, orig);
    uint32_t p;
    get(orig, 0x6c, p);
    h->rawWavePtr = 0;
    if (p - gRomBase < sq8l::kWaveRomSize) {
        o.waveData = sq8l::data::kWaveRom + (p - gRomBase);
    } else if (p - gShapeBase < 0x800) {
        o.waveData = shapeData() + (p - gShapeBase);
    } else {
        o.waveData = nullptr;
        h->rawWavePtr = p;
    }
    uint16_t codeHigh;
    get(orig, 0xaa, codeHigh);
    o.waveSource = codeHigh != 0 ? &sq8l::lfoWaveLocation : nullptr;
}

SQ8L_API void sq8l_lfo_save(void* hp, uint8_t* orig) {
    auto* h = static_cast<LfoHandle*>(hp);
    const sq8l::Lfo& o = h->lfo;
    sq8l::Lfo copy = o;
    lfoFields([](uint8_t* b, uint32_t off, auto& v) { put(b, off, v); }, copy, orig);
    uint32_t p = h->rawWavePtr;
    if (o.waveData >= sq8l::data::kWaveRom && o.waveData < sq8l::data::kWaveRom + sq8l::kWaveRomSize) {
        p = gRomBase + static_cast<uint32_t>(o.waveData - sq8l::data::kWaveRom);
    } else if (o.waveData >= shapeData() && o.waveData < shapeData() + 0x800) {
        p = gShapeBase + static_cast<uint32_t>(o.waveData - shapeData());
    }
    put(orig, 0x6c, p);
    // Padding bytes are zero in the original (zero-filled instance, byte writes only).
    const uint8_t zero[3] = {0, 0, 0};
    std::memcpy(orig + 0x92, zero, 2);
    std::memcpy(orig + 0xa5, zero, 3);
}

SQ8L_API void sq8l_lfo_construct(void* h, float rate, int32_t rc) {
    withRounding(rc, [&] {
        static_cast<LfoHandle*>(h)->lfo.construct(rate);
        static_cast<LfoHandle*>(h)->rawWavePtr = 0;
        return 0;
    });
}

SQ8L_API void sq8l_lfo_set_control_rate(void* h, float rate, int32_t rc) {
    withRounding(rc, [&] {
        static_cast<LfoHandle*>(h)->lfo.setControlRate(rate);
        return 0;
    });
}

SQ8L_API void sq8l_lfo_trigger(void* h, int32_t phaseSteps, int32_t resetPhase, int32_t rc) {
    withRounding(rc, [&] {
        static_cast<LfoHandle*>(h)->lfo.trigger(phaseSteps, (resetPhase & 0xff) != 0);
        return 0;
    });
}

SQ8L_API int32_t sq8l_lfo_tick(void* h, int32_t rc) {
    return withRounding(rc, [&] { return static_cast<LfoHandle*>(h)->lfo.tick(); });
}

SQ8L_API int32_t sq8l_lfo_freq_to_increment(void* h, int32_t freq, int32_t rc) {
    return withRounding(rc, [&] { return static_cast<LfoHandle*>(h)->lfo.freqToIncrement(freq); });
}

// Data tables: 0 wave params, 1 shaping tables, 2 level shift, 3 AM shift, 4 noise, 5 humanize.
SQ8L_API const uint8_t* sq8l_lfo_table(int32_t which, int32_t* size) {
    using namespace sq8l::data;
    switch (which) {
    case 0: *size = sizeof(kLfoWaveParams); return kLfoWaveParams;
    case 1: *size = sizeof(kLfoShapeTables); return reinterpret_cast<const uint8_t*>(kLfoShapeTables);
    case 2: *size = sizeof(kLfoLevelShift); return kLfoLevelShift;
    case 3: *size = sizeof(kLfoAmShift); return kLfoAmShift;
    case 4: *size = sizeof(kLfoNoise); return reinterpret_cast<const uint8_t*>(kLfoNoise);
    case 5: *size = sizeof(kLfoHumanize); return reinterpret_cast<const uint8_t*>(kLfoHumanize);
    default: *size = 0; return nullptr;
    }
}
