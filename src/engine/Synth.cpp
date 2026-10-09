#include "Synth.h"

#include <cstddef>
#include <cstring>

#include "Fpu.h"
#include "WaveRom.h"

namespace sq8l {

namespace {
constexpr float kControlRate = 83.592575f;  // 0x42a72f66, rate of LFOs/envelopes/followers
// (port) the per-instance playable voices in the host chunk: 0x1b..0x1e are 0 in the
// original's chunk and never read by it (see docs/modules/library.md).
constexpr size_t kChunkPolyMarker = 0x1b;
constexpr size_t kChunkPolyVoices = 0x1c;
constexpr uint8_t kChunkPolyMagic = 'V';
// (port) OPTIONS -> MTS-ESP, in a third spare header byte. No marker is needed: the original
// writes 0 there, and 0 means both switches off, which is their default.
constexpr size_t kChunkMts = 0x1d;
constexpr uint8_t kChunkMtsEnabled = 1, kChunkMtsCorrectPitch = 2;
}  // namespace

// The master's view of the Cdoc parameter block and of the LFO inputs alias the module
// storage (identical layouts, checked here).
static_assert(sizeof(MasterDocParams) == sizeof(DocVoiceParams), "Doc params layout");
static_assert(offsetof(Lfo, smoothIn) - offsetof(Lfo, freqIn) == 0x34, "Lfo input block layout");
static_assert(sizeof(LfoParams) == 0x38, "LfoParams layout");

SynthModules::SynthModules() : doc(kOriginalVoiceSlots, kMaxVoiceSlots) {
    RoundToNearest rn;
    // Master constructor order: Cdoc, then per voice the follower, 4 LFOs (with the
    // ROM wave callback), 4 envelopes, (filters,) the amp.
    for (int v = 0; v < kMaxVoiceSlots; v++) {
        foll[v].init(kControlRate);
        for (int i = 0; i < 4; i++) {
            lfo[v][i].construct(kControlRate);
            lfo[v][i].waveSource = &lfoWaveLocation;
        }
        for (int i = 0; i < 4; i++) env[v][i].init(kControlRate);
        amp[v].init();
    }
}

MasterDocParams& SynthModules::docVoiceParams(int v) {
    return *reinterpret_cast<MasterDocParams*>(doc.params(static_cast<uint32_t>(v)));
}

LfoParams& SynthModules::lfoParams(int v, int i) {
    return *reinterpret_cast<LfoParams*>(&lfo[v][i].freqIn);
}

Synth::Synth(float sampleRate, SoundLibrary* library, const Settings* settings, MasterNotify* notify) {
    if (library) {
        library_ = library;
    } else {
        ownLibrary_ = std::make_unique<SoundLibrary>();
        library_ = ownLibrary_.get();
    }
    if (settings) settings_ = *settings;
    modules_ = std::make_unique<SynthModules>();
    edit_ = std::make_unique<EditBuffer>(library_);
    {
        RoundToNearest rn;
        // CSynth ctor: master created with Round(sampleRate) of the AudioEffect (44100 default).
        master_ = std::make_unique<Master>(*modules_, fistp(sampleRate), settings_.synth, notify);
        // (port) the port's voice layout (VoiceSlots.h): the playable voices then come from
        // the program (EMU -> VOICES) or from the per-instance override, and the original's
        // 8 are what a program made by the SQ8L, a bank file or a SysEx import asks for.
        master_->setPortLayout();
    }
    edit_->listener = [this](EditBuffer::Event e, const Program* slot) {
        syncProgram();
        master_->editBufferEvent(static_cast<int32_t>(e), slot ? slot->bytes : nullptr);
    };
    syncProgram();
    // End of the master constructor: select program A000 (FUN_00460db8(editbuf, 0, -1)).
    edit_->selectProgram(0, -1);
    syncProgram();
}

void Synth::syncProgram() {
    master_->setCurrentProgram(edit_->current().bytes, static_cast<uint16_t>(edit_->programNumber()));
}

void Synth::setSampleRate(float sr) {
    RoundToNearest rn;
    master_->setSampleRate(sr);
}

void Synth::processEvents(const RawMidiEvent* ev, int32_t n) { master_->processEvents(ev, n); }

void Synth::process(float* outL, float* outR, int32_t n, bool replacing) {
    AudioFpuScope fpu;  // no FTZ/DAZ; the master sets round-toward-zero itself
    master_->process(outL, outR, n, replacing);
}

void Synth::setProgram(int32_t index) {
    if (chunkLoaded_) {
        chunkLoaded_ = false;
        return;
    }
    edit_->selectIndex(index);
    syncProgram();
}

int32_t Synth::program() const { return edit_->libraryIndex(-1, -1); }

std::string Synth::programName() const { return edit_->name(0); }

std::string Synth::programNameIndexed(int32_t index) const { return library_->programName(index); }

void Synth::setProgramName(const std::string& name) {
    edit_->setName(0, name);
    syncProgram();
}

std::vector<uint8_t> Synth::getChunk() {
    std::vector<uint8_t> chunk = edit_->getChunk();
    // (port) OPTIONS -> Polyphony belongs to this instance, so it travels in the host chunk,
    // which every plugin format saves (the VST2 chunk is these bytes verbatim). It goes in
    // two of the four spare header bytes that the original writes as 0 and never reads, and
    // only when there is an override: a chunk without one stays byte-identical to the
    // original's, which is what the differential tests compare.
    const int32_t ovr = master_->polyphonyOverride();
    if (ovr > 0 && chunk.size() == EditBuffer::kChunkSize) {
        chunk[kChunkPolyMarker] = kChunkPolyMagic;
        chunk[kChunkPolyVoices] = static_cast<uint8_t>(ovr);
    }
    // (port) The MTS-ESP switches, for the same reason. Both off writes nothing, so a chunk
    // without them stays byte-identical to the original's.
    const uint8_t mts = static_cast<uint8_t>((mtsEnabled_ ? kChunkMtsEnabled : 0) |
                                             (mtsCorrectPitch_ ? kChunkMtsCorrectPitch : 0));
    if (mts != 0 && chunk.size() == EditBuffer::kChunkSize) chunk[kChunkMts] = mts;
    return chunk;
}

int32_t Synth::setChunk(const uint8_t* data, size_t size) {
    const int n = edit_->setChunk(data, size);
    syncProgram();
    if (n > 0) {
        chunkLoaded_ = true;
        // (port) the override this chunk carries, if any (see getChunk). A chunk written by
        // the original, by an older port or by an instance set to "set by program" has none,
        // and must put this instance back to that.
        int32_t ovr = 0;
        if (size == EditBuffer::kChunkSize && data[kChunkPolyMarker] == kChunkPolyMagic) {
            const int32_t v = data[kChunkPolyVoices];
            if (v >= kMinPlayableVoices && v <= kMaxPlayableVoices) ovr = v;
        }
        master_->setPolyphonyOverride(ovr);
        // (port) likewise the MTS-ESP switches: a chunk without them puts this instance back
        // to both off.
        const uint8_t mts = size == EditBuffer::kChunkSize ? data[kChunkMts] : 0;
        mtsEnabled_ = (mts & kChunkMtsEnabled) != 0;
        mtsCorrectPitch_ = (mts & kChunkMtsCorrectPitch) != 0;
        return 0;
    }
    return -1;
}

void Synth::setPolyphonyOverride(int voices) { master_->setPolyphonyOverride(voices); }

int Synth::polyphonyOverride() const { return master_->polyphonyOverride(); }

int Synth::polyphony() const { return master_->effectivePlayableVoices(); }

// (port) MTS-ESP: the snapshot stays owned by the caller, which refreshes it per block.
void Synth::setTuning(const Tuning* t) { master_->setTuning(t); }

}  // namespace sq8l
