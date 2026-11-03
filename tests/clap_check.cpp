// The CLAP preset list as a host sees it (issue #65): the preset discovery factory declares the
// 512 library slots with their names, and clap.preset-load selects one by its load key. Loads the
// built plug-in in this process and drives it like a host, the way tests/au_check.cpp does for the
// Audio Unit, so it needs nothing installed.
//
//   sq8l_clap_check path/to/SQ8L.clap        (on macOS: .../SQ8L.clap/Contents/MacOS/SQ8L)
//
// The user data directory is pointed at a temporary path first: with no SQ8L_backup.dat there, the
// library is the one a fresh install has - factory banks C and D, with A and B copies of them -
// which is what makes the names predictable.
#include <algorithm>
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
#include "clap/entry.h"
#include "clap/ext/preset-load.h"
#include "clap/ext/state.h"
#include "clap/factory/preset-discovery.h"
#include "clap/plugin-factory.h"

namespace {

constexpr uint32_t kNumPrograms = 512;
constexpr const char* kPluginId = "sq8l.sq8l";

int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) failures++;
}

// ---- the indexer and the metadata receiver a host would pass in ------------------------------

struct Indexer {
    clap_preset_discovery_indexer_t iface;
    std::vector<clap_preset_discovery_location_t> locations;
    int filetypes = 0;

    Indexer() {
        std::memset(&iface, 0, sizeof(iface));
        iface.clap_version = CLAP_VERSION;
        iface.name = "sq8l_clap_check";
        iface.indexer_data = this;
        iface.declare_filetype = [](const clap_preset_discovery_indexer_t* const self,
                                    const clap_preset_discovery_filetype_t*) -> bool {
            static_cast<Indexer*>(self->indexer_data)->filetypes++;
            return true;
        };
        iface.declare_location = [](const clap_preset_discovery_indexer_t* const self,
                                    const clap_preset_discovery_location_t* const location) -> bool {
            static_cast<Indexer*>(self->indexer_data)->locations.push_back(*location);
            return true;
        };
        iface.declare_soundpack = [](const clap_preset_discovery_indexer_t*,
                                     const clap_preset_discovery_soundpack_t*) -> bool { return true; };
        iface.get_extension = [](const clap_preset_discovery_indexer_t*, const char*) -> const void* {
            return nullptr;
        };
    }
};

struct Receiver {
    clap_preset_discovery_metadata_receiver_t iface;
    std::vector<std::string> names, keys, pluginIds;
    int errors = 0;

    Receiver() {
        std::memset(&iface, 0, sizeof(iface));
        iface.receiver_data = this;
        iface.on_error = [](const clap_preset_discovery_metadata_receiver_t* const self, int32_t, const char* const msg) {
            Receiver* const r = static_cast<Receiver*>(self->receiver_data);
            r->errors++;
            std::printf("        provider error: %s\n", msg != nullptr ? msg : "(none)");
        };
        iface.begin_preset = [](const clap_preset_discovery_metadata_receiver_t* const self,
                                const char* const name, const char* const loadKey) -> bool {
            Receiver* const r = static_cast<Receiver*>(self->receiver_data);
            r->names.push_back(name != nullptr ? name : "");
            r->keys.push_back(loadKey != nullptr ? loadKey : "");
            return true;
        };
        iface.add_plugin_id = [](const clap_preset_discovery_metadata_receiver_t* const self,
                                 const clap_universal_plugin_id_t* const id) {
            Receiver* const r = static_cast<Receiver*>(self->receiver_data);
            r->pluginIds.push_back(std::string(id->abi) + ":" + id->id);
        };
        iface.set_soundpack_id = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
        iface.set_flags = [](const clap_preset_discovery_metadata_receiver_t*, uint32_t) {};
        iface.add_creator = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
        iface.set_description = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
        iface.set_timestamps = [](const clap_preset_discovery_metadata_receiver_t*, clap_timestamp, clap_timestamp) {};
        iface.add_feature = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
        iface.add_extra_info = [](const clap_preset_discovery_metadata_receiver_t*, const char*, const char*) {};
    }
};

// ---- the host ---------------------------------------------------------------------------------

struct Host {
    clap_host_t iface;
    clap_host_preset_load_t presetLoad;
    std::string loadedKey;
    int errors = 0;

