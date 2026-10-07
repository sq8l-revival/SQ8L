// C API: DOC oscillator engine (sq8l::Doc) for the differential tests.
//
// A Doc can be loaded from / saved to the byte image of the original Cdoc object (10400 bytes).
// The vmt (+0) and the wave ROM pointer (+0x2008) are not mapped; the linked-voice pointers
// of the parameter blocks (+0x58) are translated using the original object's address.
#include <cstdint>
#include <cstring>

#include "Doc.h"
#include "Fpu.h"

#include "capi_export.h"

namespace {

using sq8l::Doc;
using sq8l::DocVoice;
using sq8l::DocVoiceParams;

constexpr uint32_t kVoiceBase = 0x8;
constexpr uint32_t kVoiceSize = 0x200;
constexpr uint32_t kParamBase = 0x20a0;
constexpr uint32_t kParamSize = 0x80;
constexpr uint32_t kLinkedOffset = 0x58;

template <typename T>
void get(const uint8_t* src, uint32_t off, T& v) { std::memcpy(&v, src + off, sizeof(T)); }
template <typename T>
void put(uint8_t* dst, uint32_t off, const T& v) { std::memcpy(dst + off, &v, sizeof(T)); }

// Scalar fields of Cdoc outside the voice and parameter blocks.
template <typename F, typename Bytes>
void globalFields(F&& f, Doc& d, Bytes b) {
    f(b, 0x0004, d.numVoices);
    f(b, 0x200c, d.dither[0]);
    f(b, 0x2010, d.dither[1]);
    f(b, 0x2014, d.phaseScale);
    f(b, 0x2018, d.jumpThreshold);
    f(b, 0x201c, d.amScale);
    f(b, 0x2020, d.amOffset);
    f(b, 0x2024, d.sampleScale);
    f(b, 0x2028, d.k0_25);
    f(b, 0x202c, d.k0_5);
    f(b, 0x2030, d.k0_75);
    f(b, 0x2034, d.k1_5);
    f(b, 0x2038, d.k1_75);
    f(b, 0x203c, d.k2);
    f(b, 0x2040, d.k2_5);
    f(b, 0x2044, d.k3);
    f(b, 0x2048, d.k4);
    f(b, 0x204c, d.k1_6);
    f(b, 0x2050, d.k1_9);
    f(b, 0x2054, d.k1_18);
    f(b, 0x2058, d.k5_3);
    f(b, 0x205c, d.k11_36);
    f(b, 0x2060, d.k11_6);
    f(b, 0x2064, d.k85_36);
    f(b, 0x2068, d.phaseInc);
    f(b, 0x206c, d.dcB0);
    f(b, 0x2070, d.dcB1);
    f(b, 0x2074, d.dcB2);
    f(b, 0x2078, d.dcA1);
    f(b, 0x207c, d.dcA2);
    f(b, 0x2080, d.smoothPole);
    f(b, 0x2084, d.amSmoothPole);
    f(b, 0x2088, d.smoothGain);
    f(b, 0x208c, d.amSmoothGain);
    f(b, 0x2090, d.docRate);
    f(b, 0x2094, d.sampleRate);
    f(b, 0x2098, d.invSampleRate);
    f(b, 0x209c, d.releaseSamples);
}

Doc& doc(void* d) { return *static_cast<Doc*>(d); }

// Original pointer to a voice block <-> slot (index + 1, 0 = none).
int32_t slotFromPointer(uint32_t ptr, uint32_t base) {
    if (ptr == 0) return 0;
    return static_cast<int32_t>((ptr - base - kVoiceBase) / kVoiceSize) + 1;
}
uint32_t pointerFromSlot(int32_t slot, uint32_t base) {
    if (slot == 0) return 0;
    return base + kVoiceBase + static_cast<uint32_t>(slot - 1) * kVoiceSize;
}

}  // namespace

SQ8L_API uint32_t sq8l_doc_object_size() { return 10400; }

SQ8L_API void* sq8l_doc_new(uint32_t numVoices) {
    sq8l::RoundToNearest rn;
    return new Doc(numVoices);
}

SQ8L_API void sq8l_doc_free(void* d) { delete static_cast<Doc*>(d); }

SQ8L_API void sq8l_doc_load(void* d, const uint8_t* obj, uint32_t base) {
    Doc& o = doc(d);
    globalFields([](const uint8_t* b, uint32_t off, auto& v) { get(b, off, v); }, o, obj);
    for (uint32_t v = 0; v < Doc::kMaxVoices; v++) {
        std::memcpy(&o.voice[v], obj + kVoiceBase + v * kVoiceSize, kVoiceSize);
        std::memcpy(&o.param[v], obj + kParamBase + v * kParamSize, kParamSize);
        uint32_t ptr;
        get(obj, kParamBase + v * kParamSize + kLinkedOffset, ptr);
        o.param[v].linkedSlot = slotFromPointer(ptr, base);
    }
}

