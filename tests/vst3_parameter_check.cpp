// Native VST3 host regression: load the production DSO and call its actual ABI.
#include "SoundParameters.h"
#include "automation_adapter_check.h"
#include "travesty/audio_processor.h"
#include "travesty/bstream.h"
#include "travesty/component.h"
#include "travesty/edit_controller.h"
#include "travesty/factory.h"
#include <algorithm>
#include <array>
#include <cstring>
#include "automation_dynamic_library.h"
#include <functional>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>
using namespace automation_check;
static v3_result V3_API query(void *, const v3_tuid, void **) {
  return V3_NO_INTERFACE;
}
static uint32_t V3_API ref(void *) { return 1; }
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
struct Events : v3_event_list_cpp {
  Events *ptr = this;
  std::vector<v3_event> values;
  Events() {
    query_interface = query; this->ref = ::ref; unref = ::ref;
    list.get_event_count = [](void *s) { return uint32_t((**(Events **)s).values.size()); };
    list.get_event = [](void *s, int32_t i, v3_event *e) {
      *e = (**(Events **)s).values.at(i); return (v3_result)V3_OK;
    };
  }
  v3_event_list **abi() { return (v3_event_list **)&ptr; }
  void note(int offset, bool on) {
    v3_event e{}; e.sample_offset = offset;
    e.type = on ? V3_EVENT_NOTE_ON : V3_EVENT_NOTE_OFF;
    if (on) { e.note_on.pitch = 60; e.note_on.velocity = 100.0f/127; e.note_on.note_id = -1; }
    else { e.note_off.pitch = 60; e.note_off.note_id = -1; }
    values.push_back(e);
  }
};
using Audio = std::array<std::array<float, 512>, 2>;
struct AudioLane {
  v3_component **component = nullptr;
  v3_audio_processor **processor = nullptr;
  v3_edit_controller **controller = nullptr;
  AudioLane(v3_plugin_factory **factory, const v3_class_info &info) {
    require(v3_cpp_obj(factory)->create_instance(factory, info.class_id, v3_component_iid,
             (void **)&component) == V3_OK, "audio component");
    require(((v3_component_cpp *)*component)->base.initialize(component, nullptr) == V3_OK, "audio initialize");
    require(v3_cpp_obj_query_interface(component, v3_audio_processor_iid, &processor) == V3_OK, "audio processor");
    require(v3_cpp_obj_query_interface(component, v3_edit_controller_iid, &controller) == V3_OK, "audio combined controller");
    v3_process_setup setup{V3_REALTIME, V3_SAMPLE_32, 512, 44100};
    require(v3_cpp_obj(processor)->setup_processing(processor, &setup) == V3_OK, "audio setup");
    require(v3_cpp_obj(component)->set_active(component, true) == V3_OK, "audio activate");
    require(v3_cpp_obj(processor)->set_processing(processor, true) == V3_OK, "audio start");
  }
  ~AudioLane() {
    v3_cpp_obj(processor)->set_processing(processor, false);
    v3_cpp_obj(component)->set_active(component, false);
    v3_cpp_obj_unref(processor); v3_cpp_obj_unref(controller);
    ((v3_component_cpp *)*component)->base.terminate(component); v3_cpp_obj_unref(component);
  }
  std::vector<char> save() {
    Stream s; require(v3_cpp_obj(component)->get_state(component, s.abi()) == V3_OK, "audio save"); return s.bytes;
  }
  void restore(const std::vector<char> &b) {
    Stream s; s.bytes = b;
    require(v3_cpp_obj(component)->set_state(component, s.abi()) == V3_OK, "audio restore");
  }
  Audio render(Changes &in, Events &events, int frames = 512) {
    Audio a{}; float *channels[]{a[0].data(), a[1].data()};
    v3_audio_bus_buffers bus{}; bus.num_channels = 2; bus.channel_buffers_32 = channels;
    Changes out; v3_process_data data{}; data.symbolic_sample_size = V3_SAMPLE_32;
    data.nframes = frames; data.num_output_buses = 1; data.outputs = &bus;
    data.input_params = in.abi(); data.output_params = out.abi(); data.input_events = events.abi();
    require(v3_cpp_obj(processor)->process(processor, &data) == V3_OK, "audio process");
    return a;
  }
};
static std::vector<char> wireByte(std::vector<char> state, size_t byte, uint8_t value) {
  const std::string key = "editbuffer";
  auto it = std::search(state.begin(), state.end(), key.begin(), key.end());
  require(it != state.end(), "audio raw chunk key");
  size_t pos = it - state.begin() + key.size() + 1 + byte * 2;
  require(pos + 1 < state.size(), "audio raw chunk extent");
  const char *hex = "0123456789abcdef";
  state[pos] = hex[value >> 4]; state[pos+1] = hex[value & 15]; return state;
}
static bool sameAudio(const Audio &a, const Audio &b) {
  return std::memcmp(a.data(), b.data(), sizeof(Audio)) == 0;
}
static double peak(const Audio &a) {
  double p = 0;
  for (auto &c : a) for (float v : c) {
    require(std::isfinite(v), "finite stereo audio"); p = std::max(p, std::abs(double(v)));
  }
  return p;
}
static double difference(const Audio &a, const Audio &b) {
  double d = 0;
  for (size_t c = 0; c < 2; ++c) for (size_t i = 0; i < 512; ++i)
    d = std::max(d, std::abs(double(a[c][i]) - b[c][i]));
  return d;
}
static void renderEvidence(v3_plugin_factory **factory, const v3_class_info &info,
                           v3_param_id program, v3_param_id semi,
                           const std::vector<char> &clean) {
  for (int offset : {0, 127}) {
    AudioLane pending(factory, info), reference(factory, info), stale(factory, info),
              early(factory, info), missing(factory, info);
    pending.restore(clean);
    Changes select; Events none;
    // The retained bank is C: MIDI program13 selects built-in factory269.
    select.add(program, 0, 13.0/511); pending.render(select, none);
    auto base = pending.save();
    auto plus = wireByte(base, 31 + 34 + 1, 12);
    auto minus = wireByte(base, 31 + 34 + 1, uint8_t(-12));
    pending.restore(base); reference.restore(plus); stale.restore(base);
    early.restore(minus); missing.restore(plus);
    Changes timing, empty; timing.add(semi, 0, 1); timing.add(semi, 127, 0);
    Events note; note.note(offset, true);
    auto a = pending.render(timing, note), b = reference.render(empty, note),
         c = stale.render(empty, note), d = early.render(empty, note),
         m = missing.render(empty, note);
    require(sameAudio(a,b), "frame-zero audio equals independent raw +12 reference");
    require(peak(a) > 1e-5 && peak(c) > 1e-5 && peak(d) > 1e-5, "audible first callback controls");
    require(difference(a,c) > 1e-5 && difference(a,d) > 1e-5, "first callback sensitive to stale/early trajectory");
    for (int i = 0; i < offset; ++i) require(a[0][i] == 0 && a[1][i] == 0, "note offset silent prefix");
    near(v3_cpp_obj(pending.controller)->get_parameter_normalised(pending.controller, semi), 0,
         "final later input accepted after first render");
    Changes apply; apply.add(semi, 0, 0); reference.render(apply, none, 0);
    double nextDifference = 0, releasePeak = 0;
    for (int block = 0; block < 8; ++block) {
      Events events; if (block == 3) events.note(127, false);
      auto x = pending.render(empty, events), y = reference.render(empty, events),
           z = missing.render(empty, events);
      require(sameAudio(x,y), "next/continued/release audio equals boundary-applied reference");
      if (block == 0) nextDifference = difference(x,z);
      if (block > 3) releasePeak = std::max(releasePeak, peak(x));
    }
    require(nextDifference > 1e-5, "next callback sensitive to missing later input");
    require(releasePeak > 1e-5, "audible release tail");
    std::cout << "AUDIO offset=" << offset << " first_peak=" << peak(a)
              << " stale_difference=" << difference(a,c) << " early_difference=" << difference(a,d)
              << " next_difference=" << nextDifference << " release_peak=" << releasePeak << '\n';
    // Independent preloaded replacement is equivalent when program and note
    // share an offset; the unchanged factory269 control proves sensitivity.
    AudioLane replacement(factory, info), loaded(factory, info), old(factory, info);
    loaded.restore(base); Changes to263; to263.add(program, 0, 7.0/511);
    loaded.render(to263, none); auto sound263 = loaded.save();
    replacement.restore(base); loaded.restore(sound263); old.restore(base);
    Changes change; change.add(program, offset, 7.0/511);
    auto r = replacement.render(change, note), l = loaded.render(empty, note),
         o = old.render(empty, note);
    require(sameAudio(r,l), "MIDI replacement precedes same-offset note");
    require(peak(r) > 1e-5 && difference(r,o) > 1e-5, "audible MIDI replacement control");
    for (int i = 0; i < offset; ++i) require(r[0][i] == 0 && r[1][i] == 0, "replacement note offset prefix");
    std::cout << "MIDI_AUDIO offset=" << offset << " replacement_peak=" << peak(r)
              << " unchanged_difference=" << difference(r,o) << '\n';
    if (offset == 127) {
      AudioLane delayed(factory, info), unchanged(factory, info);
      delayed.restore(base); unchanged.restore(base);
      Events held; held.note(0, true); held.note(127, true); held.values.back().note_on.pitch = 64;
      auto delayedAudio = delayed.render(change, held), oldAudio = unchanged.render(empty, held);
      for (int i = 0; i < 127; ++i)
        require(delayedAudio[0][i] == oldAudio[0][i] && delayedAudio[1][i] == oldAudio[1][i],
                "later MIDI program preserves pre-offset running sound");
      require(difference(delayedAudio, oldAudio) > 1e-5,
              "later MIDI program changes sound after offset");
      std::cout << "MIDI_HELD offset=127 prefix_equal=127 post_difference="
                << difference(delayedAudio, oldAudio) << '\n';
    }


  }
}
int main(int argc, char **argv) try {
  bool optout = argc == 4 && std::string(argv[3]) == "--optout";
  require((argc == 3 || optout) && std::string(argv[1]) == "--plugin",
          "usage: --plugin <DSO>");
  require(std::getenv("SQ8L_DATA_DIRECTORY"),
          "set isolated SQ8L_DATA_DIRECTORY before loading");
  void *lib = openLibrary(argv[2]);

#ifdef __APPLE__
  const char *entryName = "bundleEntry";
  const char *exitName = "bundleExit";
#elif defined(_WIN32)
  const char *entryName = "InitDll";
  const char *exitName = "ExitDll";
#else
  const char *entryName = "ModuleEntry";
  const char *exitName = "ModuleExit";
#endif
#ifdef _WIN32
  require(((bool (*)())librarySymbol(lib, entryName))(), "module entry");
#else
  require(((bool (*)(void *))librarySymbol(lib, entryName))(nullptr), "module entry");
#endif
  auto factory = (v3_plugin_factory **)((const void *(*)())librarySymbol(
      lib, "GetPluginFactory"))();
  v3_class_info info{};
  require(v3_cpp_obj(factory)->get_class_info(factory, 0, &info) == V3_OK,
          "class info");
  v3_component **component = nullptr;
  require(v3_cpp_obj(factory)->create_instance(factory, info.class_id,
                                               v3_component_iid,
                                               (void **)&component) == V3_OK,
          "create component");
  auto *base = (v3_component_cpp *)*component;
  require(base->base.initialize(component, nullptr) == V3_OK, "initialize");
  v3_audio_processor **processor = nullptr;
  require(v3_cpp_obj_query_interface(component, v3_audio_processor_iid,
                                     &processor) == V3_OK,
          "processor");
  v3_edit_controller **controller = nullptr;
  bool separate = v3_cpp_obj_query_interface(component, v3_edit_controller_iid,
                                             &controller) != V3_OK;
  if (separate) {
    v3_tuid id;
    require(v3_cpp_obj(component)->get_controller_class_id(component, id) ==
                V3_OK,
            "controller id");
    require(v3_cpp_obj(factory)->create_instance(factory, id,
                                                 v3_edit_controller_iid,
                                                 (void **)&controller) == V3_OK,
            "create controller");
    auto *b = (v3_edit_controller_cpp *)*controller;
    require(b->base.initialize(controller, nullptr) == V3_OK,
            "controller initialize");
  }
  auto *ctrl = v3_cpp_obj(controller);
  std::vector<v3_param_id> ids;
  v3_param_id program = 0;
  for (int i = 0; i < ctrl->get_parameter_count(controller); ++i) {
    v3_param_info p{};
    require(ctrl->get_parameter_info(controller, i, &p) == V3_OK, "metadata");
    if (p.flags & V3_PARAM_PROGRAM_CHANGE)
      program = p.param_id;
    if (!(p.flags & V3_PARAM_IS_HIDDEN) && (p.flags & V3_PARAM_CAN_AUTOMATE))
      ids.push_back(p.param_id);
  }
  require(ids.size() == 256, "256 sound parameters");
  unsigned nativeChoices = 0;
  for (size_t i = 0; i < ids.size(); ++i) {
    auto &p = sq8l::sound::parameters()[i];
    v3_param_info meta{};
    require(ctrl->get_parameter_info(controller, ids[i], &meta) == V3_OK,
            "metadata id");
    require(meta.step_count == p.maximum - p.minimum, "native steps");
    near(ctrl->normalised_parameter_to_plain(controller, ids[i], 0), p.minimum,
         "native min");
    near(ctrl->normalised_parameter_to_plain(controller, ids[i], 1), p.maximum,
         "native max");
    require(meta.title[0] != 0, "native name");
    for (int value = p.minimum; value <= p.maximum; ++value) {
      double normalized = double(value - p.minimum) / (p.maximum - p.minimum);
      near(ctrl->plain_parameter_to_normalised(controller, ids[i], value), normalized,
           "all legal native normalization");
      near(ctrl->normalised_parameter_to_plain(controller, ids[i], normalized), value,
           "all legal native denormalization");
      v3_str_128 label{};
      require(ctrl->get_parameter_string_for_value(controller, ids[i], normalized, label) == V3_OK,
              "native value label");
      std::string actual;
      for (int j = 0; j < 128 && label[j]; ++j) {
        require(label[j] < 128, "ASCII parameter label"); actual += char(label[j]);
      }
      auto expected = p.kind == sq8l::sound::Kind::Integer ? std::to_string(value)
                                                               : sq8l::sound::choiceLabel(p, value);
      require(actual == expected, "native value label matches registry");
      ++nativeChoices;
    }

  }
  require(nativeChoices == 20418, "all 20418 native choices");
  std::cout << "METADATA native_choices=" << nativeChoices << " normalization/labels passed\n";
  v3_process_setup setup{V3_REALTIME, V3_SAMPLE_32, 128, 48000};
  require(v3_cpp_obj(processor)->setup_processing(processor, &setup) == V3_OK,
          "setup");
  Handler handler;
  require(ctrl->set_component_handler(controller, handler.abi()) == V3_OK,
          "component handler");
  auto save = [&]() {
    Stream s;
    require(v3_cpp_obj(component)->get_state(component, s.abi()) == V3_OK,
            "save");
    return s.bytes;
  };
  auto restore = [&](const std::vector<char> &bytes) {
    Stream s;
    s.bytes = bytes;
    require(v3_cpp_obj(component)->set_state(component, s.abi()) == V3_OK,
            "restore");
    if (separate) {
      s.pos = 0;
      require(ctrl->set_component_state(controller, s.abi()) == V3_OK,
              "controller state");
    }
  };
  auto process = [&](Changes &in, Changes &out, int frames, bool syncController = true) {
    for (int i = 0; i < in.count; ++i) {
      auto &q = in.queues[i];
      if (syncController && q.id >= ids.front() && !q.points.empty())
        require(ctrl->set_parameter_normalised(controller, q.id,
                                               q.points.back().second) == V3_OK,
                "host input controller sync");
    }
    v3_process_data data{};
    data.symbolic_sample_size = V3_SAMPLE_32;
    data.nframes = frames;
    data.input_params = in.abi();
    data.output_params = out.abi();
    handler.processing = true;
    outputProbe = [&]() {
      require(handler.processing, "output queue in process context");
      std::thread reader([&]() { save(); });
      reader.join();
    };
    require(v3_cpp_obj(processor)->process(processor, &data) == V3_OK,
            "process/flush");
    outputProbe = nullptr;
    handler.processing = false;
    for (int i = 0; i < out.count; ++i) {
      auto &q = out.queues[i];
      for (auto p : q.points)
        require(ctrl->set_parameter_normalised(controller, q.id, p.second) ==
                    V3_OK,
                "host controller delivery");
    }
  };
  Changes empty, initial;
  process(empty, initial, 0);
  auto clean = save();
  // MIDI program changes are a persisted instance option. Enable it in the
  // existing legacy chunk flags before taking the stable restored baseline.
  const std::string stateKey = "editbuffer";
  auto keyPos =
      std::search(clean.begin(), clean.end(), stateKey.begin(), stateKey.end());
  require(keyPos != clean.end(), "editbuffer state key");
  size_t hexStart = keyPos - clean.begin() + stateKey.size() + 1;
  const char *hex = "0123456789abcdef";
  auto flagsPos = hexStart + 0x1d * 2 + 1;
  auto nibble = std::find(hex, hex + 16, clean[flagsPos]);
  require(nibble != hex + 16, "chunk flags hex");
  clean[flagsPos] = hex[(nibble - hex) | 4];
  restore(clean);
  clean = save();
  const double initialFirst =
      ctrl->get_parameter_normalised(controller, ids[0]);
  if (!optout) {
    const std::string genericMarker = "__dpf_parameters_begin__";
    require(std::search(clean.begin(), clean.end(), genericMarker.begin(),
                        genericMarker.end()) == clean.end(),
            "authoritative chunk omits generic values");
    auto legacyState = [&](std::vector<char> bytes, const char *symbol,
                           const std::string &text) {
      if (!bytes.empty()) {
        auto end = std::find(bytes.begin(), bytes.end(), char(0xfe));
        require(end != bytes.end(), "legacy state terminator");
        bytes.erase(end, bytes.end());
      }
      auto token = [&](const std::string &text) {
        bytes.insert(bytes.end(), text.begin(), text.end()); bytes.push_back(0);
      };
      token("__dpf_parameters_begin__"); token(symbol); token(text);
      token("__dpf_parameters_end__"); bytes.push_back(char(0xfe)); bytes.push_back(0);
      return bytes;
    };
    unsigned legacyInvalid = 0, legacyFinite = 0, legacyEqual = 0;
    for (size_t i = 0; i < ids.size(); ++i) {
      const auto &p = sq8l::sound::parameters()[i];
      for (const char *text : {"nan", "inf", "-inf"}) {
        restore(clean);
        Changes valid, accepted;
        // Opposite boolean endpoints expose coercion of nonfinite input to zero.
        const double initial = (std::string(text) == "inf" && p.maximum == 1) ? 0 : 1;
        if (p.pair < 0) { valid.add(ids[i], 0, initial); process(valid, accepted, 0); }
        Changes drained; process(empty, drained, 0);
        const auto baseline = save();
        std::array<double, 256> before{};
        for (size_t j = 0; j < ids.size(); ++j)
          before[j] = ctrl->get_parameter_normalised(controller, ids[j]);
        const int gestures = handler.gestures, restarts = handler.restarts;
        // Generic-only restore isolates rejection from legitimate chunk refresh.
        restore(legacyState({}, p.symbol, text));
        Changes output; process(empty, output, 0);
        require(save() == baseline, "invalid legacy input preserves saved bytes and unset pair intent");
        for (size_t j = 0; j < ids.size(); ++j)
          near(ctrl->get_parameter_normalised(controller, ids[j]), before[j],
               "invalid legacy input preserves every controller mirror");
        require(output.count == 0, "invalid legacy input emits no parameter output");
        require(handler.gestures == gestures && handler.restarts == restarts,
                "invalid legacy input emits no gestures or restarts");
        // The same rejection must hold after an authoritative chunk in the stream.
        restore(legacyState(baseline, p.symbol, text));
        require(save() == baseline, "chunk plus invalid legacy input preserves saved bytes");
        for (size_t j = 0; j < ids.size(); ++j)
          near(ctrl->get_parameter_normalised(controller, ids[j]), before[j],
               "chunk plus invalid legacy input preserves controller mirrors");
        ++legacyInvalid;
      }
      for (double input : {double(p.minimum)-20, double(p.maximum)+20,
                           -0.5, 0.5, 0.5001, 31.5, -1e39, 1e39,
                           -std::numeric_limits<double>::max(), std::numeric_limits<double>::max()}) {
        restore(clean);
        int expected = 0;
        require(sq8l::sound::canonicalize(p, input, expected), "finite legacy expectation");
        restore(legacyState(clean, p.symbol, std::to_string(input)));
        near(ctrl->get_parameter_normalised(controller, ids[i]),
             double(expected-p.minimum)/(p.maximum-p.minimum),
             "legacy finite signed/half/clamp/strict boolean canonicalization");
        const auto canonical = save();
        restore(canonical);
        require(save() == canonical, "canonical legacy acceptance roundtrip");
        near(ctrl->get_parameter_normalised(controller, ids[i]),
             double(expected-p.minimum)/(p.maximum-p.minimum), "canonical legacy mirror roundtrip");
        ++legacyFinite;
      }
      if (p.pair < 0) continue;
      restore(clean);
      const double normalized = ctrl->get_parameter_normalised(controller, ids[i]);
      const int plain = int(std::round(normalized*(p.maximum-p.minimum)+p.minimum));
      Changes equal, accepted; equal.add(ids[i], 0, normalized); process(equal, accepted, 0);
      const auto expected = save();
      require(expected != clean, "equal pair processor input establishes intent control");
      restore(legacyState(clean, p.symbol, std::to_string(plain)));
      require(save() == expected, "equal legacy pair overrides chunk with explicit intent");
      restore(save());
      require(save() == expected, "equal legacy pair intent survives roundtrip");
      restore(clean);
      require(save() == clean, "later authoritative chunk clears old explicit pair intent");
      ++legacyEqual;
    }
    require(legacyInvalid == 768 && legacyFinite == 2560 && legacyEqual == 40,
            "all256 legacy numeric cases and all40 pair members covered");
    std::cout << "LEGACY_STATE invalid=" << legacyInvalid << " finite=" << legacyFinite
              << " equal_pair=" << legacyEqual << " saved bytes/mirrors/notifications/precedence passed\n";
    restore(clean);
    // Deliver invalid processor values directly: controller range validation
    // must not hide processor conversion bugs. A fresh restore leaves pairs unset.
    for (int path = 0; path < 3; ++path) {
      for (size_t i = 0; i < ids.size(); ++i) {
        const auto &p = sq8l::sound::parameters()[i];
        for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity()}) {
          restore(clean);
          Changes drain; process(empty, drain, 0);
          if (p.minimum == 0 && p.maximum == 1) {
            Changes valid, accepted;
            valid.add(ids[i], 0, invalid == std::numeric_limits<double>::infinity() ? 0 : 1);
            process(valid, accepted, 0);
          }
          const auto baseline = save();
          std::array<double, 256> before{};
          for (size_t j = 0; j < ids.size(); ++j)
            before[j] = ctrl->get_parameter_normalised(controller, ids[j]);
          const int gestures = handler.gestures, restarts = handler.restarts;
          require(ctrl->set_parameter_normalised(controller, ids[i], invalid) == V3_INVALID_ARG,
                  "invalid controller observation rejected");
          Changes bad, output;
          bad.add(ids[i], path == 2 ? 127 : 0, invalid);
          process(bad, output, path == 0 ? 0 : 128, false);
          for (size_t j = 0; j < ids.size(); ++j)
            near(ctrl->get_parameter_normalised(controller, ids[j]), before[j],
                 "invalid processor preserves all controller mirrors");
          require(save() == baseline, "invalid processor preserves state and unset pair intent");
          require(output.count == 0, "invalid processor emits no parameter notification");
          require(handler.gestures == gestures && handler.restarts == restarts,
                  "invalid processor emits no gestures/restarts");
        }
      }
    }
    restore(clean);
    std::cout << "INVALID all256 NaN/+inf/-inf ignored: flush/frame0/frame127, mirrors/state/unset pairs/opposite booleans\n";
    for (size_t i = 0; i < ids.size(); ++i) {
      const auto &p = sq8l::sound::parameters()[i];
      if (p.minimum != 0 || p.maximum != 1) continue;
      for (auto point : {std::pair<double, double>{-1, 0}, {0.5, 0}, {0.5001, 1}, {2, 1}}) {
        Changes finite, accepted; finite.add(ids[i], 0, point.first);
        process(finite, accepted, 0, false);
        near(ctrl->get_parameter_normalised(controller, ids[i]), point.second,
             "finite boolean clamping and strict midpoint");
      }
    }
    auto level = std::find_if(sq8l::sound::parameters().begin(), sq8l::sound::parameters().end(),
                             [](const auto &p) { return std::string(p.symbol) == "dca1_level"; });
    require(level != sq8l::sound::parameters().end(), "level identity");
    for (auto point : {std::pair<double, double>{-1, 0}, {0.5, 32.0/63}, {2, 1}}) {
      Changes finite, accepted; finite.add(ids[level->index], 0, point.first);
      process(finite, accepted, 0, false);
      near(ctrl->get_parameter_normalised(controller, ids[level->index]), point.second,
           "finite numeric clamping and rounding");
    }
    restore(clean);
    // Host controller echoes must never create explicit Free/Sync pair intent.
    for (auto id : ids)
      require(ctrl->set_parameter_normalised(
                  controller, id,
                  ctrl->get_parameter_normalised(controller, id)) == V3_OK,
              "equal controller observation");
    require(save() == clean,
            "controller echoes preserve state and pair intent");
    for (size_t i = 0; i < ids.size(); ++i) {
      Changes in, out;
      in.add(ids[i], 0, 1);
      process(in, out, 0);
      near(ctrl->get_parameter_normalised(controller, ids[i]), 1,
           "stopped accepted cache");
      auto bytes = save();
      restore(bytes);
      near(ctrl->get_parameter_normalised(controller, ids[i]), 1,
           "stopped save/restore cache");
    }
    unsigned acceptedChoices = 0;
    for (size_t i = 0; i < ids.size(); ++i) {
      const auto &p = sq8l::sound::parameters()[i];
      for (int value = p.minimum; value <= p.maximum; ++value) {
        double normalized = double(value - p.minimum) / (p.maximum - p.minimum);
        Changes in, out; in.add(ids[i], 0, normalized); process(in, out, 0);
        near(ctrl->get_parameter_normalised(controller, ids[i]), normalized,
             "all legal stopped native acceptance");
        ++acceptedChoices;
      }
    }
    require(acceptedChoices == 20418, "all legal stopped choices");
    std::cout << "STOPPED accepted_choices=" << acceptedChoices << '\n';
    restore(clean);
    require(save() == clean, "chunk-only restoration");
    // Explicit equal input is intentional: pair flags must survive state
    // roundtrip.
    auto pair = std::find_if(sq8l::sound::parameters().begin(),
                             sq8l::sound::parameters().end(),
                             [](auto &p) { return p.pair >= 0; });
    require(pair != sq8l::sound::parameters().end(), "pair");
    Changes equal, out;
    equal.add(ids[pair->index], 0,
              ctrl->get_parameter_normalised(controller, ids[pair->index]));
    process(equal, out, 0);
    auto explicitState = save();
    require(explicitState != clean, "equal processor input retains intent");
    restore(explicitState);
    require(save() == explicitState, "equal intent roundtrip");
    // Preserve frame-zero plus final-later-point acceptance.
    Changes timing, timingOut;
    timing.add(ids[0], 0, 0);
    timing.add(ids[0], 127, 1);
    process(timing, timingOut, 128);
    near(ctrl->get_parameter_normalised(controller, ids[0]), 1,
         "final later point");

    auto semi = std::find_if(sq8l::sound::parameters().begin(), sq8l::sound::parameters().end(),
                              [](const auto &p) { return std::string(p.symbol) == "oscillator1_semi"; });
    require(semi != sq8l::sound::parameters().end(), "sensitive semitone identity");
    renderEvidence(factory, info, program, ids[semi->index], clean);

    // Failed/null host output queues retain every pending notification. A newer
    // accepted value supersedes the copied replacement snapshot on retry.
    restore(clean);
    Changes blockedIn, blockedOut;
    blockedOut.reject = true;
    process(blockedIn, blockedOut, 0);
    require(blockedOut.count == 0, "rejected output");
    v3_process_data nullData{};
    nullData.symbolic_sample_size = V3_SAMPLE_32;
    require(v3_cpp_obj(processor)->process(processor, &nullData) == V3_OK,
            "null output flush");
    Changes newer, retry;
    newer.add(ids[0], 0, 1);
    process(newer, retry, 0);
    require(retry.count == 256, "retry all replacement values");
    near(ctrl->get_parameter_normalised(controller, ids[0]), 1,
         "retry observes newer acceptance");
    // Old generic entries after the authoritative chunk remain explicit inputs.
    restore(clean);
    auto legacy = clean;
    auto end = std::find(legacy.begin(), legacy.end(), char(0xfe));
    require(end != legacy.end(), "state terminator");
    legacy.erase(end, legacy.end());
    auto token = [&](const std::string &value) {
      legacy.insert(legacy.end(), value.begin(), value.end());
      legacy.push_back(0);
    };
    token("__dpf_parameters_begin__");
    token(sq8l::sound::parameters()[0].symbol);
    token(std::to_string(sq8l::sound::parameters()[0].maximum));
    token("__dpf_parameters_end__");
    legacy.push_back(char(0xfe));
    legacy.push_back(0);
    restore(legacy);
    near(ctrl->get_parameter_normalised(controller, ids[0]), 1,
         "legacy parameter overrides chunk");
    restore(clean);
    near(ctrl->get_parameter_normalised(controller, ids[0]), initialFirst,
         "later chunk overrides parameter");
    restore(clean);
    Changes drainIn, drainOut;
    process(drainIn, drainOut, 0);
    for (int offset : {0, 127}) {
      const auto before = save();
      Changes replacement, replacementOut;
      replacement.add(program, offset, (offset == 0 ? 13.0 : 7.0) / 511);
      process(replacement, replacementOut, 128);
      require(replacementOut.count == 256, "MIDI replacement output snapshot");
      auto replaced = save();
      require(replaced != before, "MIDI changed actual engine sound");
      for (int i = 0; i < replacementOut.count; ++i) {
        auto &q = replacementOut.queues[i];
        if (q.id >= ids.front())
          near(ctrl->get_parameter_normalised(controller, q.id),
               q.points.back().second, "replacement controller convergence");
      }
      require(save() == replaced, "replacement output echo preserves intent");
    }
  }
  require(handler.gestures == 0, "bulk refresh never records gestures");
  ctrl->set_component_handler(controller, nullptr);
  if (separate)
    v3_cpp_obj_terminate(controller);
  v3_cpp_obj(component)->set_active(component, false);
  v3_cpp_obj_unref(processor);
  v3_cpp_obj_unref(controller);
  base->base.terminate(component);
  v3_cpp_obj_unref(component);
  v3_cpp_obj_unref(factory);
  ((bool (*)())librarySymbol(lib, exitName))();
  closeLibrary(lib);
  std::cout << (optout ? "PASS native VST3 opt-out basic metadata/lifecycle"
                       : "PASS native VST3 256 metadata, stopped "
                         "delivery/state, equal intent, controller "
                         "observations, later timing and replacement output ")
            << "(" << (separate ? "separate" : "combined") << ")\n";
  return 0;
} catch (const std::exception &e) {
  std::cerr << "FAIL " << e.what() << '\n';
  return 1;
}
