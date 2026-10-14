#include "MidiParser.h"

namespace sq8l {

void MidiParser::processEvents(const RawMidiEvent* ev, int32_t n) {
    if (static_cast<int32_t>(events_.size()) < n) events_.resize(n + 0x200);
    endBlock();
    int32_t last = 0;  // deltaFrames of the last record kept in order
    for (int32_t i = 0; i < n; i++) {
        const RawMidiEvent& e = ev[i];
        const uint8_t status = e.data[0];
        MidiRecord r{};
        r.channel = status & 0x0F;
        bool known = true;
        switch (status & 0xF0) {
        case 0x80:
            r.type = 2;
            r.data1 = e.data[1];
            r.value = e.data[2];
            break;
        case 0x90:
            r.value = e.data[2];
            if (r.value == 0) {
                r.type = 2;
                r.value = e.noteOffVelocity;
            } else {
                r.type = 1;
            }
            r.data1 = e.data[1];
            break;
        case 0xA0:
            r.type = 8;
            r.data1 = e.data[1];
            r.value = e.data[2];
            r.ctrl = 0x82;
            break;
        case 0xB0:
            r.type = 8;
            r.data1 = e.data[1];
            r.value = e.data[2];
            r.ctrl = r.data1;
            break;
        case 0xC0:
            // (port, issue #40) Dropped like every other unknown status unless enabled.
            if (!programChange_) {
                known = false;
                break;
            }
            r.type = 0x10;
            r.data1 = e.data[1] & 0x7f;
            break;
        case 0xD0:
            r.type = 8;
            r.value = e.data[1];
            r.ctrl = 0x80;
            break;
        case 0xE0:
            r.type = 8;
            r.value = static_cast<int16_t>((e.data[1] | (e.data[2] << 7)) - 0x2000);
            r.ctrl = 0x81;
            break;
        case 0xF0:
            // only stop (0xFC) and reset (0xFF) are kept; other system messages are dropped
            // silently but still counted as handled (the original's flag stays set).
            if (status == 0xFC || status == 0xFF) {
                r.type = 0x40;
                r.value = 0;
            }
            break;
        default:
            known = false;
            break;
        }
        if (!known) continue;
        // (capture mode for the editor's MIDI dialogs is not part of the engine)
        r.deltaFrames = e.deltaFrames;
        r.delta = e.deltaFrames - last;
        events_[count_] = r;
        count_++;
        if (r.delta < 0) {
            sortBack(count_ - 1);
        } else {
            last = e.deltaFrames;
        }
    }
}

void MidiParser::sortBack(int32_t index) {
    if (index < 0 || index >= count_) return;
    MidiRecord rec = events_[index];
    int32_t d = rec.delta;
    if (d >= 0) return;
    int32_t pos = 0;
    for (int32_t j = index - 1; j >= 0; j--) {
        pos = j;
        d += events_[j].delta;
        if (d >= 0) break;
        pos = 0;
    }
    if (d < 0) d = 0;
    if (pos == index) return;
    rec.delta = d;
    for (int32_t j = index; j > pos; j--) events_[j] = events_[j - 1];
    events_[pos] = rec;
    if (pos < 1) {
        events_[pos].deltaFrames = d;
    } else {
        events_[pos].deltaFrames = d + events_[pos - 1].deltaFrames;
    }
}

void MidiParser::beginBlock() {
    index_ = 0;
    if (count_ > 0) {
        active_ = true;
        counter_ = events_[0].delta;
    } else {
        active_ = false;
        counter_ = -1;
    }
}

void MidiParser::dispatchOne() {
    if (!active_) {
        counter_ = -1;
        return;
    }
    const MidiRecord& r = events_[index_];
    switch (r.type) {
    case 1:
        listener_->midiNoteOn(r.channel, r.data1, static_cast<uint8_t>(r.value));
        break;
    case 2:
        listener_->midiNoteOn(r.channel, r.data1, 0);  // no separate note-off handler
        break;
    case 8:
        listener_->midiControl(r.channel, r.data1, r.value, static_cast<int16_t>(r.ctrl));
        break;
    case 0x10:  // (port, issue #40)
        listener_->midiProgramChange(r.channel, r.data1);
        break;
    case 0x40:
        listener_->midiReset(r.value);
        break;
    default:
        break;
    }
    index_++;
    if (index_ < count_) {
        counter_ = events_[index_].delta;
    } else {
        active_ = false;
        counter_ = -1;
    }
}

void MidiParser::tick() {
    if (!active_) return;
    while (counter_ == 0) dispatchOne();
    counter_--;
}

void MidiParser::endBlock() {
    if (index_ < count_) {
        for (int32_t j = index_; j < count_; j++) events_[j].delta = 0;
        while (active_) {
            counter_ = 0;
            tick();
        }
    }
    count_ = 0;
}

}  // namespace sq8l