    Host() {
        presetLoad.on_error = [](const clap_host_t* const host, uint32_t, const char*, const char* const loadKey,
                                 int32_t, const char* const msg) {
            Host* const h = static_cast<Host*>(host->host_data);
            h->errors++;
            std::printf("        preset load error for '%s': %s\n", loadKey != nullptr ? loadKey : "(null)",
                        msg != nullptr ? msg : "(none)");
        };
        presetLoad.loaded = [](const clap_host_t* const host, uint32_t, const char*, const char* const loadKey) {
            static_cast<Host*>(host->host_data)->loadedKey = loadKey != nullptr ? loadKey : "";
        };

        std::memset(&iface, 0, sizeof(iface));
        iface.clap_version = CLAP_VERSION;
        iface.host_data = this;
        iface.name = "sq8l_clap_check";
        iface.vendor = "";
        iface.url = "";
        iface.version = "1.0";
        iface.get_extension = [](const clap_host_t* const host, const char* const id) -> const void* {
            Host* const h = static_cast<Host*>(host->host_data);
            if (std::strcmp(id, CLAP_EXT_PRESET_LOAD) == 0) return &h->presetLoad;
            return nullptr;
        };
        iface.request_restart = [](const clap_host_t*) {};
        iface.request_process = [](const clap_host_t*) {};
        iface.request_callback = [](const clap_host_t*) {};
    }
};

// ---- plumbing ---------------------------------------------------------------------------------

const clap_plugin_entry_t* loadEntry(const char* const path) {
#if defined(_WIN32)
    HMODULE lib = LoadLibraryA(path);
    if (lib == nullptr) return nullptr;
    return reinterpret_cast<const clap_plugin_entry_t*>(GetProcAddress(lib, "clap_entry"));
#else
    void* lib = dlopen(path, RTLD_NOW);
    if (lib == nullptr) {
        std::printf("        dlopen: %s\n", dlerror());
        return nullptr;
    }
    return static_cast<const clap_plugin_entry_t*>(dlsym(lib, "clap_entry"));
#endif
}

void useTempDataDir() {
#if defined(_WIN32)
    char buf[MAX_PATH];
    const DWORD n = GetTempPathA(sizeof(buf), buf);
    const std::string dir = std::string(buf, n) + "sq8l_clap_check";
    CreateDirectoryA(dir.c_str(), nullptr);
    _putenv_s("APPDATA", dir.c_str());
#else
    const std::string dir = "/tmp/sq8l_clap_check";
    setenv("HOME", dir.c_str(), 1);
    setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
#endif
}

std::vector<char> saveState(const clap_plugin_t* const plugin) {
    const clap_plugin_state_t* const state =
        static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
    std::vector<char> out;
    if (state == nullptr) return out;

    clap_ostream_t stream = {};
    stream.ctx = &out;
    stream.write = [](const clap_ostream_t* const s, const void* const buffer, const uint64_t size) -> int64_t {
        std::vector<char>* const v = static_cast<std::vector<char>*>(s->ctx);
        const char* const bytes = static_cast<const char*>(buffer);
        v->insert(v->end(), bytes, bytes + size);
        return static_cast<int64_t>(size);
    };
    if (!state->save(plugin, &stream)) out.clear();
    return out;
}