SQ8L_API void sq8l_doc_save(void* d, uint8_t* obj, uint32_t base) {
    Doc& o = doc(d);
    globalFields([](uint8_t* b, uint32_t off, auto& v) { put(b, off, v); }, o, obj);
    for (uint32_t v = 0; v < Doc::kMaxVoices; v++) {
        std::memcpy(obj + kVoiceBase + v * kVoiceSize, &o.voice[v], kVoiceSize);
        std::memcpy(obj + kParamBase + v * kParamSize, &o.param[v], kParamSize);
        put(obj, kParamBase + v * kParamSize + kLinkedOffset, pointerFromSlot(o.param[v].linkedSlot, base));
    }
}

SQ8L_API void sq8l_doc_load_voice(void* d, uint32_t v, const uint8_t* block) {
    std::memcpy(&doc(d).voice[v], block, kVoiceSize);
}

SQ8L_API void sq8l_doc_save_voice(void* d, uint32_t v, uint8_t* block) {
    std::memcpy(block, &doc(d).voice[v], kVoiceSize);
}

// ---------------------------------------------------------------- entry points
// tz: run in round-toward-zero (inside processReplacing) instead of round-to-nearest.
#define SQ8L_MODE(tz, stmt)              \
    do {                                 \
        if (tz) {                        \
            sq8l::RoundTowardZero rz;    \
            stmt;                        \
        } else {                         \
            sq8l::RoundToNearest rn;     \
            stmt;                        \
        }                                \
    } while (0)

SQ8L_API void sq8l_doc_set_num_voices(void* d, uint32_t n) { doc(d).setNumVoices(n); }

SQ8L_API void sq8l_doc_set_sample_rate(void* d, float sr, int32_t tz) {
    SQ8L_MODE(tz, doc(d).setSampleRate(sr));
}

SQ8L_API void sq8l_doc_reset_all(void* d, int32_t tz) { SQ8L_MODE(tz, doc(d).resetAll()); }

SQ8L_API void sq8l_doc_reset_voice(void* d, uint32_t v, int32_t tz) { SQ8L_MODE(tz, doc(d).resetVoice(v)); }

SQ8L_API void sq8l_doc_start_voice(void* d, uint32_t v, int32_t key, int32_t linked, int32_t newNote,
                                   int32_t resetPhase) {
    doc(d).startVoice(v, key, linked, newNote, resetPhase);
}

SQ8L_API void sq8l_doc_set_keys(void* d, uint32_t v, int32_t key, int32_t waveKeyToo, int32_t tz) {
    SQ8L_MODE(tz, doc(d).setKeys(v, key, waveKeyToo != 0));
}

SQ8L_API void sq8l_doc_stop_voice(void* d, uint32_t v) { doc(d).stopVoice(v); }

SQ8L_API void sq8l_doc_update(void* d, uint32_t v, int32_t tz) { SQ8L_MODE(tz, doc(d).update(v)); }

SQ8L_API void sq8l_doc_interpolate_levels(void* d, uint32_t v, int32_t tz) {
    SQ8L_MODE(tz, doc(d).interpolateLevels(v));
}

// Offset of params(v) in the original object layout, 0 for nullptr.
SQ8L_API uint32_t sq8l_doc_params_offset(void* d, uint32_t v) {
    DocVoiceParams* p = doc(d).params(v);
    if (!p) return 0;
    return kParamBase + static_cast<uint32_t>(p - doc(d).param) * kParamSize;
}

SQ8L_API double sq8l_doc_render(void* d, uint32_t v) {
    sq8l::RoundTowardZero rz;
    return doc(d).render(v);
}

// ---------------------------------------------------------------- helpers
SQ8L_API float sq8l_doc_level_to_amp(void* d, int32_t level, int32_t tz) {
    float r = 0;
    SQ8L_MODE(tz, r = doc(d).levelToAmp(level));
    return r;
}

SQ8L_API uint32_t sq8l_doc_pitch_to_freq(int32_t pitch) { return sq8l::docPitchToFreq(pitch); }

SQ8L_API void sq8l_doc_compute_pitch(void* d, int32_t waveKey, int32_t pitchKey, int32_t wave, int32_t fine,
                                     int32_t semitone, int32_t* pitch, uint8_t* waveReg, uint8_t* page) {
    doc(d).computePitch(waveKey, pitchKey, pitch, waveReg, page, wave, fine, semitone);
}

SQ8L_API const uint16_t* sq8l_doc_pitch_table() { return sq8l::data::kDocPitchTable; }

SQ8L_API const float* sq8l_doc_level_table() { return sq8l::data::kDocLevelTable; }
