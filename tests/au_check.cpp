// The Audio Unit's presets and state as a host drives them around a preset change (issue #37:
// Logic crashed in DISTRHO::PluginAU::retrieveClassInfo). Loads the built component in this
// process, so it needs neither installing nor the system's component registry. A preset name
// handed to or by the plug-in with the wrong ownership is a use after free, so run it with
// MallocScribble=1: freed memory is then overwritten and the mistake crashes every time.
//
//   MallocScribble=1 sq8l_au_check path/to/SQ8L.component
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) failures++;
}

AudioUnit open(const char* path) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8*>(path), static_cast<CFIndex>(std::strlen(path)), true);
    CFBundleRef bundle = CFBundleCreate(nullptr, url);
    CFRelease(url);
    if (bundle == nullptr || !CFBundleLoadExecutable(bundle)) return nullptr;
    auto factory = reinterpret_cast<AudioComponentFactoryFunction>(
        CFBundleGetFunctionPointerForName(bundle, CFSTR("PluginAUFactory")));
    if (factory == nullptr) return nullptr;
    AudioComponentDescription desc{'aumu', 'SQ8L', 'SQ8L', 0, 0};
    AudioComponent comp = AudioComponentRegister(&desc, CFSTR("sq8l_au_check: SQ8L"), 1, factory);
    AudioUnit au = nullptr;
    if (comp == nullptr || AudioComponentInstanceNew(comp, &au) != noErr) return nullptr;
    if (AudioUnitInitialize(au) != noErr) return nullptr;
    return au;
}

OSStatus setPreset(AudioUnit au, SInt32 number, CFStringRef name) {
    AUPreset p{number, name};
    return AudioUnitSetProperty(au, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &p, sizeof(p));
}

// The host owns the name it gets back and releases it (AudioUnitProperties.h).
SInt32 getPreset(AudioUnit au, std::string* name = nullptr) {
    AUPreset p{};
    UInt32 size = sizeof(p);
    if (AudioUnitGetProperty(au, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &p, &size) != noErr)
        return -1000;
    if (name != nullptr) {
        char buf[256] = {};
        if (p.presetName != nullptr) CFStringGetCString(p.presetName, buf, sizeof(buf), kCFStringEncodingUTF8);
        *name = buf;
    }
    if (p.presetName != nullptr) CFRelease(p.presetName);
    return p.presetNumber;
}

// ClassInfo is the plug-in's whole state, which hosts read to save a project or a preset.
CFPropertyListRef classInfo(AudioUnit au) {
    CFPropertyListRef info = nullptr;
    UInt32 size = sizeof(info);
    if (AudioUnitGetProperty(au, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &info, &size) != noErr)
        return nullptr;
    return info;
}

bool restoreClassInfo(AudioUnit au, CFPropertyListRef info) {
    return AudioUnitSetProperty(au, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &info,
                                sizeof(info)) == noErr;
}

// A copy of a ClassInfo with its program number replaced.
CFPropertyListRef withProgram(CFPropertyListRef info, SInt32 program) {
    CFMutableDictionaryRef copy = CFDictionaryCreateMutableCopy(nullptr, 0, static_cast<CFDictionaryRef>(info));
    auto data = static_cast<CFDictionaryRef>(CFDictionaryGetValue(copy, CFSTR(kAUPresetDataKey)));
    CFMutableDictionaryRef dataCopy = CFDictionaryCreateMutableCopy(nullptr, 0, data);
    CFNumberRef num = CFNumberCreate(nullptr, kCFNumberSInt32Type, &program);
    CFDictionarySetValue(dataCopy, CFSTR("program"), num);
    CFDictionarySetValue(copy, CFSTR(kAUPresetDataKey), dataCopy);
    CFRelease(num);
    CFRelease(dataCopy);
    return copy;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // what ran last is the culprit if it crashes
    if (argc < 2) {
        std::fprintf(stderr, "usage: sq8l_au_check path/to/SQ8L.component\n");
        return 2;
    }
    AudioUnit au = open(argv[1]);
    if (au == nullptr) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    std::printf("factory presets\n");
    for (SInt32 n : {3, 4, 3}) {
        check(setPreset(au, n, CFSTR("")) == noErr, "select factory preset " + std::to_string(n));
        for (int i = 0; i < 3; i++) getPreset(au);
        check(getPreset(au) == n, "PresentPreset reads it back, the name still valid");
        CFPropertyListRef info = classInfo(au);
        check(info != nullptr, "ClassInfo after the change");
        if (info != nullptr) CFRelease(info);
    }
    check(setPreset(au, 100000, CFSTR("")) != noErr, "a factory preset past the end is refused");
    check(getPreset(au) == 3, "and the previous one stays selected");

    std::printf("user preset (what Logic sets for its own patches)\n");
    for (int round = 0; round < 3; round++) {
        CFStringRef name = CFStringCreateWithCString(nullptr, "A Logic patch with a long name", kCFStringEncodingUTF8);
        check(setPreset(au, -1, name) == noErr, "set a user preset");
        CFRelease(name);  // the host's own reference ends here
        CFPropertyListRef info = classInfo(au);
        check(info != nullptr, "ClassInfo with the user preset");
        if (info != nullptr) CFRelease(info);
        std::string got;
        check(getPreset(au, &got) == -1 && got == "A Logic patch with a long name", "PresentPreset gives its name back");
    }
    check(setPreset(au, -1, nullptr) == noErr, "a user preset without a name");
    std::string got = "?";
    check(getPreset(au, &got) == -1 && got.empty(), "reads back with an empty name");

    std::printf("state\n");
    check(setPreset(au, 5, CFSTR("")) == noErr, "select factory preset 5");
    CFPropertyListRef saved = classInfo(au);
    check(saved != nullptr, "save the state");
    if (saved != nullptr) {
        check(restoreClassInfo(au, saved), "restore it");
        check(getPreset(au) == 5, "the preset comes back");
        CFPropertyListRef bad = withProgram(saved, 100000);
        restoreClassInfo(au, bad);
        CFRelease(bad);
        CFPropertyListRef info = classInfo(au);
        check(info != nullptr, "ClassInfo after a state with a program past the end");
        if (info != nullptr) CFRelease(info);
        check(getPreset(au) == -1, "which leaves no factory preset selected");
        CFRelease(saved);
    }

    AudioUnitUninitialize(au);
    AudioComponentInstanceDispose(au);
    std::printf(failures == 0 ? "OK: 0 failure(s)\n" : "FAILED: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
