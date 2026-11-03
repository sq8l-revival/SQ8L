#pragma once
#include "SoundParameterService.h"
#include <vector>

namespace sq8l::sound {
constexpr size_t kCurrentStateSize = 571, kLegacyStateSize = 544;
constexpr size_t kMaximumStateSize = 907;
// Produced only after full validation, outside engine ownership. The exact base
// is passed to Synth's existing loader; this module never serializes its options.
class ValidatedSoundParameterState {
public:
    const uint8_t* baseData() const { return base_.data(); }
    size_t baseSize() const { return baseSize_; }
    const PairValues& pairs() const { return pairs_; }
private:
    std::array<uint8_t, kCurrentStateSize> base_{};
    size_t baseSize_ = 0;
    PairValues pairs_{};
    friend bool parseSoundParameterState(const uint8_t*, size_t, ValidatedSoundParameterState&);
};
// Failure leaves output unchanged. V1 active explicit values must agree with the
// canonical base Program; inactive values are independent. Unknown framed versions
// accept base/unset caches without interpreting payload entries.
bool parseSoundParameterState(const uint8_t*, size_t, ValidatedSoundParameterState&);
// Outside lock: append to an exact valid original base copy from Synth. Empty
// explicit caches preserve it byte-for-byte. Failure leaves output unchanged.
bool encodeSoundParameterState(const uint8_t* base, size_t, const PairValues&,
                               std::vector<uint8_t>& output);
} // namespace sq8l::sound

#include "Synth.h"

namespace sq8l::sound {
// Owning, bounded snapshot: capture under engine ownership; encode after release.
struct SoundParameterProjectSnapshot {
    EditBuffer::Chunk base{};
    PairValues pairs{};
};
// Optional headless composition. Construct/destroy and call every method under the
// same caller-owned engine mutex as Synth/service. Synth and service outlive it.
// One observer per Synth; an existing observer makes attachment fail explicitly.
class SoundParameterLifecycle final : private EditBuffer::LifecycleObserver {
public:
    SoundParameterLifecycle(Synth&, SoundParameterService&);
    ~SoundParameterLifecycle() override;
    SoundParameterLifecycle(const SoundParameterLifecycle&) = delete;
    SoundParameterLifecycle& operator=(const SoundParameterLifecycle&) = delete;
    bool attached() const { return attached_; }
    SoundParameterProjectSnapshot capture();
    bool restore(const ValidatedSoundParameterState&);
private:
    void synchronize() override;
    void backupCompare() override;
    void replaced(EditBuffer::Replacement) override;
    Synth& synth_;
    SoundParameterService& service_;
    PairValues comparePairs_{};
    bool attached_ = false;
};
} // namespace sq8l::sound
