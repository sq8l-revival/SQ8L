// Shared native benchmark protocol. Current harness/codecs are not pre-change bytes.
#pragma once
#include "SoundParameters.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <exception>
namespace benchmark {
using Clock=std::chrono::steady_clock;
inline uint64_t now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
struct Options {
    int rate=44100, frames=128, voices=8; double warmup=.1, seconds=.2;
    std::string workload="idle", lane="new", scenario="closed", raw;
};
inline Options parse(int argc,char** argv) {
    Options o;
    for(int i=2;i<argc;i+=2) {
        if(i+1==argc) throw std::runtime_error("missing option value");
        std::string k=argv[i],v=argv[i+1];
        if(k=="--sample-rate") o.rate=std::stoi(v); else if(k=="--frames") o.frames=std::stoi(v);
        else if(k=="--voices") o.voices=std::stoi(v); else if(k=="--warmup") o.warmup=std::stod(v);
        else if(k=="--seconds") o.seconds=std::stod(v); else if(k=="--workload") o.workload=v;
        else if(k=="--lane") o.lane=v; else if(k=="--scenario") o.scenario=v;
        else if(k=="--raw") o.raw=v; else throw std::runtime_error("unknown option");
    }
    if((o.rate!=44100&&o.rate!=48000&&o.rate!=96000)||(o.frames!=32&&o.frames!=64&&o.frames!=128&&o.frames!=512)||
       (o.voices!=8&&o.voices!=64)||!std::isfinite(o.warmup)||!std::isfinite(o.seconds)||o.warmup<0||o.warmup>3600||o.seconds<=0||o.seconds>3600||
       (o.workload!="idle"&&o.workload!="dense"&&o.workload!="coalesced")||
       (o.lane!="new"&&o.lane!="direct"&&o.lane!="prechange")||o.raw.empty()) throw std::runtime_error("invalid workload/options");
    if(o.scenario!="closed" && o.scenario!="concurrent-save-refresh") throw std::runtime_error("unavailable: native core cannot observe editor-open timing");
    return o;
}
inline int value(const sq8l::sound::Parameter& p,size_t block) {
    // All identities get a legal edit each block; four sends end at the same trajectory.
    return p.minimum+int((block+p.index)%size_t(p.maximum-p.minimum+1));
}
inline void direct(sq8l::Program& program,size_t block) {
    using namespace sq8l::sound;
    for(const auto& p:parameters()) if(p.pair<0) write(p,program,value(p,block));
    for(const auto& p:parameters()) if(p.pair>=0) {
        const auto* mode=parameterForField(p.modeField);
        const bool sync=mode && read(*mode,program)!=0;
        if(sync!=(p.codec==Codec::Free)) write(p,program,value(p,block));
    }
}
inline void hash(uint64_t& h,const void* ptr,size_t n) {
    const auto* p=static_cast<const unsigned char*>(ptr); for(size_t i=0;i<n;++i) { h^=p[i];h*=1099511628211ull; }
}
struct Record { uint64_t block, producerBegin,producerEnd,renderBegin,renderEnd,hash; };
struct Callback { const char* kind; uint64_t begin,end,block; unsigned index; };
struct Callbacks {
    // At most32 selected measured blocks * (1024 setters +256 getters), plus refresh.
    std::vector<Callback> records=std::vector<Callback>(65536); size_t used=0; bool enabled=false; uint64_t block=0;
    template<class F> auto call(const char* kind,unsigned index,F f) -> decltype(f()) {
        if(!enabled) return f();
        auto begin=now(); auto result=f(); auto end=now();
        if(used==records.size()) throw std::runtime_error("callback capacity exceeded");
        records[used++]={kind,begin,end,block,index}; return result;
    }
};
inline void stats(const char* name,std::vector<uint64_t> x,uint64_t deadline) {
    if(x.empty()) { std::printf("\"%s\":{\"count\":0}",name);return; }
    std::sort(x.begin(),x.end()); size_t misses=0; for(auto n:x) if(n>deadline) ++misses;
    auto q=[&](double p) {return x[size_t(std::ceil(p*x.size()))-1];};
    std::printf("\"%s\":{\"count\":%zu,\"p50_ns\":%llu,\"p95_ns\":%llu,\"p99_ns\":%llu,\"max_ns\":%llu,\"deadline_misses\":%zu}",name,x.size(),(unsigned long long)q(.5),(unsigned long long)q(.95),(unsigned long long)q(.99),(unsigned long long)x.back(),misses);
}
// Adapter provides actual render, manual engine access, callbacks, and program changes.
template<class Adapter> int run(Adapter& a,const Options& o,bool diagnostic=false) {
    const uint64_t deadline=uint64_t(1e9*o.frames/o.rate);
    // Capacity bounds paced execution by audio-block count; no allocation/logging in callbacks.
    size_t warm=size_t(std::ceil(o.warmup*o.rate/o.frames));
    size_t count=size_t(std::ceil(o.seconds*o.rate/o.frames));
    if(count>11000000) throw std::runtime_error("record capacity limit");
    std::vector<Record> records(count); std::array<float,512> l{},r{};
    std::vector<uint64_t> overhead(10000);
    for(auto& t:overhead) {auto b=now(); Record tmp{};tmp.renderBegin=now();tmp.renderEnd=now();t=now()-b; volatile auto sink=tmp.renderEnd;(void)sink;}
    uint64_t h=1469598103934665603ull;
    a.setup(o);
    const bool concurrent=o.scenario=="concurrent-save-refresh";
    const size_t sampleStep=std::max(size_t(1),(count+31)/32);
    struct Background { uint64_t block=0,saveBegin=0,saveEnd=0,refreshBegin=0,refreshEnd=0; };
    std::vector<Background> backgrounds((count+15)/16+1); size_t backgroundCount=0;
    std::mutex coordination; std::condition_variable cv;
    bool request=false,done=true,stop=false; size_t requestedBlock=0;
    std::exception_ptr error; std::string saved; std::array<int,256> refreshed{};
    Background bg;
    std::thread worker;
    if(concurrent) worker=std::thread([&] {
        for(;;) {
            { std::unique_lock<std::mutex> lock(coordination);
              if(!cv.wait_for(lock,std::chrono::seconds(5),[&]{return request||stop;})) std::abort();
              if(stop) return; request=false; bg.block=requestedBlock; }
            try {
                bg.saveBegin=now(); auto state=a.save(); bg.saveEnd=now();
                // Converting retained state and validation are outside actual save callback.
                saved=state.buffer();
                bg.refreshBegin=now(); a.refresh(refreshed); bg.refreshEnd=now();
            } catch(...) { error=std::current_exception(); }
            { std::lock_guard<std::mutex> lock(coordination); done=true; } cv.notify_all();
        }
    });
    // A failed/blocked operation terminates the process rather than joining indefinitely.
    struct Cleanup { std::thread& worker; std::mutex& mutex; std::condition_variable& cv; bool& done; bool& stop;
        ~Cleanup() { if(!worker.joinable()) return;
            { std::unique_lock<std::mutex> lock(mutex);
              if(!cv.wait_for(lock,std::chrono::seconds(5),[&]{return done;})) std::abort(); stop=true; }
            cv.notify_all(); worker.join(); }
    } cleanup{worker,coordination,cv,done,stop};
    // Fixed number of audio blocks, paced on monotonic deadlines to measure wall-time load.
    auto epoch=Clock::now();
    for(size_t b=0;b<warm+count;++b) {
        Record rec{}; rec.block=b;
        a.callbacks.block=b;
        a.callbacks.enabled=b>=warm && (b-warm)%sampleStep==0;
        rec.producerBegin=now();
        if(b%128==0) a.program(256+int((b/128)%16));
        if(o.workload!="idle") a.edits(b,o.workload=="coalesced"?4:1,o.lane);
        rec.producerEnd=now();
        const bool background=concurrent && b%16==0;
        if(background) { { std::lock_guard<std::mutex> lock(coordination); requestedBlock=b; done=false; request=true; } cv.notify_all(); }
        rec.renderBegin=now();
        a.render(l.data(),r.data(),o.frames,b,o.voices);
        rec.renderEnd=now();
        a.callbacks.enabled=false;
        if(background) {
            { std::unique_lock<std::mutex> lock(coordination);
              if(!cv.wait_for(lock,std::chrono::seconds(5),[&]{return done;})) std::abort(); }
            if(error) std::rethrow_exception(error);
            if(saved.empty() || saved!=a.save().buffer()) throw std::runtime_error("incoherent saved snapshot");
            std::array<int,256> expected{}; a.refresh(expected);
            if(refreshed!=expected) throw std::runtime_error("incoherent refresh snapshot");
            if(b>=warm) backgrounds[backgroundCount++]=bg;
        }
        // Hash outside timed render; detects different sound/voice trajectories across lanes.
        for(int i=0;i<o.frames;++i) if(!std::isfinite(l[i])||!std::isfinite(r[i])) throw std::runtime_error("nonfinite audio");
        hash(h,l.data(),o.frames*sizeof(float)); hash(h,r.data(),o.frames*sizeof(float));
        hash(h,a.programBytes(),sq8l::kProgramSize); rec.hash=h;
        if(b>=warm) records[b-warm]=rec;
        std::this_thread::sleep_until(epoch+std::chrono::nanoseconds((b+1)*deadline));
    }
    if(worker.joinable()) {
        { std::lock_guard<std::mutex> lock(coordination); stop=true; } cv.notify_all(); worker.join();
    }
    // Same native helper lifecycle, but diagnostic records cannot enter timing qualification.
    if(diagnostic) {
        FILE* f=std::fopen(o.raw.c_str(),"w"); if(!f) throw std::runtime_error("diagnostic output open failed");
        size_t overlaps=0;
        for(size_t i=0;i<backgroundCount;++i) {
            const auto& c=backgrounds[i]; const auto& r=records[c.block-warm];
            const bool overlap=c.saveBegin<r.renderEnd && c.refreshEnd>r.renderBegin;
            overlaps+=overlap;
            std::fprintf(f,"{\"mode\":\"harness-diagnostic\",\"timing_eligible\":false,\"block\":%llu,\"producer_quiescent\":%s,\"render_overlap\":%s,\"snapshot_and_refresh_equal\":true}\n",
                (unsigned long long)c.block,r.producerEnd<=c.saveBegin?"true":"false",overlap?"true":"false");
        }
        if(std::fclose(f)) throw std::runtime_error("diagnostic output close failed");
        std::printf("{\"mode\":\"harness-diagnostic\",\"timing_eligible\":false,\"release_qualified\":false,\"lane\":\"%s\",\"workload\":\"%s\",\"voices\":%d,\"blocks\":%zu,\"background_operations\":%zu,\"render_overlaps\":%zu,\"snapshot_and_refresh_equal\":true,\"worker_joined\":true,\"scope\":\"finite actual native helper schedule; no performance, wrapper, editor or general race/deadlock claim\"}\n",
            o.lane.c_str(),o.workload.c_str(),o.voices,count,backgroundCount,overlaps);
        return 0;
    }
    FILE* f=std::fopen(o.raw.c_str(),"w"); if(!f) throw std::runtime_error("raw output open failed");
    std::vector<uint64_t> producer,render; producer.reserve(count);render.reserve(count);
    for(const auto& rec:records) {
        std::fprintf(f,"{\"block\":%llu,\"producer_begin_ns\":%llu,\"producer_end_ns\":%llu,\"render_begin_ns\":%llu,\"render_end_ns\":%llu,\"trajectory_fnv64\":\"%llx\"}\n",(unsigned long long)rec.block,(unsigned long long)rec.producerBegin,(unsigned long long)rec.producerEnd,(unsigned long long)rec.renderBegin,(unsigned long long)rec.renderEnd,(unsigned long long)rec.hash);
        producer.push_back(rec.producerEnd-rec.producerBegin);render.push_back(rec.renderEnd-rec.renderBegin);
    }
    if(std::fclose(f)) throw std::runtime_error("raw output close failed");
    FILE* callbacks=std::fopen((o.raw+".callbacks.jsonl").c_str(),"w");
    FILE* backgroundFile=std::fopen((o.raw+".background.jsonl").c_str(),"w");
    if(!callbacks || !backgroundFile) throw std::runtime_error("sidecar open failed");
    std::vector<uint64_t> setters,getters,saves,refreshes;
    for(size_t i=0;i<a.callbacks.used;++i) { const auto& c=a.callbacks.records[i];
        std::fprintf(callbacks,"{\"kind\":\"%s\",\"index\":%u,\"block\":%llu,\"begin_ns\":%llu,\"end_ns\":%llu}\n",c.kind,c.index,(unsigned long long)c.block,(unsigned long long)c.begin,(unsigned long long)c.end);
        (std::strcmp(c.kind,"setter")==0?setters:getters).push_back(c.end-c.begin);
    }
    for(size_t i=0;i<backgroundCount;++i) { const auto& c=backgrounds[i];
        std::fprintf(backgroundFile,"{\"block\":%llu,\"save_begin_ns\":%llu,\"save_end_ns\":%llu,\"refresh_begin_ns\":%llu,\"refresh_end_ns\":%llu}\n",(unsigned long long)c.block,(unsigned long long)c.saveBegin,(unsigned long long)c.saveEnd,(unsigned long long)c.refreshBegin,(unsigned long long)c.refreshEnd);
        saves.push_back(c.saveEnd-c.saveBegin);refreshes.push_back(c.refreshEnd-c.refreshBegin);
    }
    if(std::fclose(callbacks) || std::fclose(backgroundFile)) throw std::runtime_error("sidecar close failed");
    std::printf("{\"schema_version\":3,\"mode\":\"benchmark\",\"lane\":\"%s\",\"workload\":\"%s\",\"sample_rate\":%d,\"frames\":%d,\"voices\":%d,\"warmup_seconds\":%.9g,\"measurement_seconds\":%.9g,\"deadline_ns\":%llu,\"trajectory_fnv64\":\"%llx\",\"scope\":\"native source-linked callbacks; serial producer/render; optional background save/refresh overlaps render; waits excluded; no wrapper/editor claim\",\"timing_eligible\":%s,",o.lane.c_str(),o.workload.c_str(),o.rate,o.frames,o.voices,o.warmup,o.seconds,(unsigned long long)deadline,(unsigned long long)h,o.warmup>=10&&o.seconds>=60?"true":"false");
    std::printf("\"scenario\":\"%s\",\"callback_sample_step_blocks\":%zu,",o.scenario.c_str(),sampleStep);
    stats("setter",setters,deadline);std::printf(",");stats("getter",getters,deadline);std::printf(",");
    stats("save",saves,deadline);std::printf(",");stats("refresh",refreshes,deadline);std::printf(",");
    stats("producer",producer,deadline);std::printf(",");stats("render",render,deadline);std::printf(",");stats("recorder_calibration",overhead,deadline);std::printf("}\n");return 0;
}
}
