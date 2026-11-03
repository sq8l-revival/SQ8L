#include "SQ8LPlugin.hpp"
#include "SoundParameterInstrumentation.h"

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
    : Plugin(sq8l::sound::kParameterCount, sq8l::Synth::kNumPrograms, 1),
      shared_(sq8l::SharedLibrary::acquire()),
      synth_(static_cast<float>(getSampleRate() > 0 ? getSampleRate() : 44100.0), shared_.library, shared_.settings,
             this),
      events_(1024) {
    synth_.setSampleRate(static_cast<float>(getSampleRate()));
    // (port) MTS-ESP: the engine reads this snapshot, run() refreshes it per block.
    synth_.setTuning(&mts_.tuning());
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        parameters_ = std::make_unique<sq8l::sound::SoundParameterService>(synth_.editBuffer().current());
        parameterLifecycle_ = std::make_unique<sq8l::sound::SoundParameterLifecycle>(synth_, *parameters_);
    }
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

SQ8LPlugin::~SQ8LPlugin() {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        parameterLifecycle_.reset();
        parameters_.reset();
    }
    sq8l::SharedLibrary::release();
}

namespace {
uint32_t parameterGroup(unsigned index) {
    const auto& registry = sq8l::sound::parameters();
    uint32_t group = 0;
    // Groups are assigned by first occurrence, including noncontiguous aliases.
    for (unsigned i = 0; i < registry.size(); ++i) {
        bool first = true;
        for (unsigned j = 0; j < i; ++j)
            if (std::strcmp(registry[j].group, registry[i].group) == 0) { first = false; break; }
        if (!first) continue;
        if (std::strcmp(registry[i].group, registry[index].group) == 0) return group;
        ++group;
    }
    return 0;
}
}

void SQ8LPlugin::initParameter(uint32_t index, Parameter& out) {
    const auto* p = sq8l::sound::parameter(index);
    if (!p) return;
    out.name = p->name;
    out.symbol = p->symbol;
    out.groupId = parameterGroup(index);
    out.hints = kParameterIsAutomatable | kParameterIsInteger;
    if (p->kind == sq8l::sound::Kind::Boolean) out.hints |= kParameterIsBoolean;
    out.ranges = ParameterRanges(p->defaultValue, p->minimum, p->maximum);
    if (p->kind != sq8l::sound::Kind::Integer) {
        const unsigned count = p->maximum - p->minimum + 1;
        out.enumValues.count = static_cast<uint8_t>(count);
        out.enumValues.restrictedMode = true;
        out.enumValues.values = new ParameterEnumerationValue[count];
        for (unsigned n = 0; n < count; ++n) {
            const int value = p->minimum + int(n);
            out.enumValues.values[n].value = float(value);
            out.enumValues.values[n].label = sq8l::sound::choiceLabel(*p, value).c_str();
        }
    }
}

void SQ8LPlugin::initPortGroup(uint32_t id, PortGroup& out) {
    for (const auto& p : sq8l::sound::parameters()) if (parameterGroup(p.index) == id) {
        out.name = p.group;
        out.symbol = p.symbol; // first parameter gives a stable, valid group symbol
        return;
    }
}

float SQ8LPlugin::getParameterValue(uint32_t index) const {
    SQ8L_MEASURE_LOCK(mutex_, Getter);
    int value = 0;
    parameters_->get(index, value);
    return float(value);
}

bool SQ8LPlugin::sq8lReadAcceptedParameter(uint32_t index, SQ8LAcceptedParameter& out) {
    SQ8L_MEASURE_LOCK(mutex_, Accepted);
    int value = 0;
    if (!parameters_->get(index, value)) return false;
    const auto stamp = parameters_->stamp(index);
    out = {stamp.generation, stamp.revision, float(value)};
    return true;
}

