// Small native loader seam shared by actual format ABI hosts.
#pragma once
#include <stdexcept>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef near
#undef near // Keep the numeric ABI assertion helper usable after windows.h.
#endif
#else
#include <dlfcn.h>
#endif
namespace automation_check {
inline void* openLibrary(const char* path) {
#ifdef _WIN32
    auto handle = LoadLibraryA(path);
    if (!handle) throw std::runtime_error("LoadLibrary failed: " + std::to_string(GetLastError()));
    return reinterpret_cast<void*>(handle);
#else
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) throw std::runtime_error(dlerror());
    return handle;
#endif
}
inline void* librarySymbol(void* handle, const char* name) {
#ifdef _WIN32
    auto symbol = reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
    void* symbol = dlsym(handle, name);
#endif
    if (!symbol) throw std::runtime_error(std::string("Missing plugin symbol: ") + name);
    return symbol;
}
inline void closeLibrary(void* handle) {
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}
} // namespace automation_check
