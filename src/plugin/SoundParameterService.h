#pragma once
#include "SoundParameters.h"
#include <bitset>
#include <cstdint>
#include <cassert>


namespace sq8l::sound {
// Fixed copy records. Member slot = pair ordinal * 2 + (Sync ? 1 : 0).
struct PairValues {
    std::array<int, kTempoPairCount * 2> values{};
    std::bitset<kTempoPairCount * 2> explicitValues{};
};
enum class Origin { Host, Panel, Replacement };
struct EditStamp {
    uint64_t generation = 0, revision = 0;
    Origin origin = Origin::Replacement;
};
struct EchoToken {
    EditStamp stamp{};
    unsigned index = kParameterCount;
    int value = 0;
    const void* source = nullptr; // identity only, never dereferenced; bridge uses synchronously
};
// GUI-thread staging only; the service itself remains fixed-storage and headless.
// Enter after locking, leave AFTER unlocking. Recursive scopes and modal releases
// share this depth. Drain pops before invoking a callback, preserving reentrant order.
struct PanelNotification {
    enum Kind { Begin, Value, End } kind;
    unsigned index;
    EchoToken token{};
};
class PanelNotificationQueue {
public:
    void engineEntered() { ++depth_; }
    void engineLeft() { assert(depth_ > 0); --depth_; }
    int engineDepth() const { return depth_; }
    // At capacity, merge buffered histories per identity into a balanced gesture
    // with its latest value. Already-delivered begins stay open until their end.
    void push(PanelNotification event);
    template<class Callback> void drain(Callback callback) {
        if (depth_ != 0 || draining_) return;
        struct Guard { bool& flag; ~Guard() { flag = false; } } guard{draining_};
        draining_ = true;
        while (depth_ == 0 && count_ != 0) {
            const auto event = events_[head_];
            head_ = (head_ + 1) % kCapacity;
            --count_;
            if (event.kind == PanelNotification::Begin) deliveredOpen_.set(event.index);
            if (event.kind == PanelNotification::End) deliveredOpen_.reset(event.index);
            callback(event);
        }
    }
private:
    int depth_ = 0;
    bool draining_ = false;
    static constexpr size_t kCapacity = kParameterCount * 3;
    std::array<PanelNotification, kCapacity> events_{};
    size_t head_ = 0, count_ = 0;
    std::bitset<kParameterCount> deliveredOpen_{};
};
enum class Acceptance { Rejected, Accepted, Echo };
struct AcceptResult {
    Acceptance status = Acceptance::Rejected;
    EchoToken token{};
};
struct SoundParameterSnapshot {
    Program program{}; // accepted projection; copy only, never replay/acknowledge it
    std::array<int, kParameterCount> values{};
    std::array<EditStamp, kParameterCount> edits{};
    PairValues pairs{};
    uint64_t generation = 0, revision = 0;
    bool pending = false;
};

// ALL methods, including reads/copies, require the caller's instance engine mutex.
// No internal lock, retained Program pointer, callback, allocation or deferred ack.
// Apply and replacement must run within that same ownership scope. A retained
// snapshot is an owning observation and has no application API.
class SoundParameterService {
public:
    explicit SoundParameterService(const Program&);
    AcceptResult accept(unsigned, double, Origin = Origin::Host,
                        const EchoToken* synchronousEcho = nullptr);
    bool get(unsigned, int&) const;
    bool currentPanelEcho(const EchoToken&) const;
    EditStamp stamp(unsigned index) const { return index < kParameterCount ? edits_[index] : EditStamp{}; }
    // Explicit bridge delivery may race a replacement/new host edit after unlock.
    // A stale scoped echo is discarded, while unrelated host values stay ordinary.
    AcceptResult acceptScopedPanelEcho(unsigned, double, const EchoToken&);
    SoundParameterSnapshot snapshot() const;
    // Caller supplies the current Program of this generation. Returns whether
    // accepted work was consumed, including cache-only/equal-value assignments.
    bool apply(Program&);
    // Invalid caches reject without changing any state. Does not write Program.
    bool replace(const Program&, const PairValues& = {});
    bool hasPending() const { return dirty_.any(); }
    uint64_t generation() const { return generation_; }
    static bool validPairs(const PairValues&);
private:
    struct PairBinding { unsigned free = 0, sync = 0, mode = 0; };
    std::array<PairBinding, kTempoPairCount> bindings_{};
    Program applied_{};
    Program projected_{};
    std::array<int, kParameterCount> accepted_{}, pending_{};
    std::array<EditStamp, kParameterCount> edits_{};
    std::bitset<kParameterCount> dirty_{};
    PairValues pairs_{};
    uint64_t generation_ = 0, revision_ = 0;
    void project(Program&) const;
    void refresh();
};
} // namespace sq8l::sound
