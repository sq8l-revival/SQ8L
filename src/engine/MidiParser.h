// MIDI event queue with sample-accurate dispatch (original unit midiParser, class CMidiParser).
//
// processEvents() converts raw MIDI messages into 16-byte records sorted by
// sample offset; during process() the master calls beginBlock(), then tick()
// once per sample (dispatching events due at that sample), then endBlock()
// which dispatches anything left over.
#pragma once

#include <cstdint>
#include <vector>

namespace sq8l {

// Record layout identical to the original (16 bytes).
struct MidiRecord {
    uint16_t type;        // 1 note on, 2 note off, 8 controller-like, 0x10 program change, 0x40 stop/reset
    uint8_t channel;
    uint8_t data1;        // key / controller number / program number
    int32_t deltaFrames;  // sample offset inside the block
    int32_t delta;        // samples since the previous dispatched record
    uint16_t ctrl;        // CC number, or 0x80 channel pressure, 0x81 pitch bend, 0x82 poly pressure
    int16_t value;        // velocity / CC value / pressure / bend (-8192..8191)
};
static_assert(sizeof(MidiRecord) == 16, "layout");

// Raw input event: VstMidiEvent essentials.
struct RawMidiEvent {
    int32_t deltaFrames;
    uint8_t data[3];
    uint8_t noteOffVelocity;
};

class MidiListener {
public:
    virtual ~MidiListener() = default;
    virtual void midiNoteOn(uint8_t channel, uint8_t key, uint8_t velocity) = 0;
    virtual void midiControl(uint8_t channel, uint8_t data1, int16_t value, int16_t ctrl) = 0;
    virtual void midiReset(int16_t value) = 0;
    // (port, issue #40) The original drops 0xC0 in processEvents and has no handler at all.
    virtual void midiProgramChange(uint8_t channel, uint8_t program) { (void)channel; (void)program; }
};

class MidiParser {
public:
    explicit MidiParser(MidiListener* listener) : listener_(listener) { events_.reserve(512); }

    void processEvents(const RawMidiEvent* ev, int32_t n);  // FUN_0044f93c
    void beginBlock();                                      // FUN_0044fca8
    void tick();                                            // FUN_0044fe88
    void endBlock();                                        // FUN_0044f7fc

    // (port, issue #40) Off keeps the original's record stream: 0xC0 is dropped at parse
    // time, so no record exists to dispatch. The gate is here rather than in the listener
    // because the record array itself is what the differential test compares.
    void setProgramChangeEnabled(bool on) { programChange_ = on; }
    bool programChangeEnabled() const { return programChange_; }

    const std::vector<MidiRecord>& records() const { return events_; }
    int32_t count() const { return count_; }

private:
    void sortBack(int32_t index);   // FUN_0044f840
    void dispatchOne();             // FUN_0044fcd0

    MidiListener* listener_;
    std::vector<MidiRecord> events_;  // +0x04 (capacity managed like the original's dynarray)
    int32_t count_ = 0;               // +0x08
    int32_t index_ = 0;               // +0x50
    int32_t counter_ = -1;            // +0x54
    bool active_ = false;             // +0x58 != nil
    bool programChange_ = false;      // (port) see setProgramChangeEnabled
};

}  // namespace sq8l
