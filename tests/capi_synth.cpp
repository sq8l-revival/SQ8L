// C API: the complete engine (Synth), for full-render differential tests.
#include <cstdint>
#include <cstring>

#include "Synth.h"

#include "capi_export.h"

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
