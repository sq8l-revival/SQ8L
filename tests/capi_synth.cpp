// C API: the complete engine (Synth), for full-render differential tests.
#include <cstdint>
#include <cstring>

#include "Synth.h"

#include "capi_export.h"

// (port) MTS-ESP. Test-only: a single snapshot, so one synth at a time per process.
static sq8l::Tuning g_tuning;

SQ8L_API void sq8l_synth_set_tuning(void* s, int32_t enabled, int32_t absolute, int32_t connected,
                                    const double* ratio, const uint32_t* filtered) {
    g_tuning.enabled = enabled != 0;
    g_tuning.correctNativeOffsets = absolute != 0;
    g_tuning.connected = connected != 0;
    for (int i = 0; i < 128; i++) g_tuning.ratio[i] = ratio[i];
    for (int i = 0; i < 4; i++) g_tuning.filtered[i] = filtered[i];
    static_cast<sq8l::Synth*>(s)->setTuning(&g_tuning);
}

SQ8L_API uint32_t sq8l_synth_voice_phase_inc(void* s, int32_t slot) {
    return static_cast<sq8l::Synth*>(s)->modules().doc.clock[slot].phaseInc;
}

// (port) OPTIONS -> MTS-ESP, per instance and carried by the chunk.
SQ8L_API void sq8l_synth_set_mts(void* s, int32_t enabled, int32_t correctPitch) {
    sq8l::Synth& sy = *static_cast<sq8l::Synth*>(s);
    sy.setMtsEnabled(enabled != 0);
    sy.setMtsCorrectPitch(correctPitch != 0);
}

SQ8L_API void sq8l_synth_get_mts(void* s, int32_t* enabled, int32_t* correctPitch) {
    const sq8l::Synth& sy = *static_cast<sq8l::Synth*>(s);
    *enabled = sy.mtsEnabled() ? 1 : 0;
    *correctPitch = sy.mtsCorrectPitch() ? 1 : 0;
}

SQ8L_API int32_t sq8l_synth_active_voices(void* s) {
    return static_cast<sq8l::Synth*>(s)->master().activeVoiceCount();
}

// The slot of the first voice playing `key`, or -1. The port layout maps the playable voices
// through Master::slotMap_ over 128 slots, so a test cannot assume a note lands in slot 0.
SQ8L_API int32_t sq8l_synth_slot_of_key(void* s, int32_t key) {
    sq8l::Master& m = static_cast<sq8l::Synth*>(s)->master();
    for (int32_t i = 0; i < sq8l::Master::kMaxVoices; i++) {
        const sq8l::Voice& v = m.voice(i);
        if (v.active != 0 && v.key == static_cast<int8_t>(key)) return i;
    }
    return -1;
}

SQ8L_API void* sq8l_synth_new(float sampleRate) { return new sq8l::Synth(sampleRate); }
SQ8L_API void sq8l_synth_free(void* s) { delete static_cast<sq8l::Synth*>(s); }
SQ8L_API void sq8l_synth_set_sample_rate(void* s, float sr) { static_cast<sq8l::Synth*>(s)->setSampleRate(sr); }
SQ8L_API void sq8l_synth_set_program(void* s, int32_t i) { static_cast<sq8l::Synth*>(s)->setProgram(i); }
SQ8L_API int32_t sq8l_synth_program(void* s) { return static_cast<sq8l::Synth*>(s)->program(); }

SQ8L_API void sq8l_synth_events(void* s, const sq8l::RawMidiEvent* ev, int32_t n) {
    static_cast<sq8l::Synth*>(s)->processEvents(ev, n);
}

SQ8L_API void sq8l_synth_process(void* s, float* l, float* r, int32_t n, int32_t replacing) {
    static_cast<sq8l::Synth*>(s)->process(l, r, n, replacing != 0);
}

SQ8L_API int32_t sq8l_synth_get_chunk(void* s, uint8_t* out, int32_t max) {
    auto c = static_cast<sq8l::Synth*>(s)->getChunk();
    int32_t n = static_cast<int32_t>(c.size());
    std::memcpy(out, c.data(), static_cast<size_t>(n < max ? n : max));
    return n;
}

SQ8L_API int32_t sq8l_synth_set_chunk(void* s, const uint8_t* data, int32_t n) {
    return static_cast<sq8l::Synth*>(s)->setChunk(data, static_cast<size_t>(n));
}

SQ8L_API int32_t sq8l_synth_name(void* s, char* out, int32_t max) {
    auto n = static_cast<sq8l::Synth*>(s)->programName();
    std::strncpy(out, n.c_str(), static_cast<size_t>(max));
    return static_cast<int32_t>(n.size());
}
