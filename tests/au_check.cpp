// The Audio Unit's presets and state as a host drives them around a preset change (issue #37:
// Logic crashed in DISTRHO::PluginAU::retrieveClassInfo). Loads the built component in this
// process without installation. Native AudioComponent registration still needs a working
// system registrar. A preset name
// handed to or by the plug-in with the wrong ownership is a use after free, so run it with
// MallocScribble=1: freed memory is then overwritten and the mistake crashes every time.
//
//   MallocScribble=1 sq8l_au_check path/to/SQ8L.component
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cstdio>
#include <cstring>
#include <string>
#include "SoundParameters.h"
#include "automation_adapter_check.h"
#include <algorithm>
#include <array>
#include <vector>
#include <thread>
#include <memory>
#include <type_traits>
#include <cstdlib>
#include <climits>
#include <limits>

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) failures++;
}

struct InstanceDisposer {
    void operator()(AudioUnit au) const {
        AudioUnitUninitialize(au);
        AudioComponentInstanceDispose(au);
    }
};
using Instance = std::unique_ptr<std::remove_pointer<AudioUnit>::type, InstanceDisposer>;

// One registration per requested artifact, then independent native instances.
// Keep the exact bundle/factory alive until every instance has been disposed.
class Component {
    CFBundleRef bundle = nullptr;
    AudioComponent component = nullptr;
public:
    explicit Component(const char* path) {
        char requested[PATH_MAX];
        if (realpath(path, requested) == nullptr) {std::fprintf(stderr,"bundle path invalid\n");return;}
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(
            nullptr, reinterpret_cast<const UInt8*>(requested), static_cast<CFIndex>(std::strlen(requested)), true);
        if (url == nullptr) return;
        bundle = CFBundleCreate(nullptr, url);
        CFRelease(url);
        if (bundle == nullptr) {std::fprintf(stderr,"bundle missing\n");return;}
        CFURLRef actualURL = CFBundleCopyBundleURL(bundle);
        UInt8 actual[PATH_MAX];
        char resolved[PATH_MAX];
        const bool exact = actualURL != nullptr
            && CFURLGetFileSystemRepresentation(actualURL, true, actual, sizeof(actual))
            && realpath(reinterpret_cast<const char*>(actual), resolved) != nullptr
            && std::strcmp(requested, resolved) == 0;
        if (actualURL != nullptr) CFRelease(actualURL);
        if (!exact) {std::fprintf(stderr,"bundle identity mismatch\n");return;}
        if (!CFBundleLoadExecutable(bundle)) {std::fprintf(stderr,"bundle load failed\n");return;}
        auto factory = reinterpret_cast<AudioComponentFactoryFunction>(
            CFBundleGetFunctionPointerForName(bundle, CFSTR("PluginAUFactory")));
        if (factory == nullptr) {std::fprintf(stderr,"factory missing\n");return;}
        AudioComponentDescription desc{'aumu', 'SQ8L', 'SQ8L', kAudioComponentFlag_SandboxSafe, 0};
        component = AudioComponentRegister(&desc, CFSTR("sq8l_au_check: SQ8L"), 1, factory);
        if (component == nullptr) std::fprintf(stderr,"register failed\n");
    }
    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;
    ~Component() {if (bundle != nullptr) CFRelease(bundle);}

    Instance open() const {
        if (component == nullptr) return {};
        AudioUnit au = nullptr;
        auto status=AudioComponentInstanceNew(component,&au);
        Instance instance(au);
        if(status!=noErr || au==nullptr){std::fprintf(stderr,"instantiate %d\n",int(status));return {};}
        status=AudioUnitInitialize(au);if(status!=noErr){std::fprintf(stderr,"initialize %d\n",int(status));return {};}
        return instance;
    }
};

OSStatus setPreset(AudioUnit au, SInt32 number, CFStringRef name) {
    AUPreset p{number, name};
    return AudioUnitSetProperty(au, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &p, sizeof(p));
}