bool SQ8LPlugin::sq8lCopyParameterRefresh(uint64_t& generation, float* values, uint32_t count) {
    static_assert(SQ8LParameterRefreshHooks::kParameterCount == sq8l::sound::kParameterCount,
                  "update the adapter snapshot capacity when appending parameters");
    if (values == nullptr || count != sq8l::sound::kParameterCount) return false;
    SQ8L_MEASURE_LOCK(mutex_, Refresh);
    const uint64_t current = parameters_->generation();
    if (generation == current) return false;
    for (uint32_t i = 0; i < count; ++i) {
        int value = 0;
        parameters_->get(i, value);
        values[i] = float(value);
    }
    generation = current;
    return true;
}

namespace {
thread_local const SQ8LPlugin* echoPlugin = nullptr;
thread_local const sq8l::sound::EchoToken* echoToken = nullptr;
}
bool SQ8LPlugin::sq8lCapturePanelNotification(uint32_t index, float value,
                                              SQ8LPanelNotificationStamp& stamp) {
    if (echoPlugin != this || echoToken == nullptr || echoToken->index != index ||
        echoToken->value != value) return false;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!parameters_->currentPanelEcho(*echoToken)) return false;
    stamp = {echoToken->stamp.generation, echoToken->stamp.revision,
             echoToken->index, echoToken->value, echoToken->source};
    return true;
}
bool SQ8LPlugin::sq8lCurrentPanelNotification(const SQ8LPanelNotificationStamp& stamp) {
    const sq8l::sound::EchoToken token{
        {stamp.generation, stamp.revision, sq8l::sound::Origin::Panel},
        stamp.index, stamp.value, stamp.source};
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return parameters_->currentPanelEcho(token);
}
void SQ8LPlugin::dispatchPanelEcho(const sq8l::sound::EchoToken& token,
                                 const std::function<void()>& callback) {
    struct Restore {
        const SQ8LPlugin* plugin = echoPlugin;
        const sq8l::sound::EchoToken* token = echoToken;
        ~Restore() { echoPlugin = plugin; echoToken = token; }
    } restore;
    echoPlugin = this;
    echoToken = &token;
    callback();
}
void SQ8LPlugin::setParameterValue(uint32_t index, float value) {
    SQ8L_MEASURE_LOCK(mutex_, Setter);
    if (echoPlugin == this && echoToken) parameters_->acceptScopedPanelEcho(index, value, *echoToken);
    else parameters_->accept(index, value);
}

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
    sq8l::sound::SoundParameterProjectSnapshot snapshot;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        snapshot = parameterLifecycle_->capture();
    }
    std::vector<uint8_t> bytes;
    if (!sq8l::sound::encodeSoundParameterState(snapshot.base.data(), snapshot.base.size(), snapshot.pairs, bytes))
        return String();
    return toHex(bytes);
}

void SQ8LPlugin::setState(const char* key, const char* value) {
    if (std::strcmp(key, "editbuffer") != 0) return;
    const std::vector<uint8_t> bytes = fromHex(value);
    sq8l::sound::ValidatedSoundParameterState state;
    if (!sq8l::sound::parseSoundParameterState(bytes.data(), bytes.size(), state)) return;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    parameterLifecycle_->restore(state);
}

void SQ8LPlugin::sampleRateChanged(double sr) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    synth_.setSampleRate(static_cast<float>(sr));
}

void SQ8LPlugin::run(const float**, float** outputs, uint32_t frames, const MidiEvent* midi, uint32_t midiCount) {
    SQ8L_MEASURE_LOCK(mutex_, Render);
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
    // processEvents only parses/queues MIDI. Synth::process synchronizes this
    // lifecycle once before Master begins sample-offset dispatch. This entire
    // run owns mutex_, with no intervening release or early return, so no setter
    // can accept work between render entry and that existing application hook.
    if (!events_.empty()) synth_.processEvents(events_.data(), static_cast<int32_t>(events_.size()));
    synth_.process(outputs[0], outputs[1], static_cast<int32_t>(frames), true);
}

Plugin* createPlugin() { return new SQ8LPlugin(); }

END_NAMESPACE_DISTRHO
