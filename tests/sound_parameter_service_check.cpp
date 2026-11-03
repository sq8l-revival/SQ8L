#include "SoundParameterService.h"
#include "SoundParameterState.h"
#include "EditBuffer.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <limits>
#include <mutex>
#include <thread>

using namespace sq8l;
using namespace sq8l::sound;
namespace {
unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while (0)
int value(const SoundParameterService& s, unsigned id) { int v = -999; CHECK(s.get(id,v)); return v; }
bool same(const Program& a, const Program& b) { return std::memcmp(a.bytes,b.bytes,kProgramSize) == 0; }
struct Pair { const Parameter *free = nullptr, *sync = nullptr, *mode = nullptr; };
std::array<Pair,kTempoPairCount> pairBindings() {
    std::array<Pair,kTempoPairCount> result{};
    for (const auto& p : parameters()) if (p.pair >= 0) {
        auto& b = result[size_t(p.pair)];
        (p.codec == Codec::Free ? b.free : b.sync) = &p;
        b.mode = parameterForField(p.modeField);
    }
    return result;
}
int syncRaw(const Pair& b, int v) {
    return b.sync->codec == Codec::SyncT4 && v >= 36 ? (v - 36) | 64 : v;
}
int syncProjection(const Pair& b, int raw) {
    return b.sync->codec == Codec::SyncT4 ? std::min(raw & 63,35) + ((raw & 64) ? 36 : 0) : std::min(raw,35);
}
void pairs() {
    const auto bindings = pairBindings();
    for (const auto& b : bindings) {
        CHECK(b.free && b.sync && b.mode);
        for (int initialMode : {0,1}) {
            const int initialRaw = b.free->maximum == 127 ? 100 : 60;
            Program original = makeInitProgram();
            write(*b.free,original,initialRaw); write(*b.mode,original,initialMode);
            // Unset mode changes never rewrite a legacy out-of-sync-domain raw.
            SoundParameterService untouched(original);
            Program p = original;
            for (int mode : {1-initialMode,initialMode}) {
                CHECK(untouched.accept(b.mode->index,mode).status == Acceptance::Accepted);
                CHECK(value(untouched,b.free->index) == initialRaw);
                CHECK(value(untouched,b.sync->index) == syncProjection(b,initialRaw));
                CHECK(untouched.snapshot().pairs.explicitValues.none());
                CHECK(untouched.apply(p));
                CHECK(getParam(p,b.free->field) == initialRaw);
            }
            for (int finalMode : {0,1}) {
                std::array<int,3> order{{0,1,2}};
                do {
                    SoundParameterService grouped(original), split(original);
                    Program gp = original, sp = original;
                    bool explicitFree = false, explicitSync = false;
                    int raw = initialRaw, mode = initialMode;
                    const int sv = b.sync->maximum == 71 ? 53 : 17;
                    for (int op : order) {
                        const unsigned id = op == 0 ? b.free->index : op == 1 ? b.sync->index : b.mode->index;
                        const int v = op == 0 ? 20 : op == 1 ? sv : finalMode;
                        grouped.accept(id,v); split.accept(id,v);
                        CHECK(same(gp,original)); // setters/getters do not touch Program
                        if (op == 0) explicitFree = true;
                        if (op == 1) explicitSync = true;
                        if (op == 2) mode = finalMode;
                        if (mode ? explicitSync : explicitFree) raw = mode ? syncRaw(b,sv) : 20;
                        CHECK(value(split,b.free->index) == (explicitFree ? 20 : std::min(raw,b.free->maximum)));
                        CHECK(value(split,b.sync->index) == (explicitSync ? sv : syncProjection(b,raw)));
                        const auto before = split.snapshot();
                        CHECK(split.apply(sp));
                        CHECK(getParam(sp,b.free->field) == raw);
                        CHECK(before.values == split.snapshot().values);
                        CHECK(!split.apply(sp));
                    }
                    const auto before = grouped.snapshot();
                    CHECK(grouped.apply(gp));
                    CHECK(getParam(gp,b.free->field) == (finalMode ? syncRaw(b,sv) : 20));
                    CHECK(before.values == grouped.snapshot().values);
                    CHECK(same(gp,sp));
                    CHECK(value(grouped,b.free->index) == 20 && value(grouped,b.sync->index) == sv);
                } while (std::next_permutation(order.begin(),order.end()));
            }
            // A split boundary is meaningful: the first active edit changes the
            // raw baseline; in a grouped batch the final unset target preserves
            // the original baseline. Assert both outcomes literally.
            {
                SoundParameterService grouped(original), split(original);
                Program gp = original, sp = original;
                const auto* active = initialMode ? b.sync : b.free;
                grouped.accept(active->index,20); split.accept(active->index,20);
                CHECK(split.apply(sp));
                grouped.accept(b.mode->index,1-initialMode);
                split.accept(b.mode->index,1-initialMode);
                const auto before = grouped.snapshot();
                CHECK(grouped.apply(gp)); CHECK(split.apply(sp));
                CHECK(getParam(gp,b.free->field) == initialRaw);
                CHECK(getParam(sp,b.free->field) == 20);
                CHECK(grouped.snapshot().values == before.values);
                CHECK(value(grouped,initialMode ? b.free->index : b.sync->index) ==
                      (initialMode ? initialRaw : syncProjection(b,initialRaw)));
                CHECK(value(split,initialMode ? b.free->index : b.sync->index) == 20);
            }
            // Inactive and equal assignments establish intent, including no raw change.
            SoundParameterService inactive(original);
            const auto* id = initialMode ? b.free : b.sync;
            const int shown = value(inactive,id->index);
            inactive.accept(id->index,shown);
            const auto saved = inactive.snapshot();
            CHECK(saved.pairs.explicitValues[size_t(id->pair)*2 + (initialMode ? 0 : 1)]);
            p = original; CHECK(inactive.apply(p)); CHECK(same(p,original));
            inactive.accept(b.mode->index,1-initialMode); inactive.apply(p);
            CHECK(getParam(p,b.free->field) == (initialMode ? shown : syncRaw(b,shown)));
        }
    }
    // Literal regression, independent of any reference implementation.
    const auto b = bindings[0];
    Program p = makeInitProgram(); write(*b.free,p,100);
    SoundParameterService s(p);
    s.accept(b.free->index,20);
    CHECK(value(s,b.sync->index) == 20);
    CHECK(getParam(p,b.free->field) == 100);
    const auto retained = s.snapshot();
    CHECK(s.apply(p)); CHECK(getParam(p,b.free->field) == 20);
    CHECK(value(s,b.sync->index) == 20);
    s.accept(b.free->index,30); s.replace(makeInitProgram());
    CHECK(!s.hasPending()); CHECK(s.snapshot().pairs.explicitValues.none());
    CHECK(retained.values[b.free->index] == 20 && retained.pending);
    CHECK(getParam(retained.program,b.free->field) == 20);
    SoundParameterService other(makeInitProgram());
    other.accept(b.sync->index,27); CHECK(s.snapshot().pairs.explicitValues.none());
}
void valuesAndOrigins() {
    Program p = makeInitProgram(), original = p;
    SoundParameterService s(p);
    int out = 42;
    CHECK(!s.get(kParameterCount,out) && out == 42);
    CHECK(s.accept(kParameterCount,2).status == Acceptance::Rejected);
    CHECK(s.accept(0,std::numeric_limits<double>::infinity()).status == Acceptance::Rejected);
    CHECK(s.accept(0,std::numeric_limits<double>::quiet_NaN()).status == Acceptance::Rejected);
    CHECK(!s.hasPending());
    for (const auto& d : parameters()) (void)value(s,d.index);
    CHECK(!s.apply(p)); CHECK(same(p,original)); // all reads preserve legacy raw bytes
    for (const auto& d : parameters()) if (d.pair < 0) {
        s.replace(original); p = original;
        CHECK(s.accept(d.index,d.maximum + 100).status == Acceptance::Accepted);
        CHECK(value(s,d.index) == d.maximum);
        CHECK(same(p,original));
        const auto copy = s.snapshot();
        CHECK(s.apply(p)); CHECK(read(d,p) == d.maximum);
        CHECK(copy.values == s.snapshot().values);
    }
    s.replace(original); p = original;
    auto edit = s.accept(0,2,Origin::Panel);
    CHECK(edit.status == Acceptance::Accepted);
    SoundParameterService independent=s;
    // Matching generation/revision/value in another instance is not an echo.
    CHECK(independent.accept(0,2,Origin::Host,&edit.token).status == Acceptance::Accepted);
    const auto revision = s.snapshot().revision;
    CHECK(s.accept(0,2,Origin::Host,&edit.token).status == Acceptance::Echo);
    CHECK(s.snapshot().revision == revision);
    CHECK(s.apply(p)); CHECK(s.accept(0,2,Origin::Host,&edit.token).status == Acceptance::Echo);
    CHECK(!s.hasPending());
    CHECK(s.accept(0,2).status == Acceptance::Accepted); // independent equal host edit
    CHECK(s.snapshot().revision > revision);
    CHECK(s.accept(0,2,Origin::Host,&edit.token).status == Acceptance::Accepted); // stale token
    s.replace(original);
    CHECK(s.accept(0,2,Origin::Host,&edit.token).status == Acceptance::Accepted);
    PairValues invalid; invalid.explicitValues.set(0); invalid.values[0] = 128;
    const auto before = s.snapshot();
    CHECK(!s.replace(original,invalid)); CHECK(s.snapshot().revision == before.revision);
}
std::vector<uint8_t> base() {
    // Obtain actual existing base encoding; never use a synthetic alternate owner.
    EditBuffer edit(nullptr);
    for (const auto& p : parameters()) if (p.codec == Codec::Free) write(p,edit.current(),p.maximum);
    return edit.getChunk();
}
void put(std::vector<uint8_t>& p,size_t off,uint32_t v,size_t n) {
    for (size_t i=0;i<n;++i) p[off+i] = uint8_t(v >> (8*i));
}
void states() {
    auto current = base(); CHECK(current.size() == 571);
    // Existing owner options are opaque bytes here and must survive verbatim.
    current[0x1b]='V'; current[0x1c]=64; current[0x1d]=7; current[0x1e]=4;
    ValidatedSoundParameterState parsed;
    CHECK(parseSoundParameterState(current.data(),current.size(),parsed));
    CHECK(parsed.baseSize() == 571 && parsed.pairs().explicitValues.none());
    std::vector<uint8_t> encoded{77};
    CHECK(encodeSoundParameterState(current.data(),current.size(),{},encoded)); CHECK(encoded == current);
    std::vector<uint8_t> old(544,0); old[0x200] = 1;
    CHECK(parseSoundParameterState(old.data(),old.size(),parsed)); CHECK(parsed.baseSize() == 544);
    CHECK(encodeSoundParameterState(old.data(),old.size(),{},encoded)); CHECK(encoded == old);
    const auto bindings = pairBindings();
    PairValues maximum;
    for (const auto& p : parameters()) if (p.pair >= 0) {
        const size_t slot = size_t(p.pair)*2 + (p.codec == Codec::Free ? 0 : 1);
        maximum.explicitValues.set(slot); maximum.values[slot] = p.maximum;
    }
    CHECK(!encodeSoundParameterState(old.data(),old.size(),maximum,encoded)); CHECK(encoded == old);
    CHECK(encodeSoundParameterState(current.data(),current.size(),maximum,encoded));
    CHECK(encoded.size() == 907);
    CHECK(parseSoundParameterState(encoded.data(),encoded.size(),parsed));
    CHECK(parsed.pairs().values == maximum.values && parsed.pairs().explicitValues.count() == 40);
    CHECK(std::memcmp(parsed.baseData(),current.data(),571) == 0);
    // Sorted IDs and exact LE header, including count/payload high bytes.
    CHECK(std::memcmp(encoded.data()+571,"SQ8LAUTO\1\0\50\0\100\1\0\0",16) == 0);
    uint32_t previous = 0; unsigned entry = 0;
    for (const auto& p : parameters()) if (p.pair >= 0) {
        CHECK(encoded[587+entry*8] == (p.index & 255));
        CHECK(entry == 0 || p.index > previous); previous = p.index; ++entry;
    }
    // A failure must leave the last validated owning result unchanged.
    auto rejects = [&](const std::vector<uint8_t>& bytes) {
        CHECK(!parseSoundParameterState(bytes.data(),bytes.size(),parsed));
        CHECK(parsed.baseSize() == 571 && parsed.pairs().explicitValues.count() == 40);
        CHECK(std::memcmp(parsed.baseData(),current.data(),571) == 0);
    };
    for (size_t n=0;n<encoded.size();++n) if (n != 571) {
        auto bad = encoded; bad.resize(n); rejects(bad);
    }
    for (size_t at : {size_t(0),size_t(1),size_t(4),size_t(13),size_t(571)}) {
        auto bad = encoded; bad[at] ^= 1; rejects(bad);
    }
    { auto bad=encoded; bad.push_back(0); rejects(bad); }
    { auto bad=encoded; put(bad,581,41,2); rejects(bad); }
    { auto bad=encoded; put(bad,583,319,4); rejects(bad); }
    { auto bad=encoded; put(bad,595,51,4); rejects(bad); } // duplicate first pair ID
    { auto bad=encoded; put(bad,587,0,4); rejects(bad); } // non-pair
    { auto bad=encoded; put(bad,587,256,4); rejects(bad); }
    { auto bad=encoded; put(bad,591,0xffffffff,4); rejects(bad); }
    { auto bad=encoded; put(bad,591,64,4); rejects(bad); } // ENV1 T1 Free max63
    { auto bad=encoded; put(bad,591,62,4); rejects(bad); } // legal active value disagrees with base
    { auto bad=current; bad.push_back(0); rejects(bad); }
    { auto bad=old; bad[0x200]=2; rejects(bad); }
    CHECK(!parseSoundParameterState(nullptr,571,parsed));
    // Unknown version retains framing validation but does not inspect entry IDs/values.
    auto unknown=encoded; put(unknown,579,2,2); put(unknown,587,0xffffffff,4);
    CHECK(parseSoundParameterState(unknown.data(),unknown.size(),parsed));
    CHECK(parsed.baseSize() == 571 && parsed.pairs().explicitValues.none());
    auto empty=current; empty.insert(empty.end(),{'S','Q','8','L','A','U','T','O',1,0,0,0,0,0,0,0});
    CHECK(parseSoundParameterState(empty.data(),empty.size(),parsed)); CHECK(parsed.pairs().explicitValues.none());
    // Equal-value explicit assignment must produce a persistent entry.
    PairValues inconsistent=maximum; inconsistent.values[8]=62; // pair4/ENV1 T1 active Free
    const auto retainedEncoding=encoded;
    CHECK(!encodeSoundParameterState(current.data(),571,inconsistent,encoded)); CHECK(encoded==retainedEncoding);
    PairValues inactive; inactive.explicitValues.set(1); inactive.values[1]=17;
    CHECK(encodeSoundParameterState(current.data(),571,inactive,encoded));
    CHECK(parseSoundParameterState(encoded.data(),encoded.size(),parsed)); CHECK(parsed.pairs().values[1]==17);
    Program p; std::memcpy(p.bytes,current.data()+31,kProgramSize); SoundParameterService s(p);
    s.accept(bindings[0].free->index,value(s,bindings[0].free->index));
    CHECK(s.apply(p));
    CHECK(encodeSoundParameterState(current.data(),571,s.snapshot().pairs,encoded)); CHECK(encoded.size()==595);
    CHECK(parseSoundParameterState(encoded.data(),encoded.size(),parsed)); CHECK(parsed.pairs().explicitValues.count()==1);
    // Canonical equality, rather than raw-byte equality, is the restore rule in
    // either mode, including clipped legacy rates and packed T4 projections.
    for (const auto& b : bindings) for (int mode : {0,1}) {
        EditBuffer edit(nullptr);
        const int raw=b.free->maximum==127 ? 100 : 60;
        write(*b.free,edit.current(),raw); write(*b.mode,edit.current(),mode);
        const auto bytes=edit.getChunk();
        const auto* active=mode ? b.sync : b.free;
        PairValues cache;
        const size_t slot=size_t(active->pair)*2+(mode ? 1 : 0);
        cache.explicitValues.set(slot);
        cache.values[slot]=mode ? syncProjection(b,raw) : raw;
        CHECK(encodeSoundParameterState(bytes.data(),bytes.size(),cache,encoded));
        CHECK(parseSoundParameterState(encoded.data(),encoded.size(),parsed));
        CHECK(std::memcmp(parsed.baseData(),bytes.data(),bytes.size())==0);
        const auto valid=encoded;
        cache.values[slot]=cache.values[slot]==0 ? 1 : cache.values[slot]-1;
        CHECK(!encodeSoundParameterState(bytes.data(),bytes.size(),cache,encoded)); CHECK(encoded==valid);
        put(encoded,591,uint32_t(cache.values[slot]),4);
        CHECK(!parseSoundParameterState(encoded.data(),encoded.size(),parsed));
        CHECK(parsed.pairs().values[slot] != cache.values[slot]);
    }
}
void concurrency() {
    using namespace std::chrono_literals;
    std::recursive_mutex mutex;
    Program program=makeInitProgram(); SoundParameterService service(program);
    std::atomic<bool> finished{false};
    // Last-resort finite deadlock bound also covers accidental join deadlocks.
    std::thread watchdog([&] {
        for (int i=0;i<1000 && !finished.load();++i) std::this_thread::sleep_for(10ms);
        if (!finished.load()) { std::fputs("mutex test timeout\n",stderr); std::_Exit(2); }
    });
    std::promise<void> entering, returned;
    auto entered=entering.get_future(); auto result=returned.get_future();
    uint64_t acceptedGeneration=0;
    std::unique_lock<std::recursive_mutex> owner(mutex);
    service.accept(0,1);
    std::thread waiter([&] {
        entering.set_value();
        std::lock_guard<std::recursive_mutex> lock(mutex);
        const auto edit=service.accept(0,3);
        acceptedGeneration=edit.token.stamp.generation;
        returned.set_value();
    });
    CHECK(entered.wait_for(2s)==std::future_status::ready);
    CHECK(result.wait_for(30ms)==std::future_status::timeout);
    // Nested recursive ownership does not release the paused outer owner.
    { std::lock_guard<std::recursive_mutex> nested(mutex); program=makeInitProgram(); service.replace(program); }
    const auto generation=service.generation();
    CHECK(result.wait_for(30ms)==std::future_status::timeout);
    owner.unlock();
    CHECK(result.wait_for(2s)==std::future_status::ready); waiter.join();
    { std::lock_guard<std::recursive_mutex> lock(mutex);
      CHECK(acceptedGeneration==generation); CHECK(value(service,0)==3); service.apply(program); CHECK(read(*parameter(0),program)==3); }
    // Concurrent setters/getters/application/copies record the actual protected
    // acceptance order. Invocation order deliberately supplies no authority.
    uint64_t lastRevision=0; int lastValue=0;
    auto producer=[&](int offset) {
        for (int i=0;i<250;++i) {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            const int expected=(i+offset)%9-3;
            auto edit=service.accept(0,expected);
            lastRevision=edit.token.stamp.revision; lastValue=expected;
            int got=-99; if (!service.get(0,got) || got!=expected) std::abort();
        }
    };
    std::thread a(producer,0), b(producer,4);
    std::thread consumer([&] {
        for (int i=0;i<250;++i) { std::lock_guard<std::recursive_mutex> lock(mutex);
            service.apply(program); const auto copy=service.snapshot();
            if (copy.values[0]!=read(*parameter(0),program)) std::abort();
        }
    });
    a.join(); b.join(); consumer.join();
    { std::lock_guard<std::recursive_mutex> lock(mutex); CHECK(service.snapshot().edits[0].revision==lastRevision);
      CHECK(value(service,0)==lastValue); CHECK(service.apply(program) || !service.hasPending());
      CHECK(read(*parameter(0),program)==lastValue); }
    finished=true; watchdog.join();
}
}
int main() {
    pairs(); valuesAndOrigins(); states(); concurrency();
    std::printf("sound parameter service: %u assertions passed\n",checks);
}