// The host owns the name it gets back and releases it (AudioUnitProperties.h).
SInt32 getPreset(AudioUnit au, std::string* name = nullptr) {
    AUPreset p{};
    UInt32 size = sizeof(p);
    if (AudioUnitGetProperty(au, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &p, &size) != noErr)
        return -1000;
    if (name != nullptr) {
        char buf[256] = {};
        if (p.presetName != nullptr) CFStringGetCString(p.presetName, buf, sizeof(buf), kCFStringEncodingUTF8);
        *name = buf;
    }
    if (p.presetName != nullptr) CFRelease(p.presetName);
    return p.presetNumber;
}

// ClassInfo is the plug-in's whole state, which hosts read to save a project or a preset.
CFPropertyListRef classInfo(AudioUnit au) {
    CFPropertyListRef info = nullptr;
    UInt32 size = sizeof(info);
    if (AudioUnitGetProperty(au, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &info, &size) != noErr)
        return nullptr;
    return info;
}

bool restoreClassInfo(AudioUnit au, CFPropertyListRef info) {
    return AudioUnitSetProperty(au, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &info,
                                sizeof(info)) == noErr;
}

// A copy of a ClassInfo with its program number replaced.
CFPropertyListRef withProgram(CFPropertyListRef info, SInt32 program) {
    CFMutableDictionaryRef copy = CFDictionaryCreateMutableCopy(nullptr, 0, static_cast<CFDictionaryRef>(info));
    auto data = static_cast<CFDictionaryRef>(CFDictionaryGetValue(copy, CFSTR(kAUPresetDataKey)));
    CFMutableDictionaryRef dataCopy = CFDictionaryCreateMutableCopy(nullptr, 0, data);
    CFNumberRef num = CFNumberCreate(nullptr, kCFNumberSInt32Type, &program);
    CFDictionarySetValue(dataCopy, CFSTR("program"), num);
    CFDictionarySetValue(copy, CFSTR(kAUPresetDataKey), dataCopy);
    CFRelease(num);
    CFRelease(dataCopy);
    return copy;
}

