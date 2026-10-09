// The sound library shared by all plugin instances in a process, like the original
// (one CsoundLib per loaded DLL): restored from SQ8L_backup.dat when the first instance
// opens, saved back when the last one closes. Settings (SQ8L.ini) live next to it.
#pragma once

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Settings.h"
#include "SoundLibrary.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace sq8l {

inline std::string userDataDir() {
#if defined(_WIN32)
    const char* base = std::getenv("APPDATA");
    std::string dir = std::string(base ? base : ".") + "\\SQ8L\\";
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    std::string dir = std::string(home ? home : ".") + "/Library/Application Support/SQ8L/";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    std::string dir = xdg ? std::string(xdg) + "/SQ8L/" : std::string(home ? home : ".") + "/.config/SQ8L/";
#endif
    return dir;
}

inline std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

class SharedLibrary {
public:
    struct Handle {
        SoundLibrary* library;
        Settings* settings;
    };

    static Handle acquire() {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.refs++ == 0) {
            const std::string dir = userDataDir();
            s.library = std::make_unique<SoundLibrary>(readFile(dir + "SQ8L_backup.dat"));
            const std::vector<uint8_t> ini = readFile(dir + "SQ8L.ini");
            s.settings = Settings();
            if (!ini.empty()) s.settings.loadIni(std::string(ini.begin(), ini.end()));
        }
        return {s.library.get(), &s.settings};
    }

    // Persist SQ8L.ini (the original writes it on every settings change).
    static void saveSettings() {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        const std::string dir = userDataDir();
        makeDir(dir);
        // Binary: saveIni() already ends every line with CRLF like the original, and on
        // Windows a text-mode stream would translate the LF again and write CR CR LF.
        std::ofstream f(dir + "SQ8L.ini", std::ios::binary | std::ios::trunc);
        f << s.settings.saveIni();
    }

    static void release() {
        auto& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (--s.refs == 0) {
            save(s);
            s.library.reset();
        }
    }

private:
    struct State {
        std::mutex mutex;
        int refs = 0;
        std::unique_ptr<SoundLibrary> library;
        Settings settings;
    };

    static State& state() {
        // Never destroyed: plugin instances (including DPF's metadata instance) may be
        // released during static destruction at process exit.
        static State* s = new State;
        return *s;
    }

    // Create the data folder (one level below an existing parent, like %APPDATA%\SQ8L).
    static void makeDir(const std::string& dir) {
#if defined(_WIN32)
        CreateDirectoryA(dir.c_str(), nullptr);
#else
        ::mkdir(dir.c_str(), 0755);
#endif
    }

    static void save(State& s) {
        const std::string dir = userDataDir();
        makeDir(dir);
        const std::vector<uint8_t> data = s.library->saveBackup();
        std::ofstream f(dir + "SQ8L_backup.dat", std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
};

}  // namespace sq8l
