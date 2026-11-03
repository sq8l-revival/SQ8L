#include "SoundParameterState.h"
#include <cstring>

namespace sq8l::sound {
namespace {
constexpr uint8_t kBaseMagic[] = {9, 'S', 'Q', '8', 'L', '.', 'E', 'D', 'I', 'T'};
constexpr char kMagic[] = "SQ8LAUTO";
uint32_t le(const uint8_t* p, size_t n) {
    uint32_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= uint32_t(p[i]) << (8 * i);
    return v;
}
void put(std::vector<uint8_t>& out, uint32_t v, size_t n) {
    for (size_t i = 0; i < n; ++i) out.push_back(uint8_t(v >> (8 * i)));
}
bool validBase(const uint8_t* p, size_t n) {
    if (!p) return false;
    if (n == kLegacyStateSize) return p[0x200] == 1 && p[0x201] == 0;
    return n == kCurrentStateSize && le(p, 4) == 2 &&
        std::memcmp(p + 4, kBaseMagic, sizeof kBaseMagic) == 0;
}
size_t slot(const Parameter& p) { return size_t(p.pair) * 2 + (p.codec == Codec::Free ? 0 : 1); }
bool consistent(const uint8_t* base, const PairValues& pairs) {
    Program program;
    std::memcpy(program.bytes, base + 31, kProgramSize);
    for (const auto& p : parameters()) if (p.pair >= 0 && pairs.explicitValues[slot(p)]) {
        const bool synced = read(*parameterForField(p.modeField), program) != 0;
        if (synced == (p.codec != Codec::Free) && read(p, program) != pairs.values[slot(p)]) return false;
    }
    return true;
}
}
bool parseSoundParameterState(const uint8_t* data, size_t size, ValidatedSoundParameterState& out) {
    if (!data || size > kMaximumStateSize) return false;
    const size_t baseSize = size == kLegacyStateSize ? kLegacyStateSize : kCurrentStateSize;
    if (size < baseSize || !validBase(data, baseSize)) return false;
    ValidatedSoundParameterState result;
    result.baseSize_ = baseSize;
    std::memcpy(result.base_.data(), data, baseSize);
    if (size != baseSize) {
        if (size < baseSize + 16 || std::memcmp(data + baseSize, kMagic, 8)) return false;
        const auto* h = data + baseSize;
        const uint32_t version = le(h + 8, 2), count = le(h + 10, 2), bytes = le(h + 12, 4);
        if (count > 40 || bytes != count * 8 || size != baseSize + 16 + bytes) return false;
        if (version == 1) {
            uint32_t previous = 0;
            for (uint32_t i = 0; i < count; ++i) {
                const auto* e = h + 16 + i * 8;
                const uint32_t id = le(e, 4), raw = le(e + 4, 4);
                const auto* p = parameter(id);
                // All pair domains are nonnegative, so this also rejects signed negatives.
                if (!p || p->pair < 0 || (i && id <= previous) || raw > uint32_t(p->maximum)) return false;
                int value;
                if (!canonicalize(*p, double(raw), value) || value != int64_t(raw)) return false;
                result.pairs_.values[slot(*p)] = value;
                result.pairs_.explicitValues.set(slot(*p));
                previous = id;
            }
            if (!consistent(data, result.pairs_)) return false;
        }
    }
    out = result;
    return true;
}
bool encodeSoundParameterState(const uint8_t* base, size_t size, const PairValues& pairs,
                               std::vector<uint8_t>& out) {
    if (!validBase(base, size) || !SoundParameterService::validPairs(pairs)) return false;
    const size_t count = pairs.explicitValues.count();
    if (count && size != kCurrentStateSize) return false;
    if (count && !consistent(base, pairs)) return false;
    std::vector<uint8_t> result(base, base + size);
    if (count) {
        result.insert(result.end(), kMagic, kMagic + 8);
        put(result, 1, 2); put(result, uint32_t(count), 2); put(result, uint32_t(count * 8), 4);
        for (const auto& p : parameters()) if (p.pair >= 0 && pairs.explicitValues[slot(p)]) {
            put(result, p.index, 4); put(result, uint32_t(pairs.values[slot(p)]), 4);
        }
    }
    out = std::move(result);
    return true;
}
} // namespace sq8l::sound

namespace sq8l::sound {
SoundParameterLifecycle::SoundParameterLifecycle(Synth& synth, SoundParameterService& service)
    : synth_(synth), service_(service) {
    auto& edit = synth_.editBuffer();
    if (!edit.lifecycleObserver()) {
        service_.replace(edit.current());
        edit.setLifecycleObserver(this);
        attached_ = true;
    }
}
SoundParameterLifecycle::~SoundParameterLifecycle() {
    if (attached_ && synth_.editBuffer().lifecycleObserver() == this)
        synth_.editBuffer().setLifecycleObserver(nullptr);
}
void SoundParameterLifecycle::synchronize() {
    auto& edit = synth_.editBuffer();
    if (service_.apply(edit.current())) edit.setModified(true);
}
void SoundParameterLifecycle::backupCompare() { comparePairs_ = service_.snapshot().pairs; }
void SoundParameterLifecycle::replaced(EditBuffer::Replacement kind) {
    service_.replace(synth_.editBuffer().current(),
        kind == EditBuffer::Replacement::CompareRestore ? comparePairs_ : PairValues{});
}
SoundParameterProjectSnapshot SoundParameterLifecycle::capture() {
    SoundParameterProjectSnapshot result;
    if (!attached_) return result;
    synth_.copyChunk(result.base);
    result.pairs = service_.snapshot().pairs;
    return result;
}
bool SoundParameterLifecycle::restore(const ValidatedSoundParameterState& state) {
    if (!attached_ || synth_.setChunk(state.baseData(), state.baseSize(), false) != 0) return false;
    // Base/options have completed, so only this final transaction advances state.
    return service_.replace(synth_.editBuffer().current(), state.pairs());
}
} // namespace sq8l::sound
