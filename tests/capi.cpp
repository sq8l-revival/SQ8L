// C API exposing engine internals to the Python differential tests.
//
// Objects can be loaded from / saved to the byte layout of the original
// Delphi object, so a state captured inside the emulator can be replayed.
#include <cstdint>
#include <cstring>

#include "FilterSQ.h"
#include "Fpu.h"

#include "capi_export.h"

namespace {

template <typename T>
void get(const uint8_t* src, uint32_t off, T& v) { std::memcpy(&v, src + off, sizeof(T)); }
template <typename T>
void put(uint8_t* dst, uint32_t off, const T& v) { std::memcpy(dst + off, &v, sizeof(T)); }

// Field map of CfilterSQ (excluding vmt and the table pointer).
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

}  // namespace

// ---------------------------------------------------------------- filter
SQ8L_API const float* sq8l_filter_unit_tables() {
    return reinterpret_cast<const float*>(&sq8l::filterUnitTables());
}

SQ8L_API void* sq8l_filter_table_new(float sampleRate) {
    auto* t = new sq8l::FilterTable;
    sq8l::RoundToNearest rn;
    t->compute(sampleRate);
    return t;
}

SQ8L_API const float* sq8l_filter_table_data(void* t) {
    return &static_cast<sq8l::FilterTable*>(t)->entry[0][0][0];
}

SQ8L_API void sq8l_filter_table_free(void* t) { delete static_cast<sq8l::FilterTable*>(t); }

SQ8L_API void* sq8l_filter_new(void* table, float sampleRate) {
    auto* f = new sq8l::FilterSQ;
    sq8l::RoundToNearest rn;
    f->init(static_cast<sq8l::FilterTable*>(table), sampleRate);
    return f;
}

SQ8L_API void sq8l_filter_free(void* f) { delete static_cast<sq8l::FilterSQ*>(f); }

SQ8L_API void sq8l_filter_load(void* f, const uint8_t* orig) {
    filterFields([](const uint8_t* b, uint32_t off, auto& v) { get(b, off, v); },
                 *static_cast<sq8l::FilterSQ*>(f), orig);
}

SQ8L_API void sq8l_filter_save(void* f, uint8_t* orig) {
    filterFields([](uint8_t* b, uint32_t off, auto& v) { put(b, off, v); },
                 *static_cast<sq8l::FilterSQ*>(f), orig);
}

SQ8L_API double sq8l_filter_process(void* f, double x) {
    sq8l::RoundTowardZero rz;
    return static_cast<sq8l::FilterSQ*>(f)->process(x);
}

SQ8L_API void sq8l_filter_set_params(void* f, int32_t cutoff, int32_t reso, int32_t immediate, int32_t towardZero) {
    auto* flt = static_cast<sq8l::FilterSQ*>(f);
    if (towardZero) {
        sq8l::RoundTowardZero rz;
        flt->setParams(cutoff, reso, immediate != 0);
    } else {
        flt->setParams(cutoff, reso, immediate != 0);
    }
}
