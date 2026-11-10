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
// (port) Switches of the instance, one bit each, in a third spare header byte. No marker is
// needed: the original writes 0 there, and a clear bit is the switch off.
constexpr size_t kChunkFlags = 0x1d;
constexpr uint8_t kChunkMtsEnabled = 1, kChunkMtsCorrectPitch = 2;
// (port, issue #40) OPTIONS -> Support MIDI Program Change. Unlike the two above, a clear bit
// is not this instance's default: a new instance responds. The bit is written whenever it is
// on, so only a chunk from the original or from an earlier port reads as off, which is the
// point - an old project keeps ignoring the program changes in its MIDI.
constexpr uint8_t kChunkProgramChange = 4;
// (port) The bank the edit buffer's patch came from, in two more bits of the same byte. The
// original's chunk stores only the program within a bank (EditBuffer 0x18), so without this a
// patch from B, C or D came back as an A slot: the right sound under the wrong name. Absent
// reads as bank A, which is both the original's chunk and a fresh instance.
constexpr uint8_t kChunkBankMask = 0x18;
constexpr int kChunkBankShift = 3;
// (port, issue #52) OPTIONS -> Output gain, in the last spare header byte, as the cut in dB.
// No marker is needed either: the original writes 0 there, and 0 dB means no cut.
constexpr size_t kChunkOutputCut = 0x1e;
// (port, issue #45) OPTIONS -> Keyboard -> Page memory. The bit is **inverted**: set means the
// switch is OFF. That is deliberate and must not be "tidied up" to match kChunkProgramChange
// three bits along, which wants the opposite. Page memory is on for a new instance and should
// be on for a project saved before it existed, so an absent bit has to read as on - and the
// default then writes nothing, which is what keeps an untouched chunk the original's bytes.
constexpr uint8_t kChunkNoPageMemory = 0x20;
// (port, issue #45) Where the editor was left, in the spare header bytes 0x0e..0x17: ten bytes
// the original writes as zero after its magic and never validates (see docs/modules/library.md).
// Written only when page memory is on and there is a place to save, so a chunk from an instance
// that never used it is byte for byte the original's. The per-sub-page focus table of
// EditorPlace does not fit and is not carried: a project needs to open where it was left, not
// to remember every page ever visited.
constexpr size_t kChunkPlaceMarker = 0x0e;
constexpr uint8_t kChunkPlaceMagic = 'K';
constexpr size_t kChunkPlacePage = 0x0f;
constexpr size_t kChunkPlaceSub = 0x10;
constexpr size_t kChunkPlaceKnob = 0x11;
constexpr size_t kChunkPlaceGroupPage = 0x12;  // three bytes: LFO, envelope, matrix
constexpr size_t kChunkPlaceGroupSub = 0x15;   // two 2-bit views, LFO in bits 0-1
constexpr uint8_t kChunkNone = 0xff;           // a field with nothing saved in it
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
    // (port, issue #52) A new instance opens at the cut last chosen (see Synth.h).
    setOutputCut(settings_.outputCutDb());
    edit_->listener = [this](EditBuffer::Event e, const Program* slot) {
        syncProgram();
        master_->editBufferEvent(static_cast<int32_t>(e), slot ? slot->bytes : nullptr);
    };
    // (port, issue #40) A program change loads through the edit buffer, exactly as the
    // editor's own program selection does, so the ring and the kill/release of the slot
    // being reused apply unchanged. Called from the parser dispatch, mid-block.
    master_->onProgramChange = [this](int program, int bankSelect) {
        if (restoreGuard()) return;
        int bank = -1;  // until a CC0 arrives: the bank the edit buffer is on
        if (bankSelect >= 0) {
            if (bankSelect >= SoundLibrary::kNumBanks) return;  // a bank there is no library for
            bank = bankSelect;
        }
        edit_->selectProgram(program, bank);
    };
    master_->setProgramChangeEnabled(true);
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

// (port) CSynth +0xb4. The original guarded effSetProgram, the one way a VST2 host pushed
// its stored program selection back after setChunk. A VST3 host restores that selection onto
// the program-change parameter, which arrives as a MIDI program change instead, so both ways
// in have to consume the guard.
bool Synth::restoreGuard() {
    const bool armed = chunkLoaded_;
    chunkLoaded_ = false;
    return armed;
}

