// The VST3 program list a host shows (issue #65): IUnitInfo has to name the 512 library slots,
// not number them. Loads the built plug-in in this process and drives it like a host, the way
// tests/clap_check.cpp does for CLAP, and compares the list against the engine's own library.
// Then saves and reloads a project, to check the program selection the host restores with it
// does not overwrite the restored patch.
//
//   sq8l_vst3_check path/to/SQ8L.vst3/Contents/<arch>/SQ8L.vst3
//
// The user data directory is pointed at a temporary path first: with no SQ8L_backup.dat there the
// plug-in's library is the one a fresh install has, which is what a plain SoundLibrary is too.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "SoundLibrary.h"
#include "travesty/audio_processor.h"
#include "travesty/bstream.h"
#include "travesty/component.h"
#include "travesty/edit_controller.h"
#include "travesty/factory.h"
#include "travesty/unit.h"

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) failures++;
}

void useTempDataDir() {
#if defined(_WIN32)
    char buf[MAX_PATH];
    const DWORD n = GetTempPathA(sizeof(buf), buf);
    const std::string dir = std::string(buf, n) + "sq8l_vst3_check";
    CreateDirectoryA(dir.c_str(), nullptr);
    _putenv_s("APPDATA", dir.c_str());
#else
    const std::string dir = "/tmp/sq8l_vst3_check";
    setenv("HOME", dir.c_str(), 1);
    setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
#endif
}

std::string fromUtf16(const v3_str_128 str) {
    std::string out;
    for (int i = 0; i < 128 && str[i] != 0; ++i) out.push_back(static_cast<char>(str[i] & 0xff));
    return out;
}

// The three host-side objects a save and reload needs. Each is handed over as a pointer to a
// pointer, and v3_cpp_obj() steps the inner one past the funknown, so none of them may have a
// vtable of its own.

struct MemStream : v3_bstream_cpp {
    std::vector<uint8_t> bytes;
    int64_t pos = 0;

    MemStream() {
        query_interface = queryInterface;
        ref = addRef;
        unref = release;
        stream.read = readBytes;
        stream.write = writeBytes;
        stream.seek = seekTo;
        stream.tell = tellPos;
    }

    static MemStream* self(void* p) { return *static_cast<MemStream**>(p); }

    static v3_result V3_API queryInterface(void* p, const v3_tuid iid, void** iface) {
        if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_bstream_iid)) {
            *iface = p;
            return V3_OK;
        }
        *iface = nullptr;
        return V3_NO_INTERFACE;
    }
    static uint32_t V3_API addRef(void*) { return 1; }
    static uint32_t V3_API release(void*) { return 1; }

    static v3_result V3_API readBytes(void* p, void* buffer, int32_t n, int32_t* read) {
        MemStream* const s = self(p);
        const int64_t left = static_cast<int64_t>(s->bytes.size()) - s->pos;
        const int32_t got = static_cast<int32_t>(left < n ? left : n);
        if (got > 0) std::memcpy(buffer, s->bytes.data() + s->pos, static_cast<size_t>(got));
        s->pos += got;
        if (read != nullptr) *read = got;
        return V3_OK;
    }
    static v3_result V3_API writeBytes(void* p, void* buffer, int32_t n, int32_t* written) {
        MemStream* const s = self(p);
        const auto* const in = static_cast<const uint8_t*>(buffer);
        if (static_cast<int64_t>(s->bytes.size()) < s->pos + n) s->bytes.resize(static_cast<size_t>(s->pos + n));
        std::memcpy(s->bytes.data() + s->pos, in, static_cast<size_t>(n));
        s->pos += n;
        if (written != nullptr) *written = n;
        return V3_OK;
    }
    static v3_result V3_API seekTo(void* p, int64_t where, int32_t mode, int64_t* result) {
        MemStream* const s = self(p);
        const int64_t size = static_cast<int64_t>(s->bytes.size());
        s->pos = mode == V3_SEEK_SET ? where : mode == V3_SEEK_CUR ? s->pos + where : size + where;
        if (s->pos < 0) s->pos = 0;
        if (s->pos > size) s->pos = size;
        if (result != nullptr) *result = s->pos;
        return V3_OK;
    }
    static v3_result V3_API tellPos(void* p, int64_t* where) {
        if (where != nullptr) *where = self(p)->pos;
        return V3_OK;
    }
};

struct ParamQueue : v3_param_value_queue_cpp {
    v3_param_id id = 0;
    double value = 0.0;

    ParamQueue() {
        query_interface = queryInterface;
        ref = addRef;
        unref = release;
        queue.get_param_id = paramId;
        queue.get_point_count = pointCount;
        queue.get_point = point;
        queue.add_point = addPoint;
    }

