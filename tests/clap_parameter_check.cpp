// Native CLAP host fixture. Uses only the published factory/plugin extensions.
#include "SoundParameters.h"
#include "automation_adapter_check.h"
#include "clap/entry.h"
#include "clap/plugin-factory.h"
#include "clap/plugin.h"
#include "clap/ext/params.h"
#include "clap/ext/state.h"
#include "clap/ext/thread-check.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include "automation_dynamic_library.h"
#include <iostream>
#include <limits>
#include <thread>
#include <vector>
using namespace automation_check;
static thread_local bool audioThread = false;
struct Input {
  clap_input_events_t abi{};
  std::vector<clap_event_param_value_t> params;
  std::vector<clap_event_midi_t> midi;
  Input() {
    abi.ctx = this;
    abi.size = [](const clap_input_events_t* a) {auto& s=*(Input*)a->ctx; return uint32_t(s.params.size()+s.midi.size());};
    abi.get = [](const clap_input_events_t* a,uint32_t i) -> const clap_event_header_t* {
      auto& s=*(Input*)a->ctx;
      return i<s.params.size() ? &s.params[i].header : &s.midi[i-s.params.size()].header;
    };
  }
  void add(unsigned id,double value,uint32_t time=0) {
    params.push_back({{sizeof(clap_event_param_value_t),time,0,CLAP_EVENT_PARAM_VALUE,0},id,nullptr,-1,-1,-1,-1,value});
  }
  void message(uint8_t status,uint8_t first,uint8_t second=0,uint32_t time=0) {
    clap_event_midi_t e{}; e.header={sizeof(e),time,0,CLAP_EVENT_MIDI,0};
    e.data[0]=status;e.data[1]=first;e.data[2]=second;midi.push_back(e);
  }
};
struct Output {
  clap_output_events_t abi{};
  bool reject=false;
  int count=0;
  Output() {abi.ctx=this; abi.try_push=[](const clap_output_events_t* a,const clap_event_header_t* e) {
    auto& s=*(Output*)a->ctx; if(s.reject)return false;
    require(e->type!=CLAP_EVENT_PARAM_GESTURE_BEGIN && e->type!=CLAP_EVENT_PARAM_GESTURE_END,"replacement has no gestures");
    if(e->type==CLAP_EVENT_PARAM_VALUE)require(e->flags&CLAP_EVENT_DONT_RECORD,"replacement nonrecording");
    ++s.count;return true;
  };}
};
struct Stream {
  std::vector<char> bytes;
  size_t pos=0;
  clap_ostream_t out{};clap_istream_t in{};
  Stream() {
    out.ctx=this;out.write=[](const clap_ostream_t* a,const void* p,uint64_t n)->int64_t {
      auto& s=*(Stream*)a->ctx;auto* b=(const char*)p;s.bytes.insert(s.bytes.end(),b,b+n);return n;
    };
    in.ctx=this;in.read=[](const clap_istream_t* a,void* p,uint64_t n)->int64_t {
      auto& s=*(Stream*)a->ctx;n=std::min<uint64_t>(n,s.bytes.size()-s.pos);
      std::memcpy(p,s.bytes.data()+s.pos,n);s.pos+=n;return n;
    };
  }
};
struct Host {
  clap_host_t abi{};clap_host_params_t params{};clap_host_thread_check_t threads{};
  std::thread::id main=std::this_thread::get_id();
  const clap_plugin_t* plugin=nullptr;const clap_plugin_params_t* pluginParams=nullptr;
  std::atomic<int> callbacks{0},flushes{0};int rescans=0;
  std::array<double,256> mirror{};
  Host() {
    abi.clap_version=CLAP_VERSION;abi.host_data=this;abi.name="SQ8L ABI regression";abi.vendor="test";abi.url="";abi.version="1";
    abi.get_extension=[](const clap_host_t* h,const char* id)->const void* {
      auto& s=*(Host*)h->host_data;if(!std::strcmp(id,CLAP_EXT_PARAMS))return &s.params;
      if(!std::strcmp(id,CLAP_EXT_THREAD_CHECK))return &s.threads;return nullptr;
    };
    abi.request_restart=[](const clap_host_t*){};abi.request_process=[](const clap_host_t*){};
    abi.request_callback=[](const clap_host_t* h){++((Host*)h->host_data)->callbacks;};
    threads.is_main_thread=[](const clap_host_t* h){return !audioThread && std::this_thread::get_id()==((Host*)h->host_data)->main;};
    threads.is_audio_thread=[](const clap_host_t*){return audioThread;};
    params.rescan=[](const clap_host_t* h,clap_param_rescan_flags flags){
      auto& s=*(Host*)h->host_data;require(s.threads.is_main_thread(h),"rescan on main thread");
      require((flags&(CLAP_PARAM_RESCAN_VALUES|CLAP_PARAM_RESCAN_TEXT))!=0,"rescan values/text");++s.rescans;
      // A separate reader can finish only after the plugin released engine ownership.
      bool read=false;double observed=0;std::thread reader([&]{read=s.pluginParams->get_value(s.plugin,0,&observed);});reader.join();require(read,"cross-thread rescan getter outside engine ownership");
      for(unsigned i=0;i<256;++i)require(s.pluginParams->get_value(s.plugin,i,&s.mirror[i]),"host refresh getter");
    };
    params.clear=[](const clap_host_t*,clap_id,clap_param_clear_flags){};
    params.request_flush=[](const clap_host_t* h){require(!audioThread,"request_flush outside audio thread");++((Host*)h->host_data)->flushes;};
  }
  void service(){if(callbacks.exchange(0))plugin->on_main_thread(plugin);}
};
using Audio=std::array<std::array<float,512>,2>;
struct Lane {
  Host host;const clap_plugin_t* p;const clap_plugin_params_t* params;const clap_plugin_state_t* state;
  bool active=false;
  Lane(const clap_plugin_factory_t* factory,const char* id):p(factory->create_plugin(factory,&host.abi,id)) {
    require(p && p->init(p),"create/init");host.plugin=p;
    params=(const clap_plugin_params_t*)p->get_extension(p,CLAP_EXT_PARAMS);
    state=(const clap_plugin_state_t*)p->get_extension(p,CLAP_EXT_STATE);
    require(params&&state,"extensions");host.pluginParams=params;
  }
  ~Lane(){if(active)stop();p->destroy(p);}
  double get(unsigned i){double v;require(params->get_value(p,i,&v),"get");return v;}
  void start(){require(p->activate(p,44100,1,512),"activate");bool started=false;std::thread t([&]{audioThread=true;started=p->start_processing(p);});t.join();require(started,"start");active=true;}
  void stop(){std::thread t([&]{audioThread=true;p->stop_processing(p);});t.join();p->deactivate(p);active=false;}
  void flush(Input& in,Output& out){
    auto call=[&]{params->flush(p,&in.abi,&out.abi);};
    if(active){std::exception_ptr error;std::thread t([&]{audioThread=true;try{call();}catch(...){error=std::current_exception();}});t.join();if(error)std::rethrow_exception(error);}else call();
  }
  Audio render(Input& in,Output& out) {
    Audio a{};float* channels[]{a[0].data(),a[1].data()};clap_audio_buffer_t bus{};bus.data32=channels;bus.channel_count=2;
    clap_process_t process{};process.steady_time=-1;process.frames_count=512;process.audio_outputs=&bus;process.audio_outputs_count=1;process.in_events=&in.abi;process.out_events=&out.abi;
    clap_process_status status=CLAP_PROCESS_ERROR;std::exception_ptr error;std::thread t([&]{audioThread=true;try{status=p->process(p,&process);}catch(...){error=std::current_exception();}});t.join();if(error)std::rethrow_exception(error);require(status!=CLAP_PROCESS_ERROR,"process");return a;
  }
  std::vector<char> save(){Stream s;require(state->save(p,&s.out),"save");return s.bytes;}
  void restore(const std::vector<char>& b){Stream s;s.bytes=b;require(state->load(p,&s.in),"restore");}
};
static std::vector<char> wireByte(std::vector<char> state,size_t byte,uint8_t value){
  std::string key="editbuffer";auto it=std::search(state.begin(),state.end(),key.begin(),key.end());require(it!=state.end(),"chunk key");
  size_t p=it-state.begin()+key.size()+1+byte*2;require(p+1<state.size(),"chunk extent");const char* hex="0123456789abcdef";
  state[p]=hex[value>>4];state[p+1]=hex[value&15];return state;
}
static std::vector<char> legacyState(std::vector<char> bytes,const char* symbol,const std::string& text){
  if(!bytes.empty()){auto end=std::find(bytes.begin(),bytes.end(),char(0xfe));require(end!=bytes.end(),"legacy state terminator");bytes.erase(end,bytes.end());}
  for(const std::string& token:{std::string("__dpf_parameters_begin__"),std::string(symbol),text,std::string("__dpf_parameters_end__")}){bytes.insert(bytes.end(),token.begin(),token.end());bytes.push_back(0);}
  bytes.push_back(char(0xfe));bytes.push_back(0);return bytes;
}
struct EntryLifetime {
  void* dso=nullptr; const clap_plugin_entry_t* entry=nullptr;
  ~EntryLifetime(){if(entry)entry->deinit();if(dso)closeLibrary(dso);}
};
int main(int argc,char** argv)try{
  const char* path=nullptr;bool optout=false;
  for(int i=1;i<argc;++i){if(!std::strcmp(argv[i],"--plugin")&&i+1<argc)path=argv[++i];else if(!std::strcmp(argv[i],"--cases")&&i+1<argc)++i;else if(!std::strcmp(argv[i],"--optout"))optout=true;else throw std::runtime_error("arguments");}
  require(path&&std::getenv("SQ8L_DATA_DIRECTORY"),"plugin and isolated data directory required");
  EntryLifetime lifetime;
  void* dso=lifetime.dso=openLibrary(path);
  auto* entry=(const clap_plugin_entry_t*)librarySymbol(dso,"clap_entry");require(entry&&entry->init(path),"entry init");lifetime.entry=entry;
  auto* factory=(const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);require(factory&&factory->get_plugin_count(factory)==1,"factory");
  auto* descriptor=factory->get_plugin_descriptor(factory,0);
  {
    Lane lane(factory,descriptor->id);require(lane.params->count(lane.p)==256,"256 parameters");
    Input empty;Output output;lane.flush(empty,output);lane.host.service();auto clean=lane.save();
    for(unsigned i=0;i<256;++i){const auto& def=sq8l::sound::parameters()[i];clap_param_info_t info{};
      require(lane.params->get_info(lane.p,i,&info),"metadata");require(info.id==i && (info.flags&CLAP_PARAM_IS_STEPPED) && (info.flags&CLAP_PARAM_IS_AUTOMATABLE),"stepped automatable identity");
      near(info.min_value,def.minimum,"min");near(info.max_value,def.maximum,"max");near(info.default_value,def.defaultValue,"default");
      for(int v=def.minimum;v<=def.maximum;++v){Input input;input.add(i,v);lane.flush(input,output);near(lane.get(i),v,"all legal choices");char text[256];require(lane.params->value_to_text(lane.p,i,v,text,sizeof text),"value text");}
    }
    lane.restore(clean);
    if(!optout){std::string marker="__dpf_parameters_begin__";require(std::search(clean.begin(),clean.end(),marker.begin(),marker.end())==clean.end(),"no generic parameter projections");
      for(int role=0;role<3;++role){if(role==1)lane.start();
        for(unsigned i=0;i<256;++i){for(double invalid:{std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity()}){
          double before=lane.get(i);Input input;input.add(i,invalid);if(role==2)lane.render(input,output);else lane.flush(input,output);near(lane.get(i),before,"invalid ignored");}
          const auto& d=sq8l::sound::parameters()[i];
          for(double v:{-1e39,1e39,-std::numeric_limits<double>::max(),std::numeric_limits<double>::max()}){
            Input input;input.add(i,v);if(role==2)lane.render(input,output);else lane.flush(input,output);
            near(lane.get(i),v<0?d.minimum:d.maximum,"original-wide finite event clamps before narrowing");
          }}
      }lane.stop();
      unsigned legacyFinite=0,legacyInvalid=0;
      for(const auto& d:sq8l::sound::parameters()){
        for(double v:{double(d.minimum)-100,double(d.maximum)+100,-0.5,0.5,0.5001,31.5,-1e39,1e39,-std::numeric_limits<double>::max(),std::numeric_limits<double>::max()}){
          lane.restore(legacyState(clean,d.symbol,std::to_string(v)));int expected=0;require(sq8l::sound::canonicalize(d,v,expected),"legacy finite expectation");
          near(lane.get(d.index),expected,"legacy native finite clamp/round");near(lane.host.mirror[d.index],expected,"legacy authoritative mirror");
          auto accepted=lane.save();lane.restore(accepted);require(lane.save()==accepted,"legacy canonical state roundtrip");++legacyFinite;
        }
        for(const char* text:{"nan","inf","-inf"}){
          lane.restore(clean);Input valid;valid.add(d.index,d.maximum);lane.flush(valid,output);lane.host.service();auto before=lane.save();
          std::array<double,256> values{};for(unsigned j=0;j<256;++j)values[j]=lane.get(j);
          lane.restore(legacyState({},d.symbol,text));lane.flush(empty,output);lane.host.service();require(lane.save()==before,"nonfinite legacy preserves state and pair intent");
          for(unsigned j=0;j<256;++j)near(lane.get(j),values[j],"nonfinite legacy preserves all accepted values");
          lane.restore(legacyState(before,d.symbol,text));require(lane.save()==before,"nonfinite legacy after chunk preserves state");++legacyInvalid;
        }
      }
      require(legacyFinite==2560&&legacyInvalid==768,"all256 legacy numeric cases");
      std::cout<<"WIDE_NUMERIC event_routes=3 finite_per_route=1024 legacy_finite="<<legacyFinite<<" legacy_invalid="<<legacyInvalid<<"\n";
      lane.restore(clean);
      for(unsigned i=0;i<256;++i){const auto& d=sq8l::sound::parameters()[i];
        for(double v:{double(d.minimum)-100,double(d.maximum)+100,double(d.minimum)+0.5}){
          Input input;input.add(i,v);lane.flush(input,output);
          near(lane.get(i),(d.kind==sq8l::sound::Kind::Boolean ? (v>(d.minimum+d.maximum)*0.5?d.maximum:d.minimum) : std::max(double(d.minimum),std::min(double(d.maximum),std::round(v)))),"finite clamp/round/midpoint");
        }}lane.restore(clean);const auto& pair=*std::find_if(sq8l::sound::parameters().begin(),sq8l::sound::parameters().end(),[](const auto& p){return p.codec==sq8l::sound::Codec::Free;});
      Input edit;edit.add(pair.index,lane.get(pair.index));lane.flush(edit,output);auto explicitState=lane.save();require(explicitState!=clean,"equal edit creates explicit pair intent");lane.restore(explicitState);near(lane.get(pair.index),edit.params[0].value,"stopped pending save roundtrip");
      auto legacy=clean;auto end=std::find(legacy.begin(),legacy.end(),char(0xfe));require(end!=legacy.end(),"state terminator");legacy.erase(end,legacy.end());
      for(const std::string& s:{std::string("__dpf_parameters_begin__"),std::string(pair.symbol),std::string("20"),std::string("__dpf_parameters_end__")}){legacy.insert(legacy.end(),s.begin(),s.end());legacy.push_back(0);}legacy.push_back(char(0xfe));legacy.push_back(0);
      lane.restore(legacy);near(lane.get(pair.index),20,"legacy explicit override");near(lane.host.mirror[pair.index],20,"restore host mirror");
      // Program-change enable is an existing persisted legacy flag, not a test hook.
      auto midiState=clean;std::string key="editbuffer";auto it=std::search(midiState.begin(),midiState.end(),key.begin(),key.end());size_t flag=it-midiState.begin()+key.size()+1+0x1d*2;
      const char* hex="0123456789abcdef";auto lo=std::find(hex,hex+16,midiState[flag+1]);require(lo!=hex+16,"flags");midiState[flag+1]=hex[(lo-hex)|4];lane.restore(midiState);lane.start();
      for(uint32_t offset:{0u,127u}){Input input;input.message(0xc0,uint8_t(offset),0,offset);int scans=lane.host.rescans;output.reject=true;lane.render(input,output);require(lane.host.callbacks>0,"MIDI requests main callback");lane.host.service();require(lane.host.rescans>scans,"MIDI main rescan");for(unsigned i=0;i<256;++i)near(lane.host.mirror[i],lane.get(i),"MIDI host cache");output.reject=false;lane.flush(empty,output);}
      Input select;select.message(0xb0,0,2);select.message(0xc0,13);lane.render(select,output);auto audioBase=lane.save();
      lane.stop();
      // Compare actual rendered process/active flush/inactive flush with an independent raw transpose byte reference.
      for(uint32_t offset:{0u,127u})for(int role=0;role<3;++role){Lane changed(factory,descriptor->id),reference(factory,descriptor->id),control(factory,descriptor->id);
        changed.restore(audioBase);reference.restore(wireByte(audioBase,31+34+1,uint8_t(-12)));control.restore(audioBase);
        const auto& transpose=*std::find_if(sq8l::sound::parameters().begin(),sq8l::sound::parameters().end(),[](const auto& p){return std::string(p.symbol)=="oscillator1_semi";});
        Input trans;trans.add(transpose.index,-12);if(role==2)changed.flush(trans,output);
        changed.start();reference.start();control.start();if(role==1)changed.flush(trans,output);
        Input note,changedNote;note.message(0x90,60,100,offset);changedNote.message(0x90,60,100,offset);if(role==0)changedNote.add(transpose.index,-12);
        auto a=changed.render(changedNote,output),b=reference.render(note,output),c=control.render(note,output);
        double peak=0,diff=0;for(unsigned ch=0;ch<2;++ch)for(unsigned i=0;i<512;++i){require(std::isfinite(a[ch][i])&&std::isfinite(b[ch][i])&&std::isfinite(c[ch][i]),"finite render");peak=std::max(peak,double(std::abs(a[ch][i])));diff=std::max(diff,double(std::abs(a[ch][i]-c[ch][i])));}
        std::cout<<"AUDIO offset="<<offset<<" role="<<role<<" accepted="<<changed.get(transpose.index)<<" control="<<control.get(transpose.index)<<" peak="<<peak<<" difference="<<diff<<std::endl;
        require(std::memcmp(a.data(),b.data(),sizeof a)==0,"changed render equals raw reference");require(peak>1e-5 && diff>1e-5,"audible sensitive render");
        for(unsigned i=0;i<offset;++i)require(a[0][i]==0&&a[1][i]==0,"note offset silent prefix");
        double releasePeak=0;
        for(int block=0;block<8;++block){Input more;if(block==3)more.message(0x80,60,0,127);
          auto x=changed.render(more,output),y=reference.render(more,output);
          require(std::memcmp(x.data(),y.data(),sizeof x)==0,"continued/release bitexact raw reference");
          for(auto& ch:x)for(float sample:ch){require(std::isfinite(sample),"finite continued audio");if(block>3)releasePeak=std::max(releasePeak,double(std::abs(sample)));}
        }require(releasePeak>1e-5,"audible release tail");
      }
      for(uint32_t offset:{0u,127u}) {
        Lane replacement(factory,descriptor->id),loaded(factory,descriptor->id),old(factory,descriptor->id);
        loaded.restore(audioBase);loaded.start();Input select7;select7.message(0xb0,0,2);select7.message(0xc0,7);loaded.render(select7,output);auto target=loaded.save();loaded.stop();
        replacement.restore(audioBase);old.restore(audioBase);loaded.restore(target);replacement.start();old.start();loaded.start();
        Input change,note;change.message(0xb0,0,2,offset);change.message(0xc0,7,0,offset);change.message(0x90,60,100,offset);note.message(0x90,60,100,offset);
        auto a=replacement.render(change,output),b=loaded.render(note,output),c=old.render(note,output);
        require(std::memcmp(a.data(),b.data(),sizeof a)==0,"same-offset MIDI replacement matches preloaded state");
        double difference=0;for(unsigned ch=0;ch<2;++ch)for(unsigned i=0;i<512;++i){require(std::isfinite(a[ch][i]),"finite replacement audio");difference=std::max(difference,double(std::abs(a[ch][i]-c[ch][i])));if(i<offset)require(a[ch][i]==0,"replacement silent prefix");}
        require(difference>1e-5,"MIDI replacement audible sensitivity");std::cout<<"MIDI_AUDIO offset="<<offset<<" difference="<<difference<<std::endl;
        replacement.host.service();for(unsigned i=0;i<256;++i)near(replacement.host.mirror[i],replacement.get(i),"MIDI replacement host cache");
      }
    }
    std::cout<<"CLAP ABI: metadata256/legal20418; process/active flush/inactive flush, invalid inputs, equal intent, stopped state, restore cache, MIDI program0/127 at offsets0/127, main rescan, rendered references passed; optout="<<optout<<"\n";
  }
  return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
