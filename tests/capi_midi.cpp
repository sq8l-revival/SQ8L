// C API: MIDI parser with a recording listener.
#include <cstdint>
#include <vector>

#include "MidiParser.h"

#include "capi_export.h"

namespace {
struct Recorder : sq8l::MidiListener {
    std::vector<int32_t> log;  // (tick, kind, a, b, c, d)
    int32_t tick = 0;
    void midiNoteOn(uint8_t ch, uint8_t key, uint8_t vel) override { log.insert(log.end(), {tick, 1, ch, key, vel, 0}); }
    void midiControl(uint8_t ch, uint8_t d1, int16_t v, int16_t c) override { log.insert(log.end(), {tick, 8, ch, d1, v, c}); }
    void midiReset(int16_t v) override { log.insert(log.end(), {tick, 0x40, v, 0, 0, 0}); }
};
struct Box {
    Recorder rec;
    sq8l::MidiParser parser{&rec};
};
}  // namespace

SQ8L_API void* sq8l_midi_new() { return new Box; }
SQ8L_API void sq8l_midi_free(void* b) { delete static_cast<Box*>(b); }

SQ8L_API void sq8l_midi_process_events(void* b, const sq8l::RawMidiEvent* ev, int32_t n) {
    static_cast<Box*>(b)->parser.processEvents(ev, n);
}

SQ8L_API int32_t sq8l_midi_records(void* b, sq8l::MidiRecord* out, int32_t max) {
    auto& p = static_cast<Box*>(b)->parser;
    int32_t n = p.count() < max ? p.count() : max;
    for (int32_t i = 0; i < n; i++) out[i] = p.records()[i];
    return p.count();
}

// Run one block of n samples; returns the number of logged ints.
SQ8L_API int32_t sq8l_midi_run_block(void* b, int32_t n, int32_t* log, int32_t max) {
    auto* box = static_cast<Box*>(b);
    box->rec.log.clear();
    box->parser.beginBlock();
    for (int32_t i = 0; i < n; i++) {
        box->rec.tick = i;
        box->parser.tick();
    }
    box->rec.tick = n;
    box->parser.endBlock();
    int32_t k = static_cast<int32_t>(box->rec.log.size());
    for (int32_t i = 0; i < k && i < max; i++) log[i] = box->rec.log[i];
    return k;
}
