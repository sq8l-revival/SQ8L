// MTS-ESP tuning snapshot and the per-voice retune math (port addition; the original
// plugin has no counterpart). See docs/modules/tuning.md.
//
// The plugin refreshes one Tuning per audio block; the engine only reads it, so no MTS-ESP
// call ever happens on the MIDI or the voice path.
#pragma once

#include <cstdint>

namespace sq8l {

struct Tuning {
    bool enabled = false;               // [port] mtsEsp
    bool correctNativeOffsets = false;  // [port] mtsCorrectPitch: absolute instead of relative
    bool connected = false;             // a master is present
    // MTS_RetuningAsRatio: the master's deviation from 12-ET per key, 1.0 = no change.
    // Both modes work from it; absolute mode additionally divides out the pitch table's own
    // rounding error (see computeRetune).
    double ratio[128] = {};
    uint32_t filtered[4] = {};          // 128-bit mask: key excluded from the scale

    bool active() const { return enabled && connected; }
    bool filteredKey(int key) const {
        if (key < 0 || key > 127) return false;
        return ((filtered[key >> 5] >> (key & 31)) & 1u) != 0;
    }
};

struct VoiceRetune {
    int32_t pitchOffset = 0;  // whole semitones in 1/256-semitone units, added to the voice pitch
    double clockRatio = 1.0;  // resampler clock scale; exactly 1.0 when nothing applies
};

// The retune for one voice. `pBase` is the voice's base pitch in 1/256 semitone, from
// Doc::basePitch; `nominalPhaseInc` the Doc's own phaseInc, which bounds the ratio.
//
// Returns {0, 1.0} whenever no retune applies, so the caller's bit-exact path is a copy of
// the nominal values rather than a recomputation.
VoiceRetune computeRetune(const Tuning& t, int key, int32_t pBase, uint32_t nominalPhaseInc);

}  // namespace sq8l