    static ParamQueue* self(void* p) { return *static_cast<ParamQueue**>(p); }

    static v3_result V3_API queryInterface(void* p, const v3_tuid iid, void** iface) {
        if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_param_value_queue_iid)) {
            *iface = p;
            return V3_OK;
        }
        *iface = nullptr;
        return V3_NO_INTERFACE;
    }
    static uint32_t V3_API addRef(void*) { return 1; }
    static uint32_t V3_API release(void*) { return 1; }

    static v3_param_id V3_API paramId(void* p) { return self(p)->id; }
    static int32_t V3_API pointCount(void*) { return 1; }
    static v3_result V3_API point(void* p, int32_t idx, int32_t* offset, double* value) {
        if (idx != 0) return V3_INVALID_ARG;
        *offset = 0;
        *value = self(p)->value;
        return V3_OK;
    }
    static v3_result V3_API addPoint(void*, int32_t, double, int32_t*) { return V3_NOT_IMPLEMENTED; }
};

struct ParamChanges : v3_param_changes_cpp {
    v3_param_value_queue** one = nullptr;

    ParamChanges() {
        query_interface = queryInterface;
        ref = addRef;
        unref = release;
        changes.get_param_count = paramCount;
        changes.get_param_data = paramData;
        changes.add_param_data = addParamData;
    }

    static ParamChanges* self(void* p) { return *static_cast<ParamChanges**>(p); }

    static v3_result V3_API queryInterface(void* p, const v3_tuid iid, void** iface) {
        if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_param_changes_iid)) {
            *iface = p;
            return V3_OK;
        }
        *iface = nullptr;
        return V3_NO_INTERFACE;
    }
    static uint32_t V3_API addRef(void*) { return 1; }
    static uint32_t V3_API release(void*) { return 1; }

    static int32_t V3_API paramCount(void*) { return 1; }
    static v3_param_value_queue** V3_API paramData(void* p, int32_t idx) {
        return idx == 0 ? self(p)->one : nullptr;
    }
    static v3_param_value_queue** V3_API addParamData(void*, const v3_param_id*, int32_t*) { return nullptr; }
};

// The module has to be entered before its factory is asked for anything: that is where DPF
// builds the dummy plug-in instance its class ids and metadata come from.
const void* loadFactory(const char* const path) {
#if defined(_WIN32)
    HMODULE lib = LoadLibraryA(path);
    if (lib == nullptr) return nullptr;
    auto entry = reinterpret_cast<bool (*)()>(GetProcAddress(lib, "InitDll"));
    auto getFactory = reinterpret_cast<const void* (*)()>(GetProcAddress(lib, "GetPluginFactory"));
#else
    void* lib = dlopen(path, RTLD_NOW);
    if (lib == nullptr) {
        std::printf("        dlopen: %s\n", dlerror());
        return nullptr;
    }
#if defined(__APPLE__)
    auto entry = reinterpret_cast<bool (*)(void*)>(dlsym(lib, "bundleEntry"));
#else
    auto entry = reinterpret_cast<bool (*)(void*)>(dlsym(lib, "ModuleEntry"));
#endif
    auto getFactory = reinterpret_cast<const void* (*)()>(dlsym(lib, "GetPluginFactory"));
#endif
    if (entry == nullptr || getFactory == nullptr) return nullptr;
#if defined(_WIN32)
    if (!entry()) return nullptr;
#else
    if (!entry(lib)) return nullptr;
#endif
    return getFactory();
}

// One plug-in instance, driven the way a host drives it: created, initialized, set up for
// processing and switched on.
struct Instance {
    v3_component** component = nullptr;
    v3_audio_processor** processor = nullptr;

    bool open(v3_plugin_factory** factory, const v3_tuid cid) {
        void* ptr = nullptr;
        if (v3_cpp_obj(factory)->create_instance(factory, cid, v3_component_iid, &ptr) != V3_OK || ptr == nullptr)
            return false;
        component = static_cast<v3_component**>(ptr);
        if (v3_cpp_obj_initialize(component, nullptr) != V3_OK) return false;
        if (v3_cpp_obj_query_interface(component, v3_audio_processor_iid, &processor) != V3_OK) return false;
        v3_cpp_obj(component)->activate_bus(component, V3_AUDIO, V3_OUTPUT, 0, true);
        v3_cpp_obj(component)->activate_bus(component, V3_EVENT, V3_INPUT, 0, true);
        v3_process_setup setup = {V3_REALTIME, V3_SAMPLE_32, 64, 44100.0};
        if (v3_cpp_obj(processor)->setup_processing(processor, &setup) != V3_OK) return false;
        return v3_cpp_obj(component)->set_active(component, true) == V3_OK;
    }

