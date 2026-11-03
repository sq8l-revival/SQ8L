#include "SoundParameterState.h"
#include "SysEx.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>

using namespace sq8l;
using namespace sq8l::sound;
namespace { unsigned checks = 0; std::atomic<unsigned> allocations{0}; }
void* operator new(std::size_t size) { ++allocations; if (auto p = std::malloc(size ? size : 1)) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
namespace {
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while (0)
bool same(const Program& a, const Program& b) { return !std::memcmp(a.bytes,b.bytes,kProgramSize); }
int value(const SoundParameterService& s, unsigned id) { int v = -999; CHECK(s.get(id,v)); return v; }
struct Fixture {
    SoundLibrary library;
    Settings settings;
    Synth synth{44100,&library,&settings};
    SoundParameterService service{synth.editBuffer().current()};
    SoundParameterLifecycle lifecycle{synth,service};
};
const Parameter& freeRate() { for (const auto& p : parameters()) if (p.codec == Codec::Free) return p; std::abort(); }
const Parameter& syncRate() { const auto& f = freeRate(); for (const auto& p : parameters()) if (p.pair == f.pair && p.codec != Codec::Free) return p; std::abort(); }
void accept(Fixture& f, const Parameter& p, int v) { CHECK(f.service.accept(p.index,v).status == Acceptance::Accepted); }
void coherent(Fixture& f) { for (const auto& p : parameters()) CHECK(value(f.service,p.index) == read(p,f.synth.editBuffer().current())); }
void stopped() {
    Fixture f;
    auto& edit = f.synth.editBuffer();
    const auto& rate = freeRate(); const auto& sync = syncRate();
    const auto& mode = *parameterForField(rate.modeField);
    CHECK(f.lifecycle.attached());
    write(mode,edit.current(),0); f.service.replace(edit.current());
    const Program old = edit.current();
    accept(f,rate,20); CHECK(value(f.service,rate.index)==20); CHECK(same(old,edit.current()));
    CHECK(value(f.service,sync.index)==20); // literal no-render accepted getter
    f.synth.setPolyphonyOverride(17); f.synth.setMtsEnabled(true); f.synth.setMtsCorrectPitch(true);
    f.synth.setProgramChangeEnabled(true); f.synth.setOutputCut(6);
    const unsigned before = allocations.load();
    auto snapshot = f.lifecycle.capture();
    CHECK(allocations.load() == before); // added synchronization/fixed copy allocates nothing
    CHECK(!f.service.hasPending()); CHECK(edit.modified());
    CHECK(read(rate,edit.current()) == 20);
    std::vector<uint8_t> encoded;
    CHECK(encodeSoundParameterState(snapshot.base.data(),snapshot.base.size(),snapshot.pairs,encoded));
    ValidatedSoundParameterState state;
    CHECK(parseSoundParameterState(encoded.data(),encoded.size(),state));
    accept(f,rate,21); auto chunk = f.synth.getChunk(); CHECK(read(rate,edit.current())==21);
    accept(f,rate,22); CHECK(edit.writeProgram(2,0)); Program written{};
    CHECK(f.library.readProgram(2,written)); CHECK(read(rate,written)==22);
    accept(f,rate,23); auto sysex = edit.exportSysex(); CHECK(read(rate,edit.current())==23);
    CHECK(!sysex.empty()); CHECK(sysex == sysex::makeSingleDump(edit.current()));
    accept(f,rate,25); const auto& directChunk=edit.getChunk();
    Program direct{}; std::memcpy(direct.bytes,directChunk.data()+EditBuffer::kChunkHeaderSize,kProgramSize);
    CHECK(read(rate,direct)==25 && !f.service.hasPending());
    accept(f,rate,26); CHECK(!edit.writeProgram(0,2));
    CHECK(read(rate,edit.current())==26 && edit.modified() && !f.service.hasPending());
    accept(f,sync,9); // inactive values count as modified and persist
    edit.setModified(false); snapshot = f.lifecycle.capture(); CHECK(edit.modified());
    CHECK(snapshot.pairs.explicitValues.count()==2);
    const auto retained = snapshot;
    const auto generation = f.service.generation();
    CHECK(f.lifecycle.restore(state)); CHECK(f.service.generation()==generation+1);
    CHECK(value(f.service,rate.index)==20); CHECK(f.synth.polyphonyOverride()==17);
    CHECK(f.synth.mtsEnabled() && f.synth.mtsCorrectPitch() && f.synth.programChangeEnabled());
    CHECK(f.synth.outputCut()==6);
    const auto after = f.service.generation(); const int slot = edit.currentSlot();
    f.synth.setProgram(7); CHECK(f.service.generation()==after && edit.currentSlot()==slot); // incidental PC suppressed
    f.synth.setProgram(7); CHECK(f.service.generation()==after+1);
    CHECK(retained.pairs.explicitValues.count()==2); CHECK(f.service.snapshot().pairs.explicitValues.none());
    // Failed validation cannot mutate bytes, options, generation or pending intent.
    accept(f,rate,24); const Program saved = edit.current(); const auto gen = f.service.generation();
    encoded[0]=99; CHECK(!parseSoundParameterState(encoded.data(),encoded.size(),state));
    CHECK(same(saved,edit.current()) && f.service.generation()==gen && f.service.hasPending());
    CHECK(f.synth.setChunk(nullptr,0)==-1); CHECK(f.service.generation()==gen);
    edit.initProgram(); CHECK(f.service.generation()==gen+1); CHECK(!f.service.hasPending()); coherent(f);
    // Direct EditBuffer chunk/import callers use the same completed-load boundary.
    const auto g = f.service.generation(); CHECK(edit.setChunk(chunk.data(),chunk.size())>0);
    CHECK(f.service.generation()==g+1); coherent(f);
    CHECK(edit.importSysex(sysex.data(),sysex.size(),true)); CHECK(f.service.generation()==g+2); coherent(f);
    const auto g2 = f.service.generation(); CHECK(!edit.importSysex(nullptr,0,true)); CHECK(f.service.generation()==g2);
    // Legacy base restores must retain exact original size for option defaults.
    std::array<uint8_t,EditBuffer::kOldChunkSize> legacy{}; legacy[0x200]=1;
    CHECK(parseSoundParameterState(legacy.data(),legacy.size(),state)); CHECK(f.lifecycle.restore(state));
    CHECK(!f.synth.mtsEnabled() && !f.synth.mtsCorrectPitch() && !f.synth.programChangeEnabled());
    CHECK(f.synth.polyphonyOverride()==0 && f.synth.outputCut()==0);
}
void compareAndFailure() {
    Fixture f; auto& edit=f.synth.editBuffer(); const auto& rate=freeRate(); const auto& sync=syncRate();
    write(*parameterForField(rate.modeField),edit.current(),0); f.service.replace(edit.current());
    accept(f,rate,27); accept(f,sync,11); const auto g=f.service.generation();
    edit.compare(4,0); CHECK(edit.comparing()); CHECK(f.service.generation()==g+1);
    CHECK(f.service.snapshot().pairs.explicitValues.none());
    CHECK(read(rate,edit.slot(EditBuffer::kRingSize))==27); // backup includes pending edit
    accept(f,rate,12); edit.compare(5,0); CHECK(f.service.generation()==g+2);
    edit.compareOff(); CHECK(!edit.comparing()); CHECK(f.service.generation()==g+3);
    CHECK(value(f.service,rate.index)==27 && value(f.service,sync.index)==11);
    CHECK(f.service.snapshot().pairs.explicitValues.count()==2);
    // Failed ordinary selection preserves legacy rotation and reconciles actual bytes.
    accept(f,rate,29); const int old=edit.currentSlot(); const auto gen=f.service.generation();
    CHECK(!edit.selectProgram(0,9)); CHECK(edit.currentSlot()==((old+1)&3));
    CHECK(f.service.generation()==gen+1 && !f.service.hasPending()); coherent(f);
    const auto g2=f.service.generation(); edit.compare(0,9); CHECK(!edit.comparing());
    CHECK(f.service.generation()==g2+1); coherent(f);
    edit.compare(2,0); const auto g3=f.service.generation(); edit.compare(0,9);
    CHECK(!edit.comparing()); CHECK(f.service.generation()==g3+1); coherent(f);
}
void renderAndRing() {
    Fixture f; SoundLibrary lib; Settings settings; Synth plain(44100,&lib,&settings);
    const auto& rate=freeRate();
    // Attach adds no synthesis changes; pending automation equals the manual edit
    // before MIDI, including notes on either side of bank/program replacement.
    accept(f,rate,31); write(rate,plain.editBuffer().current(),31);
    RawMidiEvent events[]={{0,{0x90,60,100},0},{17,{0xb0,0,1},0},{31,{0xc0,4,0},0},{37,{0x90,67,110},0}};
    f.synth.processEvents(events,4); plain.processEvents(events,4);
    float l[256],r[256],pl[256],pr[256]; const auto g=f.service.generation();
    f.synth.process(l,r,256); plain.process(pl,pr,256);
    CHECK(!std::memcmp(l,pl,sizeof l) && !std::memcmp(r,pr,sizeof r));
    CHECK(f.synth.program()==132 && f.service.generation()==g+1); CHECK(!f.service.hasPending()); coherent(f);
    // Sustained old voices and four-slot reuse match the observer-disabled engine.
    for(int i=0;i<10;++i) {
        f.synth.setProgram(256+i); plain.setProgram(256+i);
        CHECK(f.synth.editBuffer().currentSlot()==plain.editBuffer().currentSlot());
        CHECK(same(f.synth.editBuffer().current(),plain.editBuffer().current()));
        f.synth.process(l,r,256); plain.process(pl,pr,256);
        CHECK(!std::memcmp(l,pl,sizeof l) && !std::memcmp(r,pr,sizeof r));
        for(float sample:l) CHECK(std::isfinite(sample));
    }
}
void completionBoundary() {
    SoundLibrary library; Settings settings; Synth synth(44100,&library,&settings);
    struct Probe final : EditBuffer::LifecycleObserver {
        Synth& synth; unsigned loads=0, failures=0; int voices=-1; bool mts=false;
        explicit Probe(Synth& s) : synth(s) {}
        void synchronize() override {}
        void backupCompare() override {}
        void replaced(EditBuffer::Replacement kind) override {
            if(kind==EditBuffer::Replacement::FailedSelection) ++failures;
            else ++loads;
            voices=synth.polyphonyOverride(); mts=synth.mtsEnabled();
        }
    } probe(synth);
    synth.setPolyphonyOverride(19); synth.setMtsEnabled(true); auto chunk=synth.getChunk();
    synth.setPolyphonyOverride(3); synth.setMtsEnabled(false);
    synth.editBuffer().setLifecycleObserver(&probe);
    CHECK(synth.setChunk(chunk.data(),chunk.size())==0);
    CHECK(probe.loads==1 && probe.voices==19 && probe.mts); // final options at completion
    synth.setProgram(4); CHECK(probe.loads==1); // incidental suppression
    synth.setProgram(4); CHECK(probe.loads==2); // two ring events are not loads
    CHECK(!synth.editBuffer().selectProgram(0,9)); CHECK(probe.loads==2 && probe.failures==1);
    synth.editBuffer().setLifecycleObserver(nullptr);
}
void attachmentAndOwnership() {
    SoundLibrary lib; Settings settings; Synth synth(44100,&lib,&settings);
    SoundParameterService service(synth.editBuffer().current());
    const auto& rate=freeRate();
    {
        SoundParameterLifecycle hook(synth,service); CHECK(hook.attached());
        SoundParameterService other(synth.editBuffer().current());
        { SoundParameterLifecycle duplicate(synth,other); CHECK(!duplicate.attached()); }
        CHECK(synth.editBuffer().lifecycleObserver()!=nullptr);
    }
    CHECK(synth.editBuffer().lifecycleObserver()==nullptr);
    const auto g=service.generation(); synth.editBuffer().initProgram(); CHECK(service.generation()==g);
    Fixture a,b; accept(a,rate,19); CHECK(!b.service.hasPending());
    std::recursive_mutex mutex;
    std::thread setter([&] { for(int i=0;i<50;++i) { std::lock_guard<std::recursive_mutex> lock(mutex); a.service.accept(rate.index,i); } });
    for(int i=0;i<50;++i) { std::lock_guard<std::recursive_mutex> lock(mutex); auto snap=a.lifecycle.capture(); CHECK(snap.base[0]==2); }
    setter.join(); { std::lock_guard<std::recursive_mutex> lock(mutex); a.lifecycle.capture(); CHECK(value(a.service,rate.index)==49); }
    CHECK(!b.service.hasPending());
}
}
int main() { stopped(); compareAndFailure(); renderAndRing(); completionBoundary(); attachmentAndOwnership(); std::printf("sound parameter lifecycle: %u assertions passed\n",checks); }