void Synth::setProgram(int32_t index) {
    if (restoreGuard()) return;
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
    // (port) The switch bits, for the same reason. All off writes nothing, so a chunk whose
    // instance only ever ignored program changes stays byte-identical to the original's.
    const uint8_t flags = static_cast<uint8_t>((mtsEnabled_ ? kChunkMtsEnabled : 0) |
                                               (mtsCorrectPitch_ ? kChunkMtsCorrectPitch : 0) |
                                               (programChangeEnabled() ? kChunkProgramChange : 0) |
                                               (pageMemory_ ? 0 : kChunkNoPageMemory) |
                                               ((edit_->bank() << kChunkBankShift) & kChunkBankMask));
    if (flags != 0 && chunk.size() == EditBuffer::kChunkSize) chunk[kChunkFlags] = flags;
    // (port) The output cut, for the same reason. No cut writes nothing, so a chunk without
    // one stays byte-identical to the original's.
    if (outputCut_ > 0 && chunk.size() == EditBuffer::kChunkSize)
        chunk[kChunkOutputCut] = static_cast<uint8_t>(outputCut_);
    // (port, issue #45) Where the editor was left.
    if (pageMemory_ && place_.valid() && chunk.size() == EditBuffer::kChunkSize) {
        auto field = [](int8_t v) { return v < 0 ? kChunkNone : static_cast<uint8_t>(v); };
        chunk[kChunkPlaceMarker] = kChunkPlaceMagic;
        chunk[kChunkPlacePage] = field(place_.page);
        chunk[kChunkPlaceSub] = field(place_.sub);
        chunk[kChunkPlaceKnob] = field(place_.knob);
        for (int i = 0; i < 3; i++) chunk[kChunkPlaceGroupPage + size_t(i)] = field(place_.groupPage[i]);
        chunk[kChunkPlaceGroupSub] =
            static_cast<uint8_t>((place_.groupSub[0] & 3) | ((place_.groupSub[1] & 3) << 2));
    }
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
        // (port) likewise the switch bits: a chunk without them puts this instance back to
        // all off, which for the program changes is the old project's own behaviour.
        const uint8_t flags = size == EditBuffer::kChunkSize ? data[kChunkFlags] : 0;
        mtsEnabled_ = (flags & kChunkMtsEnabled) != 0;
        mtsCorrectPitch_ = (flags & kChunkMtsCorrectPitch) != 0;
        setProgramChangeEnabled((flags & kChunkProgramChange) != 0);
        // (port) The bank the patch came from, so the editor and the host name the slot the
        // chunk was saved on rather than its A-bank twin. After edit_->setChunk, which is
        // what put the program number in place.
        edit_->setBank((flags & kChunkBankMask) >> kChunkBankShift);
        // (port) likewise the output cut: a chunk without one, or with a step this build does
        // not know, puts this instance back to no cut.
        const int cut = size == EditBuffer::kChunkSize ? data[kChunkOutputCut] : 0;
        setOutputCut(Settings::validOutputCut(cut) ? cut : 0);
        // (port, issue #45) Page memory and the place. The bit is inverted (see above), so a
        // chunk from the original or from an earlier port reads as on, which is this build's
        // default; the place is restored only when the marker is there.
        pageMemory_ = !(flags & kChunkNoPageMemory);
        place_ = EditorPlace();
        if (size == EditBuffer::kChunkSize && data[kChunkPlaceMarker] == kChunkPlaceMagic) {
            auto field = [](uint8_t v) { return v == kChunkNone ? int8_t(-1) : int8_t(v); };
            place_.page = field(data[kChunkPlacePage]);
            place_.sub = field(data[kChunkPlaceSub]);
            place_.knob = field(data[kChunkPlaceKnob]);
            for (int i = 0; i < 3; i++) place_.groupPage[i] = field(data[kChunkPlaceGroupPage + size_t(i)]);
            place_.groupSub[0] = int8_t(data[kChunkPlaceGroupSub] & 3);
            place_.groupSub[1] = int8_t((data[kChunkPlaceGroupSub] >> 2) & 3);
            // The table is not carried; seed it with the one entry the chunk does hold, so the
            // page it opens on knows its parameter and the rest fills in again by browsing.
            const int i = EditorPlace::index(place_.page, place_.sub);
            if (i >= 0) place_.focusBySub[i] = place_.knob;
        }
        return 0;
    }
    return -1;
}

// (port, issue #52) see Synth.h. An unknown step is read as no cut.
void Synth::setOutputCut(int db) {
    outputCut_ = Settings::validOutputCut(db) ? db : 0;
    master_->setOutputGain(Settings::outputGainFor(outputCut_));
}

void Synth::setPolyphonyOverride(int voices) { master_->setPolyphonyOverride(voices); }

int Synth::polyphonyOverride() const { return master_->polyphonyOverride(); }

int Synth::polyphony() const { return master_->effectivePlayableVoices(); }

// (port) MTS-ESP: the snapshot stays owned by the caller, which refreshes it per block.
void Synth::setTuning(const Tuning* t) { master_->setTuning(t); }

}  // namespace sq8l
