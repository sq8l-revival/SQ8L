// Exact production VST3 wrapper and native host messages; no desktop editor.
#include "SQ8LPlugin.hpp"
#include "DistrhoPluginMain.cpp"
#ifdef near
#undef near // Windows legacy memory-model macro conflicts with the numeric assertion.
#endif
#include "automation_adapter_check.h"
#include <array>
#include <atomic>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>
using namespace automation_check;
static v3_result query(void *, const v3_tuid, void **) {
  return V3_NO_INTERFACE;
}
static uint32_t ref(void *) { return 1; }
struct Stream : v3_bstream_cpp {
  Stream *ptr = this;
  std::vector<char> bytes;
  size_t pos = 0;
  Stream() {
    query_interface = query;
    this->ref = ::ref;
    unref = ::ref;
    stream.read = [](void *s, void *b, int32_t n, int32_t *got) {
      auto &o = **(Stream **)s;
      *got = std::min<size_t>(n, o.bytes.size() - o.pos);
      std::memcpy(b, o.bytes.data() + o.pos, *got);
      o.pos += *got;
      return (v3_result)V3_OK;
    };
    stream.write = [](void *s, void *b, int32_t n, int32_t *got) {
      auto &o = **(Stream **)s;
      auto *p = (char *)b;
      o.bytes.insert(o.bytes.end(), p, p + n);
      *got = n;
      return (v3_result)V3_OK;
    };
  }
  v3_bstream **abi() { return (v3_bstream **)&ptr; }
};
struct Queue : v3_param_value_queue_cpp {
  Queue *ptr = this;
  v3_param_id id = 0;
  std::vector<std::pair<int32_t, double>> points;
  Queue() {
    query_interface = query;
    this->ref = ::ref;
    unref = ::ref;
    queue.get_param_id = [](void *s) { return (**(Queue **)s).id; };
    queue.get_point_count = [](void *s) {
      return (int32_t)(**(Queue **)s).points.size();
    };
    queue.get_point = [](void *s, int32_t i, int32_t *off, double *val) {
      auto &p = (**(Queue **)s).points.at(i);
      *off = p.first;
      *val = p.second;
      return (v3_result)V3_OK;
    };
    queue.add_point = [](void *s, int32_t off, double val, int32_t *i) {
      auto &o = **(Queue **)s;
      *i = o.points.size();
      o.points.emplace_back(off, val);
      return (v3_result)V3_OK;
    };
  }
  v3_param_value_queue **abi() { return (v3_param_value_queue **)&ptr; }
};
static std::function<void()> outputProbe;
static std::function<void()> restartProbe;
struct Handler : v3_component_handler_cpp {
  Handler *ptr = this;
  bool processing = false;
  int gestures = 0, restarts = 0;
  Handler() {
    query_interface = query;
    this->ref = ::ref;
    unref = ::ref;
    comp.begin_edit = [](void *s, v3_param_id) {
      ++(**(Handler **)s).gestures;
      return (v3_result)V3_OK;
    };
    comp.perform_edit = [](void *s, v3_param_id, double) {
      ++(**(Handler **)s).gestures;
      return (v3_result)V3_OK;
    };
    comp.end_edit = comp.begin_edit;
    comp.restart_component = [](void *s, int32_t) {
      auto &o = **(Handler **)s;
      require(!o.processing, "restart outside process");
      ++o.restarts;
      if (restartProbe) restartProbe();
      return (v3_result)V3_OK;
    };
  }
  v3_component_handler **abi() { return (v3_component_handler **)&ptr; }
};
struct Changes : v3_param_changes_cpp {
  Changes *ptr = this;
  std::array<Queue, 512> queues;
  int count = 0;
  bool reject = false;
  Changes() {
    query_interface = query;
    this->ref = ::ref;
    unref = ::ref;
    changes.get_param_count = [](void *s) { return (**(Changes **)s).count; };
    changes.get_param_data = [](void *s, int32_t i) {
      return (**(Changes **)s).queues.at(i).abi();
    };
    changes.add_param_data = [](void *s, const v3_param_id *id, int32_t *idx) {
      auto &o = **(Changes **)s;
      if (outputProbe) {
        auto probe = std::move(outputProbe);
        outputProbe = nullptr;
        probe();
      }
      if (o.reject)
        return (v3_param_value_queue **)nullptr;
      for (int i = 0; i < o.count; ++i)
        if (o.queues[i].id == *id) {
          *idx = i;
          return o.queues[i].abi();
        }
      *idx = o.count++;
      auto &q = o.queues.at(*idx);
      q.id = *id;
      q.points.clear();
      return q.abi();
    };
  }
  v3_param_changes **abi() { return (v3_param_changes **)&ptr; }
  void add(v3_param_id id, int offset, double value) {
    int32_t idx;
    auto **q = changes.add_param_data(abi(), &id, &idx);
    v3_cpp_obj(q)->add_point(q, offset, value, &idx);
  }
};
// All references go through the real plugin ABI. Only the legacy wire byte is
// edited independently; no engine/test injection is used.