    void close() {
        if (component == nullptr) return;
        v3_cpp_obj(component)->set_active(component, false);
        v3_cpp_obj_terminate(component);
        if (processor != nullptr) v3_cpp_obj_unref(processor);
        v3_cpp_obj_unref(component);
        component = nullptr;
        processor = nullptr;
    }

    // One block carrying a value for the program-change parameter, which is how VST3 delivers
    // both a sequencer's program change and a host's own program selection.
    void runBlock(v3_param_id programId, double normalized) {
        ParamQueue queue;
        queue.id = programId;
        queue.value = normalized;
        ParamQueue* queuePtr = &queue;

        ParamChanges changes;
        changes.one = reinterpret_cast<v3_param_value_queue**>(&queuePtr);
        ParamChanges* changesPtr = &changes;

        float left[64] = {}, right[64] = {};
        float* channels[2] = {left, right};
        v3_audio_bus_buffers out = {};
        out.num_channels = 2;
        out.channel_buffers_32 = channels;

        v3_process_data data = {};
        data.process_mode = V3_REALTIME;
        data.symbolic_sample_size = V3_SAMPLE_32;
        data.nframes = 64;
        data.num_output_buses = 1;
        data.outputs = &out;
        data.input_params = reinterpret_cast<v3_param_changes**>(&changesPtr);
        v3_cpp_obj(processor)->process(processor, &data);
    }

    std::vector<uint8_t> save() {
        MemStream stream;
        MemStream* streamPtr = &stream;
        v3_cpp_obj(component)->get_state(component, reinterpret_cast<v3_bstream**>(&streamPtr));
        return stream.bytes;
    }

    bool load(const std::vector<uint8_t>& bytes) {
        MemStream stream;
        stream.bytes = bytes;
        MemStream* streamPtr = &stream;
        return v3_cpp_obj(component)->set_state(component, reinterpret_cast<v3_bstream**>(&streamPtr)) == V3_OK;
    }
};

