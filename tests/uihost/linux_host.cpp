// Minimal VST2 host for Linux: opens the plugin editor in an X11 window at (0, 0) and idles
// it like a host's GUI thread, until `seconds` have passed or the file `stop` appears.
// Used with Xvfb + xdotool to drive the editor (see tests/linux_ui_session.sh). At the end
// it prints the longest idle call: how long the plugin ever held the host's thread (#36).
//
//   linux_host plugin.so seconds [stopfile]
#include <X11/Xlib.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

struct AEffect;
typedef intptr_t (*Dispatcher)(AEffect*, int32_t, int32_t, intptr_t, void*, float);
struct AEffect {
    int32_t magic; Dispatcher dispatcher; void* process; void* setParameter; void* getParameter;
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2; int32_t initialDelay, realQualities, offQualities; float ioRatio;
    void* object; void* user; int32_t uniqueID, version; void* processReplacing; void* processDouble; char future[56];
};
static intptr_t hostCb(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    if (op == 1) return 2400;   // audioMasterVersion
    if (op == 16) return 44100; // audioMasterGetSampleRate
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: linux_host plugin.so seconds [stopfile]\n");
        return 2;
    }
    const double seconds = std::atof(argv[2]);
    const char* stop = argc > 3 ? argv[3] : nullptr;
    void* h = dlopen(argv[1], RTLD_NOW);
    if (!h) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    auto entry = reinterpret_cast<AEffect* (*)(void*)>(dlsym(h, "VSTPluginMain"));
    if (!entry) { std::fprintf(stderr, "no VSTPluginMain\n"); return 1; }
    AEffect* e = entry(reinterpret_cast<void*>(hostCb));
    e->dispatcher(e, 0, 0, 0, nullptr, 0);         // effOpen
    e->dispatcher(e, 10, 0, 0, nullptr, 44100.f);  // effSetSampleRate
    e->dispatcher(e, 12, 0, 1, nullptr, 0);        // effMainsChanged
    int16_t* rect = nullptr;
    e->dispatcher(e, 13, 0, 0, &rect, 0);          // effEditGetRect
    const int w = rect ? rect[3] - rect[1] : 626, hh = rect ? rect[2] - rect[0] : 430;
    Display* d = XOpenDisplay(nullptr);
    if (!d) { std::fprintf(stderr, "no X display\n"); return 1; }
    Window win = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, w, hh, 0, 0, 0);
    XStoreName(d, win, "SQ8L test host");
    XMapWindow(d, win);
    XSync(d, False);
    e->dispatcher(e, 14, 0, 0, reinterpret_cast<void*>(win), 0);  // effEditOpen
    std::printf("editor open %dx%d\n", w, hh);
    std::fflush(stdout);
    double longest = 0;  // ms
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
        struct stat st;
        if (stop && stat(stop, &st) == 0) break;
        while (XPending(d)) {
            XEvent ev;
            XNextEvent(d, &ev);
        }
        const auto t0 = std::chrono::steady_clock::now();
        e->dispatcher(e, 19, 0, 0, nullptr, 0);  // effEditIdle
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > longest) longest = ms;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    e->dispatcher(e, 15, 0, 0, nullptr, 0);  // effEditClose
    XDestroyWindow(d, win);
    XCloseDisplay(d);
    e->dispatcher(e, 12, 0, 0, nullptr, 0);
    e->dispatcher(e, 1, 0, 0, nullptr, 0);   // effClose
    std::printf("longest idle call: %.0f ms\n", longest);
    std::printf("closed\n");
    return 0;
}
