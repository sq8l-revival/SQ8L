#include "SoundParameterInstrumentation.h"
#include "SoundParameterService.h"

namespace sq8l::sound {
namespace {
size_t slot(const Parameter& p) { return size_t(p.pair) * 2 + (p.codec == Codec::Free ? 0 : 1); }
}
bool SoundParameterService::validPairs(const PairValues& values) {
    for (const auto& p : parameters()) if (p.pair >= 0 && values.explicitValues[slot(p)]) {
        int v;
        if (!canonicalize(p, values.values[slot(p)], v) || v != values.values[slot(p)]) return false;
    }
    return true;
}
SoundParameterService::SoundParameterService(const Program& program) {
    for (const auto& p : parameters()) if (p.pair >= 0) {
        auto& b = bindings_[size_t(p.pair)];
        (p.codec == Codec::Free ? b.free : b.sync) = p.index;
        b.mode = parameterForField(p.modeField)->index;
    }
    replace(program);
}
bool SoundParameterService::replace(const Program& program, const PairValues& pairs) {
    SQ8L_MEASURE_SCOPE(Replacement);
    if (!validPairs(pairs)) return false;
    applied_ = projected_ = program;
    pairs_ = pairs;
    dirty_.reset();
    ++generation_;
    ++revision_;
    for (auto& e : edits_) e = {generation_, revision_, Origin::Replacement};
    refresh();
    return true;
}
void SoundParameterService::project(Program& program) const {
    // Stage modes/direct values before selecting any pair's final active member.
    for (const auto& p : parameters())
        if (p.pair < 0 && dirty_[p.index]) write(p, program, pending_[p.index]);
    for (size_t n = 0; n < bindings_.size(); ++n) {
        const auto& b = bindings_[n];
        if (!dirty_[b.free] && !dirty_[b.sync] && !dirty_[b.mode]) continue;
        const bool sync = read(*parameter(b.mode), program) != 0;
        const size_t s = n * 2 + (sync ? 1 : 0);
        if (pairs_.explicitValues[s])
            write(*parameter(sync ? b.sync : b.free), program, pairs_.values[s]);
    }
}
void SoundParameterService::refresh() {
    SQ8L_MEASURE_SCOPE(Projection);
    projected_ = applied_;
    project(projected_);
    for (const auto& p : parameters()) {
        accepted_[p.index] = p.pair >= 0 && pairs_.explicitValues[slot(p)]
            ? pairs_.values[slot(p)] : read(p, projected_);
    }
}
AcceptResult SoundParameterService::accept(unsigned index, double input, Origin origin,
                                          const EchoToken* echo) {
    const auto* p = parameter(index);
    int value;
    if (!p || !canonicalize(*p, input, value)) return {};
    // Equality alone never identifies independent host intent as an echo.
    if (origin == Origin::Host && echo && echo->index == index && echo->value == value && currentPanelEcho(*echo))
        return {Acceptance::Echo, *echo};
    pending_[index] = value;
    dirty_.set(index);
    if (p->pair >= 0) {
        pairs_.values[slot(*p)] = value;
        pairs_.explicitValues.set(slot(*p));
    }
    edits_[index] = {generation_, ++revision_, origin};
    refresh();
    return {Acceptance::Accepted, {edits_[index], index, value, this}};
}
AcceptResult SoundParameterService::acceptScopedPanelEcho(unsigned index, double input, const EchoToken& echo) {
    const auto* p = parameter(index);
    int value;
    if (!p || !canonicalize(*p, input, value)) return {};
    if (echo.index == index && echo.value == value) {
        if (!currentPanelEcho(echo)) return {};
        return accept(index, input, Origin::Host, &echo);
    }
    return accept(index, input, Origin::Host);
}
bool SoundParameterService::currentPanelEcho(const EchoToken& echo) const {
    if (echo.index >= kParameterCount || echo.source != this) return false;
    const auto& last = edits_[echo.index];
    return echo.stamp.origin == Origin::Panel && last.origin == Origin::Panel &&
           echo.stamp.generation == generation_ && echo.stamp.generation == last.generation &&
           echo.stamp.revision == last.revision && accepted_[echo.index] == echo.value;
}
void PanelNotificationQueue::push(PanelNotification event) {
    if (event.index >= kParameterCount) return;
    if (count_ < kCapacity) {
        events_[(head_ + count_++) % kCapacity] = event;
        return;
    }
    struct Fold { bool seen = false, began = false, open = false, value = false; PanelNotification last{}; };
    std::array<Fold, kParameterCount> folded{};
    std::array<unsigned, kParameterCount> order{};
    size_t identities = 0;
    auto fold = [&](const PanelNotification& e) {
        auto& f = folded[e.index];
        if (!f.seen) {
            f.seen = true; f.open = deliveredOpen_[e.index];
            order[identities++] = e.index;
        }
        if (e.kind == PanelNotification::Begin) { f.began = true; f.open = true; }
        if (e.kind == PanelNotification::End) f.open = false;
        if (e.kind == PanelNotification::Value) { f.last = e; f.value = true; }
    };
    for (size_t n = 0; n < count_; ++n) fold(events_[(head_ + n) % kCapacity]);
    fold(event);
    head_ = count_ = 0;
    for (size_t n = 0; n < identities; ++n) {
        const unsigned id = order[n]; const auto& f = folded[id];
        if (f.began && !deliveredOpen_[id]) events_[count_++] = {PanelNotification::Begin, id, {}};
        if (f.value) events_[count_++] = f.last;
        if (!f.open && (f.began || deliveredOpen_[id])) events_[count_++] = {PanelNotification::End, id, {}};
    }
    assert(count_ <= kCapacity);
}
bool SoundParameterService::get(unsigned index, int& value) const {
    if (!parameter(index)) return false;
    value = accepted_[index];
    return true;
}
SoundParameterSnapshot SoundParameterService::snapshot() const {
    return {projected_, accepted_, edits_, pairs_, generation_, revision_, dirty_.any()};
}
bool SoundParameterService::apply(Program& program) {
    SQ8L_MEASURE_SCOPE(Apply);
    if (dirty_.none()) return false;
    project(program);
    applied_ = program;
    dirty_.reset();
    refresh();
    return true;
}
} // namespace sq8l::sound