// The patch a saved state carries: the edit-buffer chunk's hex, past the 0x1f header. The
// header holds an "edited" byte that every setChunk raises, so it differs across a reload and
// is not part of what identifies the patch.
std::string patchOf(const std::vector<uint8_t>& state) {
    const char* const p = reinterpret_cast<const char*>(state.data());
    for (size_t i = 0; i + 11 < state.size(); ++i) {
        if (std::memcmp(p + i, "editbuffer\0", 11) != 0) continue;
        const std::string hex(p + i + 11, std::strlen(p + i + 11));
        return hex.size() > 0x1f * 2 ? hex.substr(0x1f * 2) : std::string();
    }
    return std::string();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: sq8l_vst3_check path/to/SQ8L.vst3\n");
        return 2;
    }

    useTempDataDir();

    auto** const factory = static_cast<v3_plugin_factory**>(const_cast<void*>(loadFactory(argv[1])));
    if (factory == nullptr) {
        std::printf("FAIL  could not load %s\n", argv[1]);
        return 1;
    }

    std::printf("the audio module class\n");
    v3_tuid cid = {};
    bool found = false;
    for (int32_t i = 0, count = v3_cpp_obj(factory)->num_classes(factory); i < count && !found; ++i) {
        v3_class_info info = {};
        if (v3_cpp_obj(factory)->get_class_info(factory, i, &info) != V3_OK) continue;
        if (std::strcmp(info.category, "Audio Module Class") != 0) continue;
        std::memcpy(cid, info.class_id, sizeof(cid));
        found = true;
    }
    check(found, "the factory has an audio module class");
    if (!found) return 1;

    void* componentPtr = nullptr;
    check(v3_cpp_obj(factory)->create_instance(factory, cid, v3_component_iid, &componentPtr) == V3_OK
              && componentPtr != nullptr,
          "the component is created");
    if (componentPtr == nullptr) return 1;

    auto** const component = static_cast<v3_component**>(componentPtr);
    check(v3_cpp_obj_initialize(component, nullptr) == V3_OK, "the component initializes");

    std::printf("the program list\n");
    // v3_cpp_obj() offsets past the funknown part, so the interface's own struct is the type to
    // hold it in, not the _cpp one that has the funknown fields in front.
    v3_unit_information** unitInfo = nullptr;
    check(v3_cpp_obj_query_interface(component, v3_unit_information_iid, &unitInfo) == V3_OK
              && unitInfo != nullptr,
          "the component has IUnitInfo");

    if (unitInfo != nullptr) {
        const v3_unit_information& unit = *v3_cpp_obj(unitInfo);

        check(unit.get_unit_count(unitInfo) == 1, "one unit");
        check(unit.get_program_list_count(unitInfo) == 1, "owning one program list");

        v3_unit_info info = {};
        check(unit.get_unit_info(unitInfo, 0, &info) == V3_OK, "the unit's info");

        v3_program_list_info list = {};
        check(unit.get_program_list_info(unitInfo, 0, &list) == V3_OK, "the list's info");
        check(info.program_list_id == list.id, "the unit owns that list");
        check(list.programCount == sq8l::SoundLibrary::kNumPrograms, "512 programs in it");

        // what the host puts in its preset menu, against what the library would show
        const sq8l::SoundLibrary library;
        std::string firstMismatch;
        int named = 0;
        for (int32_t i = 0; i < list.programCount; ++i) {
            v3_str_128 name = {};
            if (unit.get_program_name(unitInfo, list.id, i, name) != V3_OK) {
                if (firstMismatch.empty()) firstMismatch = "slot " + std::to_string(i) + ": no name";
                continue;
            }
            const std::string got = fromUtf16(name);
            const std::string want = library.programName(static_cast<int>(i));
            if (got != want && firstMismatch.empty())
                firstMismatch = "slot " + std::to_string(i) + ": '" + got + "' not '" + want + "'";
            if (!got.empty()) named++;
        }
        check(named == list.programCount, "every program has a name");
        check(firstMismatch.empty(), firstMismatch.empty() ? "the names are the library's"
                                                           : "the names are the library's (" + firstMismatch + ")");

        v3_str_128 first = {};
        unit.get_program_name(unitInfo, list.id, 0, first);
        std::printf("        program 0 is '%s'\n", fromUtf16(first).c_str());
    }

    // A host restores its own program selection as the project loads, after the state, and in
    // VST3 that selection reaches the plug-in as a program change on the program-change
    // parameter - the same door a sequencer's program change comes through. It must not
    // overwrite the patch the state just restored.
    std::printf("a project save and reload\n");
    {
        v3_param_id programId = 0;
        int32_t steps = 0;
        v3_edit_controller** controller = nullptr;
        if (v3_cpp_obj_query_interface(component, v3_edit_controller_iid, &controller) == V3_OK
            && controller != nullptr) {
            const v3_edit_controller& ec = *v3_cpp_obj(controller);
            for (int32_t i = 0, n = ec.get_parameter_count(controller); i < n; ++i) {
                v3_param_info info = {};
                if (ec.get_parameter_info(controller, i, &info) != V3_OK) continue;
                if ((info.flags & V3_PARAM_PROGRAM_CHANGE) == 0) continue;
                programId = info.param_id;
                steps = info.step_count;
                break;
            }
            v3_cpp_obj_unref(controller);
        }
        check(steps == sq8l::SoundLibrary::kNumPrograms - 1, "a program-change parameter over the 512 slots");

        const double toNormalized = steps > 0 ? 1.0 / steps : 0.0;
        Instance saving;
        check(saving.open(factory, cid), "an instance to save from");
        saving.runBlock(programId, 42 * toNormalized);
        const std::vector<uint8_t> project = saving.save();
        const std::string patch = patchOf(project);
        check(!patch.empty(), "the state carries the edit buffer");
        saving.close();

        Instance reloading;
        check(reloading.open(factory, cid), "an instance to reload into");
        check(reloading.load(project), "the state loads");
        check(patchOf(reloading.save()) == patch, "and brings the patch back");

        // The host's stored selection, arriving after the state the way Cubase sends it.
        reloading.runBlock(programId, 0.0);
        check(patchOf(reloading.save()) == patch, "the selection the host restores leaves the patch alone");

        // Only that one. A program change the player sends still loads.
        reloading.runBlock(programId, 7 * toNormalized);
        const std::string seven = patchOf(reloading.save());
        check(!seven.empty() && seven != patch, "a program change after it still loads");
        reloading.close();
    }

    // Removing the plug-in, in the order a host does it: it holds the unit info for as long as it
    // holds the plug-in, so the component is released first and the unit info is used and released
    // after. The unit info lives inside the component, so deleting the component here would leave
    // the host with freed memory - DPF defers that deletion while a child interface is still out.
    std::printf("the host's release order\n");
    v3_cpp_obj_terminate(component);
    v3_cpp_obj_unref(component);

    if (unitInfo != nullptr) {
        // terminate() took the plug-in behind it, so there is nothing to answer with - but the
        // call has to reach a live object and say so rather than read freed memory
        v3_program_list_info list = {};
        check(v3_cpp_obj(unitInfo)->get_program_list_info(unitInfo, 0, &list) == V3_NOT_INITIALIZED,
              "the unit info is still there to answer after the component was released");
        check(v3_cpp_obj_unref(unitInfo) == 0, "and releasing it afterwards is the last reference");
    }

    v3_cpp_obj_unref(factory);

    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
