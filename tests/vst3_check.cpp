// The VST3 program list a host shows (issue #65): IUnitInfo has to name the 512 library slots,
// not number them. Loads the built plug-in in this process and drives it like a host, the way
// tests/clap_check.cpp does for CLAP, and compares the list against the engine's own library.
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
#include "travesty/component.h"
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
