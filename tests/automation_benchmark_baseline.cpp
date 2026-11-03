// Current harness with archived plugin/DPF/MTS/DSP; no automation service.
#include "SQ8LPlugin.hpp"
#include <thread>
#include "automation_benchmark.h"
namespace DISTRHO { extern uint32_t d_nextBufferSize; extern double d_nextSampleRate; }
struct ArchivedPlugin : DISTRHO::SQ8LPlugin { using SQ8LPlugin::run; using SQ8LPlugin::getState; };
struct Baseline {
    ArchivedPlugin plugin;
    benchmark::Callbacks callbacks;
    void setup(const benchmark::Options& o) { plugin.synth().setPolyphonyOverride(o.voices); plugin.vst2SetProgram(0); }
    void program(int p) { plugin.vst2SetProgram(p); }
    void edits(size_t b,int,const std::string&) {
        std::lock_guard<std::recursive_mutex> lock(plugin.engineMutex());
        benchmark::direct(plugin.synth().editBuffer().current(),b);
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
        for(const auto& p:sq8l::sound::parameters()) {
            std::lock_guard<std::recursive_mutex> lock(plugin.engineMutex());
            values[p.index]=sq8l::sound::read(p,plugin.synth().editBuffer().current());
        }
    }
    const void* programBytes() { return plugin.synth().editBuffer().current().bytes; }
};
int main(int argc,char** argv) {
    try {
        const char* directory=std::getenv("SQ8L_DATA_DIRECTORY");
        if(!directory || !*directory) throw std::runtime_error("explicit SQ8L_DATA_DIRECTORY required");
        if(argc<2 || std::strcmp(argv[1],"--benchmark")) throw std::runtime_error("expected --benchmark");
        auto o=benchmark::parse(argc,argv);
        if(o.lane!="prechange") throw std::runtime_error("archived plugin supports --lane prechange only");
        DISTRHO::d_nextBufferSize=o.frames; DISTRHO::d_nextSampleRate=o.rate;
        Baseline a;return benchmark::run(a,o);
    } catch(const std::exception& e) {std::fprintf(stderr,"baseline: %s\n",e.what());return 2;}
}
