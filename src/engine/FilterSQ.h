// SQ-80 style 4-pole lowpass with resonance (original unit mod_filter4_31, class CfilterSQ).
#pragma once

#include <cstdint>

namespace sq8l {

// Coefficient table shared by all filters, rebuilt on sample rate change.
// entry[reso][cutoff] = {p, k}: one-pole coefficient and feedback amount.
struct FilterTable {
    float entry[32][256][2];
    void compute(float sampleRate);  // original FUN_0045f3d4
};

// Unit-level tables built once at load (original 0x4c5a14, 0x4c5e14, 0x4c5e94).
struct FilterUnitTables {
    float cutoffHz[256];
    float resoAmount[32];
    float resoGain[32];
};
const FilterUnitTables& filterUnitTables();

class FilterSQ {
public:
    // Comments give the field offset in the original object (for differential tests).
    float sampleRate = 0;          // +0x04
    int32_t mode = 0;              // +0x08
    int32_t cutoff = 0;            // +0x0c  0..255
    int32_t reso = 0;              // +0x10  0..31
    float level = 1.0f;            // +0x14
    float gain = 1.0f;             // +0x18  smoothed output gain
    int32_t keyParam = -2;         // +0x1c
    const FilterTable* table = nullptr;  // +0x20
    float stage[5] = {};           // +0x28..+0x38 (stage[0] = input after feedback)
    float g = 0;                   // +0x3c  (1 + p) / 2
    float k = 0;                   // +0x40  resonance feedback (smoothed)
    float p = 0;                   // +0x44  pole coefficient (smoothed)
    float pA = 0, pB = 0;          // +0x48, +0x4c  smoothing factor and 1 - factor
    float kA = 0, kB = 0;          // +0x50, +0x54
    float gainA = 0, gainB = 0;    // +0x58, +0x5c
    float kTarget = 0;             // +0x60  premultiplied by kB
    float pTarget = 0;             // +0x64  premultiplied by pB
    float gainTarget = 0;          // +0x68  premultiplied by gainB
    float resoComp = 1.075f;       // +0x6c
    float half = 0.5f;             // +0x70
    float sixth = 1.0f / 6.0f;     // +0x74
    float smoothTime = 0;          // +0x78

    // Constructor (FUN_0045ef38) with an external table.
    void init(const FilterTable* t, float sr);
    void setSampleRate(float sr);                         // vmt slot 0
    void setMode(int32_t m) { mode = m; }                 // vmt slot 1
    void copyStateFrom(const FilterSQ* other);            // vmt slot 3
    void setParams(int32_t c, int32_t r, bool immediate); // vmt slot 4
    void setCutoff(int32_t c, bool immediate);            // vmt slot 5
    void setReso(int32_t r, bool immediate);              // vmt slot 6
    void setKeyParam(int32_t v);                          // FUN_0045f560

    // One sample (FUN_0045f578): input and output live on the x87 stack.
    double process(double x);

private:
    void computeSmoothing();       // FUN_0045f120
    void applyParams(bool immediate);  // FUN_0045f4dc
};

}  // namespace sq8l
