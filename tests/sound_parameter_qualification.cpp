// Source-linked production callbacks. DPF supplies framework setup, not an ABI/DAW claim.
#include "SQ8LPlugin.hpp"
#include "DistrhoPluginMain.cpp"
#include "SoundParameterInstrumentation.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <thread>
#include "automation_benchmark.h"
using namespace sq8l;
using namespace sq8l::sound;
#if SQ8L_SOUND_PARAMETER_INSTRUMENTATION
namespace inst = sq8l::sound::instrumentation;
static void allocation(bool freed) noexcept {
    if(inst::recorder && inst::depth) {
        if(freed) ++inst::recorder->deallocations; else ++inst::recorder->allocations;
    }
}
void* operator new(std::size_t n) { allocation(false); if(void* p=std::malloc(n?n:1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { if(p) allocation(true); std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p,std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p,std::size_t) noexcept { ::operator delete(p); }
#endif
namespace {
std::atomic<unsigned> checks{0};
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while(false)
struct Plugin : DISTRHO::SQ8LPlugin {
    using SQ8LPlugin::run; using SQ8LPlugin::setParameterValue;
    using SQ8LPlugin::getParameterValue; using SQ8LPlugin::getState; using SQ8LPlugin::setState;
};
const sq8l::sound::Parameter& named(const char* name) {
    for(const auto& p:parameters()) if(!std::strcmp(p.symbol,name)) return p;
    std::abort();
}
void render(Plugin& plugin, float* l, float* r, bool on=false, bool off=false) {
    float* outputs[]={l,r}; DISTRHO::MidiEvent event{};
    event.frame=17; event.size=3; event.data[0]=on?0x90:0x80; event.data[1]=60; event.data[2]=100;
    plugin.run(nullptr,outputs,128,&event,on||off?1:0);
}
void parity() {
    Plugin plugin;
    SoundLibrary library; Settings settings; Synth direct(44100,&library,&settings);
    // Copy initial bytes via the existing legacy chunk boundary, without a service in this lane.
    auto chunk=plugin.synth().getChunk(); CHECK(direct.setChunk(chunk.data(),chunk.size())==0);
    CHECK(plugin.synth().setChunk(chunk.data(),chunk.size())==0);
    plugin.vst2SetProgram(0); direct.setProgram(0); // consume both incidental selections
    direct.master().setTransport(0,false,0);
    float l[128],r[128],dl[128],dr[128]; double energy=0; uint64_t hash=1469598103934665603ull;
    auto edit=[&](const sq8l::sound::Parameter& p,int v) {
        plugin.setParameterValue(p.index,float(v)); CHECK(plugin.getParameterValue(p.index)==v);
        CHECK(write(p,direct.editBuffer().current(),v));
    };
    for(int block=0;block<150;++block) {
        // First 20 blocks are no-automation; retained notes cross every later edit.
        if(block==20) edit(named("oscillator1_wave"),3); // enum
        if(block==25) edit(named("oscillator1_dir"),1); // packed flag
        if(block==30) for(const auto& p:parameters()) if(p.codec==Codec::Voices) edit(p,4);
        if(block==35 || block==40 || block==45) {
            const sq8l::sound::Parameter* free=nullptr; for(const auto& p:parameters()) if(p.codec==Codec::Free) { free=&p; break; }
            CHECK(free); const auto* mode=parameterForField(free->modeField); CHECK(mode);
            if(block==35) { edit(*mode,0); edit(*free,20); }
            if(block==40) edit(*mode,1); // mode-only preserves the original raw field
            if(block==45) for(const auto& p:parameters()) if(p.pair==free->pair && p.codec!=Codec::Free) edit(p,9);
        }
        if(block>=60 && block<70) {
            const int program=256+block-60;
            plugin.vst2SetProgram(program); direct.setProgram(program);
            CHECK(plugin.synth().editBuffer().currentSlot()==direct.editBuffer().currentSlot());
        }
        render(plugin,l,r,block==0,block==80);
        if(block==0 || block==80) {
            RawMidiEvent event{17,{uint8_t(block==0?0x90:0x80),60,100},0};
            direct.processEvents(&event,1);
        }
        direct.process(dl,dr,128,true);
        CHECK(!std::memcmp(l,dl,sizeof l)); CHECK(!std::memcmp(r,dr,sizeof r));
        CHECK(!std::memcmp(plugin.synth().editBuffer().current().bytes,direct.editBuffer().current().bytes,kProgramSize));
        for(float sample:l) { CHECK(std::isfinite(sample)); energy+=std::abs(sample); }
        const auto* bytes=reinterpret_cast<const unsigned char*>(l);
        for(size_t i=0;i<sizeof l;++i) { hash^=bytes[i]; hash*=1099511628211ull; }
    }
    CHECK(energy>0.01);
    std::printf("{\"mode\":\"parity\",\"blocks\":150,\"frames\":128,\"sample_rate\":44100,\"left_fnv64\":\"%llx\",\"energy\":%.9g,\"scope\":\"plugin callbacks versus independent direct-edit Synth; no wrapper transport\"}\n",(unsigned long long)hash,energy);
}
struct BenchmarkPlugin {
    Plugin plugin;
    benchmark::Options options;
    benchmark::Callbacks callbacks;
    void setup(const benchmark::Options& o) {
        options=o; plugin.synth().setPolyphonyOverride(o.voices);
        // Consume incidental selection equally in all adapters.
        plugin.vst2SetProgram(0);
    }
    void program(int p) { plugin.vst2SetProgram(p); }
    void edits(size_t block,int repeats,const std::string& lane) {
        if(lane=="new") {
            for(const auto& p:parameters()) {
                for(int n=0;n<repeats;++n) {
                    const float v=float(n+1==repeats?benchmark::value(p,block):p.minimum);
                    callbacks.call("setter",p.index,[&]{ plugin.setParameterValue(p.index,v);return 0; });
                }
                // Full actual getter callback is included in the producer interval.
                if(callbacks.call("getter",p.index,[&]{return plugin.getParameterValue(p.index);})!=benchmark::value(p,block)) throw std::runtime_error("accepted getter mismatch");
            }
        } else {
            std::lock_guard<std::recursive_mutex> lock(plugin.engineMutex());
            benchmark::direct(plugin.synth().editBuffer().current(),block);
        }
    }
    void render(float* l,float* r,int frames,size_t block,int voices) {
        float* outputs[]={l,r}; std::array<DISTRHO::MidiEvent,128> events{}; unsigned n=0;
        // Periodic release/retrigger keeps actual 8/64 note load, with release tails.
        if(block%32==0) for(int i=0;i<voices;++i) {
            auto& off=events[n++];off.frame=0;off.size=3;off.data[0]=0x80;off.data[1]=uint8_t(24+i);off.data[2]=0;
            auto& on=events[n++];on.frame=1;on.size=3;on.data[0]=0x90;on.data[1]=uint8_t(24+i);on.data[2]=100;
        }
        plugin.run(nullptr,outputs,frames,events.data(),n);
    }
    auto save() { return plugin.getState("editbuffer"); }
    void refresh(std::array<int,256>& values) {
        for(const auto& p:parameters()) {
            if(options.lane=="new") values[p.index]=int(plugin.getParameterValue(p.index));
            else { std::lock_guard<std::recursive_mutex> lock(plugin.engineMutex()); values[p.index]=read(p,plugin.synth().editBuffer().current()); }
        }
    }
    const void* programBytes() { return plugin.synth().editBuffer().current().bytes; }
};
int measureBenchmark(int argc,char** argv,bool diagnostic=false) {
    auto o=benchmark::parse(argc,argv);
    if(o.lane=="prechange") throw std::runtime_error("prechange requires isolated archived-source executable");
    if(diagnostic && (o.scenario!="concurrent-save-refresh" || o.warmup!=0 || o.seconds>2))
        throw std::runtime_error("diagnostic requires concurrent-save-refresh, zero warmup and at most2 seconds");
    DISTRHO::d_nextBufferSize=o.frames; DISTRHO::d_nextSampleRate=o.rate;
    // Byte/sample equality before timing; independent direct-codec path has no service replay.
    // Separate instances are discarded before constructing the measured lane.
    {
        BenchmarkPlugin automated, manual; automated.setup(o);manual.setup(o);
        float l[512],r[512],dl[512],dr[512];
        for(size_t block=0;block<160;++block) {
            if(block%128==0) { automated.program(256+int(block/128));manual.program(256+int(block/128)); }
            if(o.workload!="idle") {
                automated.edits(block,o.workload=="coalesced"?4:1,"new");manual.edits(block,1,"direct");
            }
            automated.render(l,r,o.frames,block,o.voices);manual.render(dl,dr,o.frames,block,o.voices);
            if(std::memcmp(l,dl,o.frames*sizeof(float)) || std::memcmp(r,dr,o.frames*sizeof(float)) ||
               std::memcmp(automated.programBytes(),manual.programBytes(),sq8l::kProgramSize))
                throw std::runtime_error("exact paired audio/program preflight failed");
        }
    }
    if(!diagnostic) std::printf("{\"preflight\":\"exact paired audio/program\",\"blocks\":160,\"passed\":true}\n");
    BenchmarkPlugin adapter; return benchmark::run(adapter,o,diagnostic);
}

#if SQ8L_SOUND_PARAMETER_INSTRUMENTATION
void waitFor(const std::atomic<bool>& flag) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!flag.load(std::memory_order_acquire)) {
        CHECK(std::chrono::steady_clock::now()<deadline); std::this_thread::yield();
    }
}
void measureInstrumentation() {
    Plugin plugin; const auto& p=named("dca1_level");
    float l[128],r[128]; render(plugin,l,r,true); // construction/MIDI warmup excluded
    auto records=std::make_unique<inst::Recorder>();
    inst::recorder=records.get();
    for(int i=0;i<32;++i) {
        plugin.setParameterValue(p.index,float(i)); CHECK(plugin.getParameterValue(p.index)==i);
        render(plugin,l,r);
        uint64_t generation=0; float values[kParameterCount];
        CHECK(plugin.sq8lCopyParameterRefresh(generation,values,kParameterCount));
    }
    // All identities, including inactive packed/tempo controls; four updates coalesce.
    for(const auto& parameter:parameters()) {
        for(int repeat=0;repeat<4;++repeat) plugin.setParameterValue(parameter.index,float(repeat%2?parameter.minimum:parameter.maximum));
        CHECK(plugin.getParameterValue(parameter.index)==parameter.minimum);
    }
    render(plugin,l,r);
    inst::recorder=nullptr;
    CHECK(!records->dropped); CHECK(!records->allocations && !records->deallocations);
    // Paused owner: setter must block; replacement completes before its acceptance.
    // Harness coordination waits are outside measured callbacks, not plugin locks.
    auto contention=std::make_unique<inst::Recorder>();
    std::atomic<bool> entered{false},done{false};
    plugin.setParameterValue(p.index,12); // pending old-generation intent must not replay
    std::unique_lock<std::recursive_mutex> owner(plugin.engineMutex());
    const auto old=plugin.soundParameters().generation();
    std::thread setter([&] {
        inst::recorder=contention.get(); entered.store(true,std::memory_order_release);
        plugin.setParameterValue(p.index,57); inst::recorder=nullptr;
        done.store(true,std::memory_order_release);
    });
    waitFor(entered);
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); CHECK(!done.load());
    plugin.synth().editBuffer().initProgram(); CHECK(plugin.soundParameters().generation()==old+1);
    owner.unlock(); waitFor(done); setter.join();
    CHECK(plugin.getParameterValue(p.index)==57); render(plugin,l,r);
    CHECK(read(p,plugin.synth().editBuffer().current())==57);
    CHECK(contention->count>=1); CHECK(!contention->dropped);
    bool observedWait=false;
    for(size_t i=0;i<contention->count;++i) {
        const auto& rec=contention->records[i];
        if(rec.operation==inst::Operation::Setter) {
            CHECK(rec.acquisitions==1); CHECK(rec.acquired-rec.begin>=10000000); observedWait=true;
        }
    }
    CHECK(observedWait);
    // Contend actual getter, refresh and render too. Render's begin precedes acquisition.
    for(int kind=0;kind<3;++kind) {
        entered=false; done=false; owner.lock();
        std::thread caller([&] {
            inst::recorder=contention.get(); entered.store(true,std::memory_order_release);
            if(kind==0) plugin.getParameterValue(p.index);
            if(kind==1) { uint64_t g=0; float values[kParameterCount]; plugin.sq8lCopyParameterRefresh(g,values,kParameterCount); }
            if(kind==2) render(plugin,l,r);
            inst::recorder=nullptr; done.store(true,std::memory_order_release);
        });
        waitFor(entered); std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(!done.load()); owner.unlock(); waitFor(done); caller.join();
        const auto& rec=contention->records[contention->count-1];
        CHECK(rec.acquisitions==1 && rec.acquired-rec.begin>=10000000);
        CHECK(rec.end-rec.begin>=rec.acquired-rec.begin);
    }
    // Concurrent actual stopped save, refresh, setters/getters and render; serial mutex ordering.
    std::atomic<bool> saved{false};
    std::thread saver([&] { for(int i=0;i<40;++i) { auto state=plugin.getState("editbuffer"); CHECK(state.length()>0); } saved.store(true); });
    for(int i=0;i<40;++i) {
        plugin.setParameterValue(p.index,i); plugin.getParameterValue(p.index); render(plugin,l,r);
        SQ8LAcceptedParameter accepted{}; CHECK(plugin.sq8lReadAcceptedParameter(p.index,accepted));
        CHECK(accepted.generation==old+1 && accepted.value==i);
    }
    waitFor(saved); saver.join();
    auto emit=[](const inst::Recorder& recorder,const char* lane) {
        for(size_t i=0;i<recorder.count;++i) {
            const auto& r=recorder.records[i];
            std::printf("{\"record\":\"%s\",\"operation\":%u,\"begin_ns\":%llu,\"acquired_ns\":%llu,\"released_ns\":%llu,\"end_ns\":%llu,\"acquisitions\":%u}\n",lane,unsigned(r.operation),(unsigned long long)r.begin,(unsigned long long)r.acquired,(unsigned long long)r.released,(unsigned long long)r.end,r.acquisitions);
        }
    };
    emit(*records,"warm"); emit(*contention,"paused-owner");
    uint64_t callbacks=0,wait=0,hold=0,elapsed=0;
    for(size_t i=0;i<records->count;++i) {
        const auto& rec=records->records[i]; CHECK(rec.end>=rec.begin);
        if(rec.acquisitions) {
            CHECK(rec.acquisitions==1 && rec.begin<=rec.acquired && rec.acquired<=rec.released && rec.released<=rec.end);
            ++callbacks; wait+=rec.acquired-rec.begin; hold+=rec.released-rec.acquired; elapsed+=rec.end-rec.begin;
        }
    }
    CHECK(callbacks==128+kParameterCount*5+1);
    std::printf("{\"mode\":\"instrumentation\",\"records\":%zu,\"callbacks\":%llu,\"total_callback_ns\":%llu,\"wait_ns\":%llu,\"hold_ns\":%llu,\"cpp_new\":%llu,\"cpp_delete\":%llu,\"paused_owner\":true,\"scope\":\"warm plugin callbacks; C++ ordinary new/delete only; no libc or host interposition; no timing gate\"}\n", records->count,(unsigned long long)callbacks,(unsigned long long)elapsed,(unsigned long long)wait,(unsigned long long)hold,(unsigned long long)records->allocations,(unsigned long long)records->deallocations);
}
#endif
}
int main(int argc,char** argv) {
    DISTRHO::d_nextBufferSize=128; DISTRHO::d_nextSampleRate=44100.;
    if(argc>=2 && !std::strcmp(argv[1],"--harness-diagnostic")) {
        try { return measureBenchmark(argc,argv,true); }
        catch(const std::exception& e) { std::fprintf(stderr,"harness-diagnostic: %s\n",e.what());return 2; }
    }
    if(argc>=2 && !std::strcmp(argv[1],"--benchmark")) {
#if SQ8L_SOUND_PARAMETER_INSTRUMENTATION || !SQ8L_BENCHMARK_OPTIMIZED
        std::fprintf(stderr,"benchmark requires optimized instrumentation-OFF build\n"); return 2;
#else
        try { return measureBenchmark(argc,argv); }
        catch(const std::exception& e) { std::fprintf(stderr,"benchmark: %s\n",e.what());return 2; }
#endif
    }
    if(argc==2 && !std::strcmp(argv[1],"--parity")) parity();
#if SQ8L_SOUND_PARAMETER_INSTRUMENTATION
    else if(argc==2 && !std::strcmp(argv[1],"--instrumentation")) measureInstrumentation();
#endif
    else {
        std::fprintf(stderr,"usage: %s --parity | --instrumentation | --benchmark | --harness-diagnostic\n--harness-diagnostic uses benchmark options, requires concurrent-save-refresh/zero warmup/at most2 seconds, and never emits performance qualification.\n--instrumentation requires SQ8L_SOUND_PARAMETER_INSTRUMENTATION=ON.\n--benchmark --lane new|direct --sample-rate 44100|48000|96000 --frames 32|64|128|512 --voices 8|64 --workload idle|dense|coalesced --scenario closed|concurrent-save-refresh --warmup SECONDS --seconds SECONDS --raw PATH; see tests/run_automation_matrix.py --help.\n",argv[0]); return 2;
    }
    std::printf("qualification: %u assertions passed\n",checks.load());
}
