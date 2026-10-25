#include "SQ8LPlugin.hpp"

#include <cmath>
#include <cstring>
#include <limits>

#include "DistrhoPluginUtils.hpp"

START_NAMESPACE_DISTRHO

namespace {

String toHex(const std::vector<uint8_t>& bytes) {
    static const char digits[] = "0123456789abcdef";
    std::vector<char> s(bytes.size() * 2 + 1);
    for (size_t i = 0; i < bytes.size(); i++) {
        s[2 * i] = digits[bytes[i] >> 4];
        s[2 * i + 1] = digits[bytes[i] & 15];
    }
    s[bytes.size() * 2] = '\0';
    return String(s.data());
}

std::vector<uint8_t> fromHex(const char* s) {
    std::vector<uint8_t> out;
    const auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        c = static_cast<char>(c | 0x20);
        return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    };
    for (size_t i = 0; s[i] && s[i + 1]; i += 2) {
        const int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) break;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

void copyName(char* out, const std::string& s) {
    std::strncpy(out, s.c_str(), 24);
    out[23] = '\0';
}

}  // namespace

SQ8LPlugin::SQ8LPlugin()
    : Plugin(0, sq8l::Synth::kNumPrograms, 1),
      shared_(sq8l::SharedLibrary::acquire()),
      synth_(static_cast<float>(getSampleRate() > 0 ? getSampleRate() : 44100.0), shared_.library, shared_.settings,
             this),
      events_(1024) {
    synth_.setSampleRate(static_cast<float>(getSampleRate()));
    // (port) MTS-ESP: the engine reads this snapshot, run() refreshes it per block.
    synth_.setTuning(&mts_.tuning());
    const char* format = getPluginFormatName();
    if (std::strcmp(format, "VST2") == 0 || std::strcmp(format, "VST3") == 0)
        bbtStyle_ = BbtStyle::Vst;
    else if (std::strcmp(format, "CLAP") == 0 || std::strcmp(format, "AudioUnit") == 0)
        bbtStyle_ = BbtStyle::Quarters;
}

// VST2 and VST3 split a bar of int(num * 4 / denom) quarters into num beats, CLAP and AU count
// quarter notes as the beats, and LV2 passes the host's own bar and beat through. These only
// differ for a meter whose denominator is not 4.
double SQ8LPlugin::songQuarters(const TimePosition& position) const {
    const TimePosition::BarBeatTick& t = position.bbt;
    double q = std::numeric_limits<double>::quiet_NaN();
    if (!position.playing || !t.valid || !(t.ticksPerBeat > 0) || !(t.beatsPerBar > 0) || !(t.beatType > 0))
        return q;
    const double beats = (t.beat - 1) + t.tick / t.ticksPerBeat;
    switch (bbtStyle_) {
    case BbtStyle::Vst: {
        const int perBar = static_cast<int>(t.beatsPerBar) * 4 / static_cast<int>(t.beatType);
        if (perBar > 0) q = (t.bar - 1) * perBar + beats * perBar / t.beatsPerBar;
        break;
    }
    case BbtStyle::Quarters:
        q = (t.bar - 1) * static_cast<double>(t.beatsPerBar) + beats;
        break;
    case BbtStyle::Signature:
        q = ((t.bar - 1) * static_cast<double>(t.beatsPerBar) + beats) * 4.0 / t.beatType;
        break;
    }
    return q;
}

SQ8LPlugin::~SQ8LPlugin() { sq8l::SharedLibrary::release(); }

void SQ8LPlugin::vst2SetProgram(int32_t index) {
    if (index < 0 || index >= sq8l::Synth::kNumPrograms) return;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    synth_.setProgram(index);
}

int32_t SQ8LPlugin::vst2GetProgram() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return synth_.program();
}

void SQ8LPlugin::vst2SetProgramName(const char* name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    synth_.setProgramName(name);
}

void SQ8LPlugin::vst2GetProgramName(char* out) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    copyName(out, synth_.programName());
}

bool SQ8LPlugin::vst2GetProgramNameIndexed(int32_t index, char* out) {
    if (index < 0 || index >= sq8l::Synth::kNumPrograms) return false;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    copyName(out, synth_.programNameIndexed(index));
    return true;
}

void SQ8LPlugin::initProgramName(uint32_t index, String& name) {
    name = synth_.programNameIndexed(static_cast<int32_t>(index)).c_str();
}

void SQ8LPlugin::loadProgram(uint32_t index) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    synth_.setProgram(static_cast<int32_t>(index));
}

void SQ8LPlugin::initState(uint32_t index, State& state) {
    if (index == 0) {
        state.key = "editbuffer";
        state.defaultValue = "";
        state.label = "Edit buffer";
        state.hints = kStateIsOnlyForDSP;
    }
}

// The chunk also carries OPTIONS -> Polyphony (Synth::getChunk), so one state key is all the
// plugin needs: the VST2 chunk is this value verbatim (DISTRHO_PLUGIN_VST2_RAW_CHUNK_KEY).
String SQ8LPlugin::getState(const char* key) const {
    if (std::strcmp(key, "editbuffer") != 0) return String();
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return toHex(const_cast<sq8l::Synth&>(synth_).getChunk());
}

void SQ8LPlugin::setState(const char* key, const char* value) {
    if (std::strcmp(key, "editbuffer") != 0) return;
    const std::vector<uint8_t> bytes = fromHex(value);
    if (bytes.empty()) return;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    synth_.setChunk(bytes.data(), bytes.size());
}

void SQ8LPlugin::sampleRateChanged(double sr) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    synth_.setSampleRate(static_cast<float>(sr));
}

void SQ8LPlugin::run(const float**, float** outputs, uint32_t frames, const MidiEvent* midi, uint32_t midiCount) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // (port) MTS-ESP: one snapshot per block, before the events are dispatched, so the MIDI
    // and voice paths never call into the client.
    mts_.refresh(synth_.mtsEnabled(), synth_.mtsCorrectPitch());
    const TimePosition& position = getTimePosition();
    const double quarters = songQuarters(position);
    synth_.master().setTransport(position.bbt.beatsPerMinute, std::isfinite(quarters), quarters);
    events_.clear();
    for (uint32_t i = 0; i < midiCount; i++) {
        const MidiEvent& m = midi[i];
        if (m.size == 0 || m.size > MidiEvent::kDataSize) continue;
        sq8l::RawMidiEvent e{};
        e.deltaFrames = static_cast<int32_t>(m.frame);
        for (uint32_t j = 0; j < 3 && j < m.size; j++) e.data[j] = m.data[j];
        events_.push_back(e);
    }
    if (!events_.empty()) synth_.processEvents(events_.data(), static_cast<int32_t>(events_.size()));
    synth_.process(outputs[0], outputs[1], static_cast<int32_t>(frames), true);
}

Plugin* createPlugin() { return new SQ8LPlugin(); }

END_NAMESPACE_DISTRHO