bool stateHas(const std::vector<char>& state, const std::string& needle) {
    return std::search(state.begin(), state.end(), needle.begin(), needle.end()) != state.end();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: sq8l_clap_check path/to/SQ8L.clap\n");
        return 2;
    }

    useTempDataDir();

    const clap_plugin_entry_t* const entry = loadEntry(argv[1]);
    if (entry == nullptr || !entry->init(argv[1])) {
        std::printf("FAIL  could not load %s\n", argv[1]);
        return 1;
    }

    std::printf("preset discovery factory\n");
    const auto* const factory =
        static_cast<const clap_preset_discovery_factory_t*>(entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
    check(factory != nullptr, "the entry has a preset discovery factory");
    check(entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT) == factory,
          "the draft id gives the same factory");
    if (factory == nullptr) return 1;

    check(factory->count(factory) == 1, "one provider");
    const clap_preset_discovery_provider_descriptor_t* const desc = factory->get_descriptor(factory, 0);
    // the provider's id is the plugin's own: hosts match it to pick the provider for a plugin
    check(desc != nullptr && std::strcmp(desc->id, kPluginId) == 0, "the provider's id is the plug-in id");

    Indexer indexer;
    check(factory->create(factory, &indexer.iface, "not.a.provider") == nullptr, "an unknown provider id is refused");

    const clap_preset_discovery_provider_t* const provider = factory->create(factory, &indexer.iface, desc->id);
    check(provider != nullptr, "the provider is created");
    if (provider == nullptr) return 1;

    check(provider->init(provider), "the provider initializes");
    check(indexer.locations.size() == 1, "one location declared");
    if (indexer.locations.size() == 1) {
        const clap_preset_discovery_location_t& loc = indexer.locations[0];
        check(loc.kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, "the location is inside the plug-in");
        check(loc.location == nullptr, "a plug-in location has no path");
        check((loc.flags & CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT) != 0, "declared as factory content");
    }

    std::printf("the programs as presets\n");
    Receiver receiver;
    check(provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, &receiver.iface),
          "the provider walks the plug-in location");
    check(receiver.errors == 0, "no errors reported");
    check(receiver.names.size() == kNumPrograms, "512 presets declared");
    if (receiver.names.size() == kNumPrograms) {
        // what the host puts in its browser, against what the library would show
        const sq8l::SoundLibrary library;
        bool keysOk = true, namesOk = true;
        std::string firstMismatch;
        for (uint32_t i = 0; i < kNumPrograms; ++i) {
            keysOk = keysOk && receiver.keys[i] == std::to_string(i);
            namesOk = namesOk && !receiver.names[i].empty();
            const std::string want = library.programName(static_cast<int>(i));
            if (receiver.names[i] != want && firstMismatch.empty())
                firstMismatch = "slot " + std::to_string(i) + ": '" + receiver.names[i] + "' not '" + want + "'";
        }
        check(keysOk, "the load keys are the program indices");
        check(namesOk, "every preset has a name");
        check(firstMismatch.empty(), firstMismatch.empty() ? "the names are the library's"
                                                           : "the names are the library's (" + firstMismatch + ")");
        check(receiver.pluginIds.size() == kNumPrograms, "every preset names the plug-in it is for");
        check(!receiver.pluginIds.empty() && receiver.pluginIds[0] == std::string("clap:") + kPluginId,
              "and names it as a CLAP id");
        std::printf("        first names: '%s', '%s', '%s'\n", receiver.names[0].c_str(),
                    receiver.names[1].c_str(), receiver.names[2].c_str());
    }
    provider->destroy(provider);

    std::printf("loading one of them\n");
    const auto* const plugins = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (plugins == nullptr) {
        std::printf("FAIL  no plugin factory\n");
        return 1;
    }

    Host host;
    const clap_plugin_t* const plugin = plugins->create_plugin(plugins, &host.iface, kPluginId);
    check(plugin != nullptr && plugin->init(plugin), "the plug-in is created");
    if (plugin == nullptr) return 1;

    const auto* const presetLoad =
        static_cast<const clap_plugin_preset_load_t*>(plugin->get_extension(plugin, CLAP_EXT_PRESET_LOAD));
    check(presetLoad != nullptr, "the plug-in has clap.preset-load");
    check(plugin->get_extension(plugin, CLAP_EXT_PRESET_LOAD_COMPAT) == presetLoad,
          "the draft id gives the same extension");
    if (presetLoad != nullptr) {
        check(presetLoad->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "300"),
              "preset 300 loads");
        check(host.loadedKey == "300", "the host is told which preset it got");
        // DPF writes the state as NUL separated key/value pairs, the program first
        std::string needle("__dpf_program__");
        needle.push_back('\0');
        needle += "300";
        needle.push_back('\0');
        check(stateHas(saveState(plugin), needle), "the plug-in state is on program 300");

        host.errors = 0;
        check(!presetLoad->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "9999"),
              "a key past the last program is refused");
        check(host.errors == 1, "and the host hears why");
        check(!presetLoad->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE, "some.preset", nullptr),
              "a file location is refused");
    }

    plugin->destroy(plugin);
    entry->deinit();

    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