using automation_check::require;
using automation_check::near;
float get(AudioUnit au, unsigned id) {float v=0;require(AudioUnitGetParameter(au,id,kAudioUnitScope_Global,0,&v)==noErr,"get parameter");return v;}
void set(AudioUnit au,unsigned id,float v) {require(AudioUnitSetParameter(au,id,kAudioUnitScope_Global,0,v,0)==noErr,"set parameter");}
CFDictionaryRef data(CFPropertyListRef p){return (CFDictionaryRef)CFDictionaryGetValue((CFDictionaryRef)p,CFSTR(kAUPresetDataKey));}
std::string chunk(CFPropertyListRef p) {
    CFArrayRef states=(CFArrayRef)CFDictionaryGetValue(data(p),CFSTR("states"));
    for(CFIndex i=0;i<CFArrayGetCount(states);++i){auto d=(CFDictionaryRef)CFArrayGetValueAtIndex(states,i);auto v=(CFStringRef)CFDictionaryGetValue(d,CFSTR("editbuffer"));if(v){std::vector<char> text(CFStringGetLength(v)+1);CFStringGetCString(v,text.data(),text.size(),kCFStringEncodingUTF8);return text.data();}}
    throw std::runtime_error("missing editbuffer");
}
CFPropertyListRef change(CFPropertyListRef p,const std::string& bytes,const char* symbol=nullptr,double value=0) {
    auto copy=CFDictionaryCreateMutableCopy(nullptr,0,(CFDictionaryRef)p);auto d=CFDictionaryCreateMutableCopy(nullptr,0,data(p));
    auto str=CFStringCreateWithCString(nullptr,bytes.c_str(),kCFStringEncodingUTF8);const void* key=CFSTR("editbuffer");const void* val=str;
    auto state=CFDictionaryCreate(nullptr,&key,&val,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);const void* sd=state;
    auto states=CFArrayCreate(nullptr,&sd,1,&kCFTypeArrayCallBacks);CFDictionarySetValue(d,CFSTR("states"),states);
    CFDictionaryRemoveValue(d,CFSTR("params"));
    if(symbol){auto k=CFStringCreateWithCString(nullptr,symbol,kCFStringEncodingUTF8);auto v=CFNumberCreate(nullptr,kCFNumberFloat64Type,&value);const void* pk=k;const void* pv=v;auto pd=CFDictionaryCreate(nullptr,&pk,&pv,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);const void* pp=pd;auto params=CFArrayCreate(nullptr,&pp,1,&kCFTypeArrayCallBacks);CFDictionarySetValue(d,CFSTR("params"),params);CFRelease(params);CFRelease(pd);CFRelease(k);CFRelease(v);}
    CFDictionarySetValue(copy,CFSTR(kAUPresetDataKey),d);CFRelease(d);CFRelease(states);CFRelease(state);CFRelease(str);return copy;
}
std::string byte(std::string s,size_t n,uint8_t value){const char* hex="0123456789abcdef";s.at(n*2)=hex[value>>4];s.at(n*2+1)=hex[value&15];return s;}
using Audio=std::array<std::array<float,512>,2>;
Audio render(AudioUnit au,double time=0){Audio result{};struct {UInt32 count;AudioBuffer buffers[2];} list{2,{{1,512*sizeof(float),result[0].data()},{1,512*sizeof(float),result[1].data()}}};AudioTimeStamp ts{};ts.mFlags=kAudioTimeStampSampleTimeValid;ts.mSampleTime=time;AudioUnitRenderActionFlags flags=0;require(AudioUnitRender(au,&flags,&ts,0,512,(AudioBufferList*)&list)==noErr,"render");return result;}
struct Listener {
    AudioUnit au;AUEventListenerRef listener=nullptr;AUParameterListenerRef immediate=nullptr;unsigned immediateCount=0;unsigned count=0,gestures=0;std::array<float,256> mirror{};std::thread::id thread;
    Listener(AudioUnit a):au(a){require(AUEventListenerCreate([](void* ctx,void*,const AudioUnitEvent* e,UInt64,Float32){auto& s=*(Listener*)ctx;s.thread=std::this_thread::get_id();if(e->mEventType!=kAudioUnitEvent_ParameterValueChange){++s.gestures;return;}++s.count;
        // Waiting for a separate getter and coherent stopped save detects engine ownership.
        std::thread read([&]{s.mirror[e->mArgument.mParameter.mParameterID]=get(s.au,e->mArgument.mParameter.mParameterID);auto p=classInfo(s.au);require(p,"listener state getter");CFRelease(p);});read.join();
    },this,CFRunLoopGetCurrent(),kCFRunLoopDefaultMode,0,0,&listener)==noErr,"create listener");
    // Unlike AUEventListenerCreate, AUListenerCreate explicitly guarantees
    // synchronous notifier-thread delivery for interval zero (installed SDK).
    require(AUListenerCreate([](void* ctx,void*,const AudioUnitParameter* p,Float32){auto& s=*(Listener*)ctx;++s.immediateCount;
        std::thread read([&]{get(s.au,p->mParameterID);auto state=classInfo(s.au);require(state,"immediate listener save");CFRelease(state);});read.join();
    },this,CFRunLoopGetCurrent(),kCFRunLoopDefaultMode,0,&immediate)==noErr,"create immediate parameter listener");
    for(unsigned id=0;id<256;++id){AudioUnitParameter p{au,id,kAudioUnitScope_Global,0};require(AUListenerAddParameter(immediate,this,&p)==noErr,"register immediate parameter listener");}
    for(unsigned id=0;id<256;++id)for(auto type:{kAudioUnitEvent_ParameterValueChange,kAudioUnitEvent_BeginParameterChangeGesture,kAudioUnitEvent_EndParameterChangeGesture}){AudioUnitEvent e{};e.mEventType=type;e.mArgument.mParameter={au,id,kAudioUnitScope_Global,0};require(AUEventListenerAddEventType(listener,this,&e)==noErr,"register listener");}}
    void drain(unsigned minimum){
        const auto deadline=CFAbsoluteTimeGetCurrent()+3.0;
        while(count<minimum && CFAbsoluteTimeGetCurrent()<deadline)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.01,false);
        require(count>=minimum,"native listener delivery within runloop deadline");
    }
    ~Listener(){AUListenerDispose(immediate);AUListenerDispose(listener);}
};
void automation(AudioUnit au,const Component& component,bool optout){
    UInt32 size=0;Boolean writable=false;require(AudioUnitGetPropertyInfo(au,kAudioUnitProperty_ParameterList,kAudioUnitScope_Global,0,&size,&writable)==noErr&&size==256*sizeof(AudioUnitParameterID),"256 native parameters");
    auto clean=classInfo(au);require(clean,"initial state");
    unsigned choices=0;
    for(const auto& p:sq8l::sound::parameters()){
        AudioUnitParameterInfo info{};size=sizeof(info);require(AudioUnitGetProperty(au,kAudioUnitProperty_ParameterInfo,kAudioUnitScope_Global,p.index,&info,&size)==noErr,"native metadata");near(info.minValue,p.minimum,"min");near(info.maxValue,p.maximum,"max");near(info.defaultValue,p.defaultValue,"default");require(info.unit==(p.kind==sq8l::sound::Kind::Boolean?kAudioUnitParameterUnit_Boolean:kAudioUnitParameterUnit_Indexed),"discrete native unit");require((info.flags&kAudioUnitParameterFlag_IsWritable)&&!(info.flags&kAudioUnitParameterFlag_NonRealTime),"automatable");if(info.flags&kAudioUnitParameterFlag_CFNameRelease)CFRelease(info.cfNameString);
        CFArrayRef strings=nullptr;if(!optout&&p.kind!=sq8l::sound::Kind::Integer){size=sizeof(strings);require(AudioUnitGetProperty(au,kAudioUnitProperty_ParameterValueStrings,kAudioUnitScope_Global,p.index,&strings,&size)==noErr,"native enum strings");require(CFArrayGetCount(strings)==p.maximum-p.minimum+1,"enum count");}
        for(int v=p.minimum;v<=p.maximum;++v){set(au,p.index,v);near(get(au,p.index),v,"all legal choices");++choices;if(strings){char text[256];CFStringGetCString((CFStringRef)CFArrayGetValueAtIndex(strings,v-p.minimum),text,sizeof text,kCFStringEncodingUTF8);require(text==sq8l::sound::choiceLabel(p,v),"enum label");}}
        if(strings)CFRelease(strings);
    }
    check(choices==20418,"256 metadata and 20418 native legal choices");require(restoreClassInfo(au,clean),"restore initial");
    if(optout){CFRelease(clean);return;}
    require(!CFDictionaryContainsKey(data(clean),CFSTR("params")),"new ClassInfo omits projections");auto original=chunk(clean);
    for(const auto& p:sq8l::sound::parameters())for(float invalid:{NAN,INFINITY,-INFINITY}){float before=get(au,p.index);require(AudioUnitSetParameter(au,p.index,kAudioUnitScope_Global,0,invalid,0)!=noErr,"nonfinite rejected");near(get(au,p.index),before,"nonfinite unchanged");}
    auto invalidState=classInfo(au);require(chunk(invalidState)==original,"nonfinite does not create intent");CFRelease(invalidState);
    for(const auto& p:sq8l::sound::parameters())for(float v:{float(p.minimum-100),float(p.maximum+100),float(p.minimum)+0.5f}){set(au,p.index,v);int expected=0;require(sq8l::sound::canonicalize(p,v,expected),"canonical expectation");near(get(au,p.index),expected,"finite canonical");}
    unsigned legacyFinite=0,legacyInvalid=0;
    for(const auto& p:sq8l::sound::parameters()){
        // These doubles are serialized as real float64 CFNumbers and delivered through ClassInfo.
        for(double v:{double(p.minimum)-100,double(p.maximum)+100,-0.5,0.5,0.5001,31.5,-1e39,1e39,-std::numeric_limits<double>::max(),std::numeric_limits<double>::max()}){
            auto state=change(clean,original,p.symbol,v);require(restoreClassInfo(au,state),"ClassInfo float64 input");CFRelease(state);
            int expected=0;require(sq8l::sound::canonicalize(p,v,expected),"ClassInfo finite expectation");near(get(au,p.index),expected,"ClassInfo original-wide clamp/round");
            auto accepted=classInfo(au);auto bytes=chunk(accepted);require(restoreClassInfo(au,accepted),"ClassInfo canonical roundtrip");CFRelease(accepted);accepted=classInfo(au);require(chunk(accepted)==bytes,"ClassInfo accepted intent roundtrip");CFRelease(accepted);++legacyFinite;
        }
        for(double invalid:{std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity()}){
            require(restoreClassInfo(au,clean),"ClassInfo invalid baseline");std::array<float,256> before{};for(const auto& q:sq8l::sound::parameters())before[q.index]=get(au,q.index);auto state=change(clean,original,p.symbol,invalid);require(restoreClassInfo(au,state),"ClassInfo nonfinite envelope");CFRelease(state);
            for(const auto& q:sq8l::sound::parameters())near(get(au,q.index),before[q.index],"ClassInfo nonfinite preserves all initial mirrors");
            auto accepted=classInfo(au);require(chunk(accepted)==original,"ClassInfo nonfinite preserves bytes and unset intent");CFRelease(accepted);++legacyInvalid;
        }
    }
    require(legacyFinite==2560&&legacyInvalid==768,"all256 float64 ClassInfo numeric cases");
    std::printf("  ClassInfo float64 finite=%u nonfinite=%u through native AU property ABI\n",legacyFinite,legacyInvalid);
    require(restoreClassInfo(au,clean),"restore unset");
    auto pair=std::find_if(sq8l::sound::parameters().begin(),sq8l::sound::parameters().end(),[](const auto& p){return p.codec==sq8l::sound::Codec::Free;});
    set(au,pair->index,get(au,pair->index));auto explicitState=classInfo(au);require(chunk(explicitState)!=original,"equal Free creates explicit intent");auto explicitBytes=chunk(explicitState);require(restoreClassInfo(au,explicitState),"restore equal intent");auto roundtrip=classInfo(au);require(chunk(roundtrip)==explicitBytes,"new owning state authoritative");CFRelease(roundtrip);
    require(restoreClassInfo(au,clean),"restore absent params");roundtrip=classInfo(au);require(chunk(roundtrip)==original,"absent params preserve unset caches");CFRelease(roundtrip);
    auto legacy=change(clean,original,pair->symbol,20);require(restoreClassInfo(au,legacy),"legacy explicit entries");near(get(au,pair->index),20,"legacy explicit after chunk");CFRelease(legacy);
    auto sync=std::find_if(sq8l::sound::parameters().begin(),sq8l::sound::parameters().end(),[&](const auto& p){return p.pair==pair->pair&&p.codec!=sq8l::sound::Codec::Free;});set(au,sync->index,get(au,sync->index));auto inactive=classInfo(au);require(chunk(inactive).size()>original.size(),"equal inactive Sync intent saved");require(restoreClassInfo(au,inactive),"inactive restore");roundtrip=classInfo(au);require(chunk(roundtrip)==chunk(inactive),"inactive cache restored");CFRelease(roundtrip);CFRelease(inactive);
    require(setPreset(au,270,CFSTR(""))==noErr,"consume restore incidental program before replacement test");
    {
        Listener listener(au);auto caller=std::this_thread::get_id();require(setPreset(au,269,CFSTR(""))==noErr,"stopped factory replacement");require(listener.immediateCount>=256,"immediate parameter transport outside engine lock");listener.drain(256);require(listener.thread==caller,"stopped listener on selected runloop");for(unsigned i=0;i<256;++i)near(listener.mirror[i],get(au,i),"program mirror");
        unsigned count=listener.count;require(restoreClassInfo(au,clean),"stopped state replacement");listener.drain(count+256);
        auto enabled=change(clean,byte(original,0x1d,uint8_t(std::stoi(original.substr(0x1d*2,2),nullptr,16)|4)));require(restoreClassInfo(au,enabled),"enable MIDI program change");CFRelease(enabled);
        for(unsigned offset:{0u,127u}){count=listener.count;require(MusicDeviceMIDIEvent(au,0xc0,offset?127:0,0,offset)==noErr,"MIDI program");render(au,offset?512:0);listener.drain(count+256);for(unsigned i=0;i<256;++i)near(listener.mirror[i],get(au,i),"MIDI mirror");}
        require(listener.gestures==0,"bulk refresh creates no gestures");
    }
    require(restoreClassInfo(au,clean),"precedence baseline");
    const float baselineFree=get(au,pair->index);
    set(au,pair->index,22);require(restoreClassInfo(au,clean),"chunk replaces earlier explicit parameter");
    near(get(au,pair->index),baselineFree,"chunk wins earlier edit");
    const SInt32 incidental=getPreset(au)==270?271:270;
    require(setPreset(au,incidental,CFSTR(""))==noErr,"incidental program after owning chunk");
    auto preserved=classInfo(au);require(chunk(preserved)==original,"incidental program preserves owning chunk");CFRelease(preserved);
    set(au,pair->index,23);near(get(au,pair->index),23,"later explicit parameter wins chunk");
    require(restoreClassInfo(au,clean),"restore before malformed state");
    auto malformed=change(clean,"zz");auto numbered=withProgram(malformed,269);require(restoreClassInfo(au,numbered),"malformed owning payload accepted by AU envelope");CFRelease(numbered);CFRelease(malformed);
    preserved=classInfo(au);require(chunk(preserved)==original,"malformed owning state does not load provisional program");CFRelease(preserved);
    auto semi=std::find_if(sq8l::sound::parameters().begin(),sq8l::sound::parameters().end(),[](const auto& p){return std::string(p.symbol)=="oscillator1_semi";});
    require(setPreset(au,270,CFSTR(""))==noErr,"consume restore incidental before sensitive selection");
    require(setPreset(au,269,CFSTR(""))==noErr,"sensitive factory269");auto base=classInfo(au);auto baseBytes=chunk(base);
    for(unsigned offset:{0u,127u}){auto aInstance=component.open(),bInstance=component.open(),cInstance=component.open();auto a=aInstance.get(),b=bInstance.get(),c=cInstance.get();require(a&&b&&c,"audio lanes");auto ref=change(base,byte(baseBytes,31+34+1,uint8_t(-12)));require(restoreClassInfo(a,base)&&restoreClassInfo(b,ref)&&restoreClassInfo(c,base),"audio initial state");CFRelease(ref);set(a,semi->index,-12);set(a,semi->index,0);set(a,semi->index,12);ref=change(base,byte(baseBytes,31+34+1,12));require(restoreClassInfo(b,ref),"independent raw reference");CFRelease(ref);
        for(auto lane:{a,b,c})require(MusicDeviceMIDIEvent(lane,0x90,60,100,offset)==noErr,"note offset");auto aa=render(a),bb=render(b),cc=render(c);double peak=0,diff=0;for(unsigned ch=0;ch<2;++ch)for(unsigned n=0;n<512;++n){require(std::isfinite(aa[ch][n]),"finite audio");peak=std::max(peak,double(std::abs(aa[ch][n])));diff=std::max(diff,double(std::abs(aa[ch][n]-cc[ch][n])));if(n<offset)require(aa[ch][n]==0,"MIDI prefix");}require(std::memcmp(&aa,&bb,sizeof aa)==0,"pending actual render bitexact raw reference");require(peak>1e-5&&diff>1e-5,"audible differing control");
        for(unsigned block=1;block<=12;++block){
            if(block==5)for(auto lane:{a,b})require(MusicDeviceMIDIEvent(lane,0x80,60,0,127)==noErr,"release offset");
            aa=render(a,block*512);bb=render(b,block*512);
            require(std::memcmp(&aa,&bb,sizeof aa)==0,"sustained and release trajectory bitexact");
            for(const auto& channel:aa)for(float sample:channel)require(std::isfinite(sample),"finite sustained release");
        }
        // Two owning ClassInfos differing only in provisional program metadata
        // must produce the same sustained/released continuation and new notes.
        ref=change(base,byte(baseBytes,31+34+1,12));auto numbered=withProgram(ref,0);require(restoreClassInfo(a,numbered)&&restoreClassInfo(b,ref),"owning restore with provisional program");CFRelease(numbered);CFRelease(ref);
        set(a,semi->index,-12);set(a,semi->index,12);
        for(auto lane:{a,b})require(MusicDeviceMIDIEvent(lane,0x90,67,100,127)==noErr,"new note after restore");
        for(unsigned block=13;block<18;++block){aa=render(a,block*512);bb=render(b,block*512);require(std::memcmp(&aa,&bb,sizeof aa)==0,"restore then explicit parameter trajectory");}
    }
    CFRelease(base);CFRelease(explicitState);CFRelease(clean);check(true,"AU pending intent, numeric policy, ClassInfo, native listener and sensitive render");
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // what ran last is the culprit if it crashes
    if (argc < 2) {
        std::fprintf(stderr, "usage: sq8l_au_check path/to/SQ8L.component\n");
        return 2;
    }
    Component component(argv[1]);
    auto instance = component.open();
    AudioUnit au = instance.get();
    if (au == nullptr) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    std::printf("factory presets\n");
    for (SInt32 n : {3, 4, 3}) {
        check(setPreset(au, n, CFSTR("")) == noErr, "select factory preset " + std::to_string(n));
        for (int i = 0; i < 3; i++) getPreset(au);
        check(getPreset(au) == n, "PresentPreset reads it back, the name still valid");
        CFPropertyListRef info = classInfo(au);
        check(info != nullptr, "ClassInfo after the change");
        if (info != nullptr) CFRelease(info);
    }
    check(setPreset(au, 100000, CFSTR("")) != noErr, "a factory preset past the end is refused");
    check(getPreset(au) == 3, "and the previous one stays selected");

    std::printf("user preset (what Logic sets for its own patches)\n");
    for (int round = 0; round < 3; round++) {
        CFStringRef name = CFStringCreateWithCString(nullptr, "A Logic patch with a long name", kCFStringEncodingUTF8);
        check(setPreset(au, -1, name) == noErr, "set a user preset");
        CFRelease(name);  // the host's own reference ends here
        CFPropertyListRef info = classInfo(au);
        check(info != nullptr, "ClassInfo with the user preset");
        if (info != nullptr) CFRelease(info);
        std::string got;
        check(getPreset(au, &got) == -1 && got == "A Logic patch with a long name", "PresentPreset gives its name back");
    }
    check(setPreset(au, -1, nullptr) == noErr, "a user preset without a name");
    std::string got = "?";
    check(getPreset(au, &got) == -1 && got.empty(), "reads back with an empty name");

    std::printf("state\n");
    check(setPreset(au, 5, CFSTR("")) == noErr, "select factory preset 5");
    CFPropertyListRef saved = classInfo(au);
    check(saved != nullptr, "save the state");
    if (saved != nullptr) {
        check(restoreClassInfo(au, saved), "restore it");
        check(getPreset(au) == 5, "the preset comes back");
        CFPropertyListRef bad = withProgram(saved, 100000);
        restoreClassInfo(au, bad);
        CFRelease(bad);
        CFPropertyListRef info = classInfo(au);
        check(info != nullptr, "ClassInfo after a state with a program past the end");
        if (info != nullptr) CFRelease(info);
        check(getPreset(au) == -1, "which leaves no factory preset selected");
        CFRelease(saved);
    }

    try { automation(au,component,argc>2 && std::strcmp(argv[2],"--optout")==0); }
    catch(const std::exception& e) {check(false,e.what());}

    std::printf(failures == 0 ? "OK: 0 failure(s)\n" : "FAILED: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