struct Message : v3_message_cpp {
  Message* ptr=this;
  std::string id;
  struct Attributes : v3_attribute_list_cpp {
    Attributes* ptr=this;
    std::map<std::string,int64_t> ints;
    std::map<std::string,double> floats;
    Attributes() {
      query_interface=query; this->ref=::ref; unref=::ref;
      attrlist.set_int=[](void* s,const char* k,int64_t v){(**(Attributes**)s).ints[k]=v;return (v3_result)V3_OK;};
      attrlist.get_int=[](void* s,const char* k,int64_t* v){*v=(**(Attributes**)s).ints.at(k);return (v3_result)V3_OK;};
      attrlist.set_float=[](void* s,const char* k,double v){(**(Attributes**)s).floats[k]=v;return (v3_result)V3_OK;};
      attrlist.get_float=[](void* s,const char* k,double* v){*v=(**(Attributes**)s).floats.at(k);return (v3_result)V3_OK;};
      attrlist.set_string=[](void*,const char*,const int16_t*){return (v3_result)V3_OK;};
    }
  } attributes;
  Message(const char* name="") : id(name) {
    query_interface=query; this->ref=::ref;
    unref=[](void* s){delete *(Message**)s;return uint32_t(0);};
    msg.get_message_id=[](void* s){return (**(Message**)s).id.c_str();};
    msg.set_message_id=[](void* s,const char* name){(**(Message**)s).id=name;};
    msg.get_attributes=[](void* s){return (v3_attribute_list**)&(**(Message**)s).attributes.ptr;};
  }
  v3_message** abi(){return (v3_message**)&ptr;}
};
struct Host : v3_host_application_cpp {
  Host* ptr=this;
  Host() {
    query_interface=query; this->ref=::ref; unref=::ref;
    app.create_instance=[](void*,v3_tuid,v3_tuid,void** out){auto* m=new Message;*out=m->abi();return (v3_result)V3_TRUE;};
  }
  v3_host_application** abi(){return (v3_host_application**)&ptr;}
};
struct View : v3_connection_point_cpp {
  View* ptr=this;
  std::map<v3_param_id,double> values;
  std::function<void()> probe;
  View() {
    query_interface=query; this->ref=::ref; unref=::ref;
    point.notify=[](void* s,v3_message** m){
      auto& view=**(View**)s;
      if (view.probe) view.probe();
      if (std::string(v3_cpp_obj(m)->get_message_id(m))=="parameter-set") {
        auto* a=v3_cpp_obj(m)->get_attributes(m);int64_t id;double value;
        v3_cpp_obj(a)->get_int(a,"rindex",&id);v3_cpp_obj(a)->get_float(a,"value",&value);
        view.values[id]=value;
      }
      return (v3_result)V3_OK;
    };
  }
  v3_connection_point** abi(){return (v3_connection_point**)&ptr;}
};
int main() try {
  using namespace DISTRHO;
  d_nextBufferSize=512;d_nextSampleRate=44100.;
  Host host;View view;Handler handler;
  PluginVst3 wrapper(host.abi(),true);
  auto* plugin=static_cast<SQ8LPlugin*>(wrapper.getInstancePointer());
  unsigned level=0;
  for (const auto& p:sq8l::sound::parameters()) if (std::string(p.symbol)=="dca1_level") level=p.index;
  const auto id=kVst3InternalParameterCount+level;
  wrapper.setComponentHandler(handler.abi());wrapper.ctrl2view_connect(view.abi());
  auto accepted=[&](){SQ8LAcceptedParameter v;require(plugin->sq8lReadAcceptedParameter(level,v),"accepted read");return v;};
  auto read=[&](){return wrapper.getParameterNormalized(id);};
  auto save=[&](){Stream stream;require(wrapper.getState(stream.abi())==V3_OK,"save");return stream.bytes;};
  auto ownershipProbe=[&](){std::thread reader([&](){read();save();});reader.join();};
  restartProbe=ownershipProbe;view.probe=ownershipProbe;
  Message init("init"),idle("idle");
  require(wrapper.ctrl2view_notify(init.abi())==V3_OK,"native init");
  // Initial and stopped idle native parameter/ready callbacks probe unlock.
  auto serviceIdle=[&](){require(wrapper.ctrl2view_notify(idle.abi())==V3_OK,"native idle");};
  auto flush=[&](Changes& input,Changes& output){v3_process_data d{};d.symbolic_sample_size=V3_SAMPLE_32;d.input_params=input.abi();d.output_params=output.abi();require(wrapper.process(&d)==V3_OK,"native process flush");};
  Changes empty;
#if DISTRHO_PLUGIN_SQ8L_PARAMETER_REFRESH
  wrapper._setNormalizedPluginParameterValue(level,0);
  const auto before=save();const auto stamp=accepted();
  require(wrapper.setParameterNormalized(id,17.0/63)==V3_OK,"controller observation");
  near(read(),17.0/63,"immediate controller-only synchronization");
  require(save()==before && accepted().revision==stamp.revision,"observation never semantic edit");
  // A later equal acceptance invalidates an older observation by revision.
  wrapper._setNormalizedPluginParameterValue(level,0);
  near(read(),0,"equal host acceptance invalidates observation");
  require(accepted().revision!=stamp.revision,"equal acceptance has exact revision");
  const sq8l::sound::Parameter *free=nullptr,*sync=nullptr;
  for (const auto& p:sq8l::sound::parameters()) if (p.pair==0) {
    if(p.codec==sq8l::sound::Codec::Free) free=&p;
    else sync=&p;
  }
  require(free && sync,"pair identities");
  const auto* mode=sq8l::sound::parameterForField(free->modeField);
  require(mode,"pair mode identity");
  const auto syncId=kVst3InternalParameterCount+sync->index;
  wrapper._setNormalizedPluginParameterValue(mode->index,0);
  wrapper._setNormalizedPluginParameterValue(free->index,100.0/free->maximum);
  SQ8LAcceptedParameter syncBefore,syncAfter;
  plugin->sq8lReadAcceptedParameter(sync->index,syncBefore);
  wrapper.setParameterNormalized(syncId,17.0/sync->maximum);
  near(wrapper.getParameterNormalized(syncId),17.0/sync->maximum,"inactive controller observation");
  wrapper._setNormalizedPluginParameterValue(free->index,20.0/free->maximum);
  plugin->sq8lReadAcceptedParameter(sync->index,syncAfter);
  require(syncBefore.revision==syncAfter.revision && syncBefore.value!=syncAfter.value,
          "paired projection changes without own revision");
  near(wrapper.getParameterNormalized(syncId),syncAfter.value/sync->maximum,"paired projection invalidates baseline");
  {std::lock_guard<std::recursive_mutex> lock(plugin->engineMutex());plugin->synth().editBuffer().initProgram();}
  wrapper._setNormalizedPluginParameterValue(sync->index,5.0/sync->maximum);
  SQ8LAcceptedParameter freeBefore,freeAfter;
  plugin->sq8lReadAcceptedParameter(free->index,freeBefore);
  wrapper.setParameterNormalized(kVst3InternalParameterCount+free->index,17.0/free->maximum);
  wrapper._setNormalizedPluginParameterValue(mode->index,1);
  plugin->sq8lReadAcceptedParameter(free->index,freeAfter);
  require(freeBefore.revision==freeAfter.revision && freeBefore.value!=freeAfter.value,
          "mode changes unset projection without own revision");
  near(wrapper.getParameterNormalized(kVst3InternalParameterCount+free->index),freeAfter.value/free->maximum,
       "mode projection invalidates baseline");
  auto replace=[&](int kind){
    std::lock_guard<std::recursive_mutex> lock(plugin->engineMutex());auto& e=plugin->synth().editBuffer();
    if (kind==0) e.initProgram();
    if (kind==1) e.compare(0,0);
    if (kind==2) e.compareOff();
    if (kind==3) {sq8l::EditBuffer::Chunk chunk;e.copyChunk(chunk);require(e.setChunk(chunk.data(),chunk.size())>0,"import-like chunk replacement");}
  };
  for (int kind: {0,0,1,2,3,0}) {
    // Create a still-valid host-only observation, then invalidate by replacement.
    wrapper.setParameterNormalized(id,17.0/63);
    auto generation=accepted().generation;replace(kind);
    require(accepted().generation!=generation,"real completed replacement");
    int prior=handler.restarts;serviceIdle();
    require(handler.restarts==prior+1,"stopped idle discovers each generation");
    near(read(),accepted().value/63,"controller current after replacement");
    near(view.values.at(id),accepted().value,"native view current after replacement");
    serviceIdle();require(handler.restarts==prior+1,"idle coalesces same generation");
    Changes out;flush(empty,out);require(out.count==256,"idle preserves process transport obligation");
    auto q=std::find_if(out.queues.begin(),out.queues.begin()+out.count,[&](const Queue& q){return q.id==id;});
    require(q!=out.queues.begin()+out.count,"level output");near(q->points.back().second,accepted().value/63,"output authoritative");
  }
  view.probe=nullptr;
  // Processing discovers first; main's independent cursor still restarts the host.
  replace(0);Changes first;flush(empty,first);int prior=handler.restarts;serviceIdle();
  require(handler.restarts==prior+1,"processing cannot steal main discovery");
  // Output refusal preserves pending delivery after main idle consumes its cursor.
  replace(0);serviceIdle();Changes refused;refused.reject=true;flush(empty,refused);
  Changes retry;flush(empty,retry);require(retry.count==256,"output backpressure retry");
  // A stale controller observation tagged at the current accepted point loses
  // to subsequent explicit automation, including equal values.
  wrapper.setParameterNormalized(id,17.0/63);Changes input,out;input.add(id,0,1);flush(input,out);
  near(read(),1,"later native host automation wins");serviceIdle();near(view.values.at(id),63,"UI host automation wins");
  // Finite overlapping production process/idle discovery; no synthetic publisher.
  restartProbe=nullptr;
  std::atomic<bool> go{false};
  std::thread processing([&](){while(!go.load()) std::this_thread::yield();for(int n=0;n<80;++n){Changes o;flush(empty,o);}});
  go=true;for(int n=0;n<80;++n){replace(0);serviceIdle();}
  processing.join();serviceIdle();near(read(),accepted().value/63,"overlap current controller");
  near(view.values.at(id),accepted().value,"overlap current native view");
  require(handler.gestures==0,"bulk refresh emits zero gestures");
  std::cout<<"VST3 production native idle: repeated INIT/COMPARE/import, controller stamp overlay/equal authority, native restart/view, unlock reentry, independent output/retry, overlap pass; no desktop UI\n";
#else
  const double original=read();
  wrapper.setParameterNormalized(id,17.0/63);near(read(),original,"original combined DPF controller observation remains ignored");
  serviceIdle();std::cout<<"VST3 optout native message/controller lifecycle pass; extension semantics excluded\n";
#endif
  wrapper.ctrl2view_disconnect();restartProbe=nullptr;
  return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
