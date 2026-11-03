// Exact production CLAP idle handler; desktop UI creation is deliberately bypassed.
// Target-local -fno-access-control exposes existing wrapper ownership to this
// fixture, without adding plugin exports or replacing production refresh logic.
#include "SQ8LPlugin.hpp"
#include "DistrhoPluginMain.cpp"
#ifdef near
#undef near // Windows legacy memory-model macro conflicts with the numeric assertion.
#endif
#include "automation_adapter_check.h"
#include <array>
#include <functional>
#include <iostream>
#include <thread>
using namespace automation_check;
using namespace DISTRHO;
struct Host {
  clap_host_t abi{};
  clap_host_params_t params{};
  clap_host_gui_t gui{};
  PluginCLAP* wrapper=nullptr;
  std::thread::id main=std::this_thread::get_id();
  int requests=0, rescans=0, flushes=0;
  std::array<double,256> mirror{};
  std::function<void()> probe;
  Host() {
    abi.clap_version=CLAP_VERSION;abi.host_data=this;abi.name="CLAP idle regression";
    abi.vendor="test";abi.url="";abi.version="1";
    abi.get_extension=[](const clap_host_t* h,const char* id)->const void* {
      auto& s=*(Host*)h->host_data;
      if(!std::strcmp(id,CLAP_EXT_PARAMS))return &s.params;
      if(!std::strcmp(id,CLAP_EXT_GUI))return &s.gui;
      return nullptr;
    };
    abi.request_callback=[](const clap_host_t* h){auto& s=*(Host*)h->host_data;++s.requests;if(s.probe)s.probe();};
    abi.request_process=[](const clap_host_t*){};abi.request_restart=[](const clap_host_t*){};
    params.rescan=[](const clap_host_t* h,clap_param_rescan_flags flags){
      auto& s=*(Host*)h->host_data;require(std::this_thread::get_id()==s.main,"main-thread rescan");
      require(flags==(CLAP_PARAM_RESCAN_VALUES|CLAP_PARAM_RESCAN_TEXT),"values and text rescan");
      ++s.rescans;if(s.probe)s.probe();
      for(unsigned i=0;i<256;++i){
        require(s.wrapper->getParameterValue(i,&s.mirror[i]),"host current getter");
        char text[128]{};require(s.wrapper->getParameterStringForValue(i,s.mirror[i],text,sizeof(text)),"host current text");
        require(text[0]!=0,"nonempty host text");
      }
    };
    params.request_flush=[](const clap_host_t* h){++((Host*)h->host_data)->flushes;};
    params.clear=[](const clap_host_t*,clap_id,clap_param_clear_flags){};
  }
};
int main() try {
  d_nextBufferSize=512;d_nextSampleRate=44100.;
  Host host;PluginCLAP wrapper(&host.abi);host.wrapper=&wrapper;require(wrapper.init(),"wrapper init");
  auto* plugin=static_cast<SQ8LPlugin*>(wrapper.fPlugin.getInstancePointer());
  unsigned level=0;for(const auto& p:sq8l::sound::parameters())if(std::string(p.symbol)=="dca1_level")level=p.index;
  auto accepted=[&](){SQ8LAcceptedParameter p;require(plugin->sq8lReadAcceptedParameter(level,p),"accepted stamp");return p;};
  auto save=[&](){std::vector<char> bytes;clap_ostream_t s{};s.ctx=&bytes;
    s.write=[](const clap_ostream_t* s,const void* p,uint64_t n)->int64_t{auto& b=*(std::vector<char>*)s->ctx;auto* c=(const char*)p;b.insert(b.end(),c,c+n);return n;};
    require(wrapper.stateSave(&s),"native save");return bytes;};
  host.probe=[&](){std::thread reader([&](){double v;require(wrapper.getParameterValue(level,&v),"cross-thread getter");save();});reader.join();};
  require(wrapper.createUI(false),"create wrapper UI without desktop");
  // ClapUI exists, its UIExporter remains null. The same idle callback's
  // post-editor boundary runs; actual desktop frames are not claimed.
  auto idle=[&](){require(wrapper.getUI()!=nullptr,"UI wrapper exists");wrapper.getUI()->idleCallback();};
  auto service=[&](){wrapper.onMainThread();};
  auto replace=[&](int kind){std::lock_guard<std::recursive_mutex> lock(plugin->engineMutex());auto& e=plugin->synth().editBuffer();
    if(kind==0)e.initProgram();if(kind==1)e.compare(0,0);if(kind==2)e.compareOff();
    if(kind==3){sq8l::EditBuffer::Chunk c;e.copyChunk(c);require(e.setChunk(c.data(),c.size())>0,"import-like replacement");}
  };
#if DISTRHO_PLUGIN_SQ8L_PARAMETER_REFRESH
  idle();service();
  for(int kind:{0,0,1,2,3,0}){
    auto before=accepted();replace(kind);auto replaced=accepted();require(replaced.generation!=before.generation,"completed generation");
    const int requests=host.requests,rescans=host.rescans;
    idle();require(host.requests==requests+1,"stopped idle requests host service without process or flush");
    require(host.rescans==rescans,"no rescan until cooperating host services main callback");
    idle();require(host.requests==requests+1,"same generation coalesced");
    service();require(host.rescans==rescans+1,"cooperating host rescans");near(host.mirror[level],accepted().value,"host current replacement");
    require(accepted().revision==replaced.revision,"refresh does not accept semantic intent");
    service();idle();require(host.rescans==rescans+1,"no duplicate refresh");
  }
  // A later explicit equal host edit must retain its revision and authority
  // even when an older generation refresh is pending host main service.
  replace(0);idle();auto old=accepted();
  clap_event_param_value_t event{};event.header={sizeof(event),0,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_PARAM_VALUE,0};
  event.param_id=level;event.value=old.value;
  clap_input_events_t in{};in.ctx=&event;in.size=[](const clap_input_events_t*){return uint32_t(1);};
  in.get=[](const clap_input_events_t* s,uint32_t)->const clap_event_header_t*{return &((clap_event_param_value_t*)s->ctx)->header;};
  int gestures=0;clap_output_events_t out{};out.ctx=&gestures;
  out.try_push=[](const clap_output_events_t* s,const clap_event_header_t* e){if(e->type==CLAP_EVENT_PARAM_GESTURE_BEGIN||e->type==CLAP_EVENT_PARAM_GESTURE_END)++*(int*)s->ctx;return true;};
  wrapper.flushParameters(&in,&out,0);auto newer=accepted();require(newer.revision!=old.revision,"equal native host edit accepted");
  service();idle();near(host.mirror[level],newer.value,"newer equal host authority");
  require(accepted().revision==newer.revision,"idle never replays cached intent");
  require(gestures==0 && wrapper.fEventQueue.used==0 && host.flushes==0,"bulk idle emits zero gestures or panel queue events");
  std::cout<<"CLAP exact production idle: repeated INIT/COMPARE/off/import, stopped request/main rescan, text/getter, unlock getter/save reentry, equal Host authority, no duplicate acceptance/gestures pass; desktop bypass\n";
#else
  const int requests=host.requests;replace(0);idle();service();require(host.requests==requests && host.rescans==0,"optout original idle scheduling");
  std::cout<<"CLAP optout original idle lifecycle pass; extension semantics excluded; desktop bypass\n";
#endif
  require(wrapper.getUI()->hide(),"null desktop UI hide safe");wrapper.destroyUI();require(wrapper.getUI()==nullptr,"UI teardown safe");
  service();host.probe=nullptr;return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
