// Global plugin settings (original class CplugConfig, unit plugConfig 0x45269c..0x452db4),
// persisted in <plugin dir>/SQ8L/SQ8L.ini. See docs/modules/settings.md.
//
// The original reads all values with TIniFile.ReadInteger at DLL load (creating an empty
// SQ8L.ini if there is none) and writes them back at unload, but only if the file exists.
// NOTE: the [synth] key names do not match their meaning (the options menu uses the value
// index): "muffleMode" is the DCA1-3 smoothing override, "oscDcaMode" the DCA4 smoothing,
// "dca4Mode" the muffle filter. The accessors below use the real meaning.
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "Program.h"
#include "VoiceSlots.h"

namespace sq8l {

struct Settings {
    static constexpr const char* kFileName = "SQ8L.ini";
    static constexpr int kNumGui = 6, kNumSynth = 5;
    static constexpr const char* kGuiKeys[kNumGui] = {
        "restMouseMenu", "restMouseKnob", "keyCaptMode", "compareOnWrite", "swapProgUpDn",
        "rmbScrollDisplay"};
    static constexpr int kGuiDefaults[kNumGui] = {1, 1, 1, 1, 0, 1};
    static constexpr const char* kSynthKeys[kNumSynth] = {
        "voiceStealMode", "muffleMode", "oscDcaMode", "dca4Mode", "dcbMode"};
    static constexpr int kSynthDefaults[kNumSynth] = {2, 0, 0, 0, 0};

    // Raw values in original order (CplugConfig +0x20 gui[6], +0x38 synth[5]).
    int gui[kNumGui] = {1, 1, 1, 1, 0, 1};
    int synth[kNumSynth] = {2, 0, 0, 0, 0};

    // [port]: options added by the port (not in the original, which ignores the section).
    static constexpr int kNumPort = 2;
    static constexpr const char* kPortKeys[kNumPort] = {"confirmLoad", "polyphony"};
    static constexpr int kPortDefaults[kNumPort] = {1, kOriginalPlayableVoices};
    int port[kNumPort] = {1, kOriginalPlayableVoices};
    // Ask before loading a library, a bank or a SysEx bank over existing programs.
    bool confirmLoading() const { return port[0] > 0; }
    // [port] zoom: size of the editor window in percent (OPTIONS -> Zoom), 100 = the original's
    // 626x430. A preference of the user, not of a project: every editor opens at it.
    static constexpr int kMinZoom = 100, kMaxZoom = 300;
    int zoom = kMinZoom;
    int zoomPercent() const { return zoom < kMinZoom ? kMinZoom : zoom > kMaxZoom ? kMaxZoom : zoom; }
    // [port] hd: the editor drawn at the window's resolution (OPTIONS -> HD graphics) instead
    // of the original's pixels enlarged. Off by default.
    bool hd = false;
    // Playable voices (OPTIONS -> Polyphony), 8 like the original up to 32.
    int polyphony() const {
        return port[1] < kOriginalPlayableVoices ? kOriginalPlayableVoices
               : port[1] > kMaxPlayableVoices    ? kMaxPlayableVoices
                                                 : port[1];
    }

    // [gui] (only the editor uses these). getGuiBool = value > 0.
    bool restoreMouseAfterMenu() const { return gui[0] > 0; }
    bool restoreMouseAfterKnob() const { return gui[1] > 0; }
    int keyCaptureMode() const { return gui[2]; }        // -1 off, 0, 1 (see readme E.10)
    bool compareOnWrite() const { return gui[3] > 0; }
    bool swapProgramUpDown() const { return gui[4] > 0; }
    bool rightClickScrollsDisplay() const { return gui[5] > 0; }

    // [synth] overrides, 0 = "set by program" (copied by the master into +0xfe4..+0xff4).
    int voiceStealOverride() const { return synth[0]; }  // 1 HARD, 2 SOFT     (master +0xfe4)
    int dca13Override() const { return synth[1]; }       // 1 EMU, 2 FAST      (+0xfec, key muffleMode)
    int dca4Override() const { return synth[2]; }        // 1 EMU, 2 HARD      (+0xff0, key oscDcaMode)
    int muffleOverride() const { return synth[3]; }      // 1 OFF, 2 ON        (+0xfe8, key dca4Mode)
    int dcBlockOverride() const { return synth[4]; }     // 1 SMART, 2 ON, 3 OFF (+0xff4)

    // Effective emulation modes for a program, as computed in the voice code (plugCore).
    // Soft voice stealing (FUN_00463388): override > 0 ? override - 2 : prog VSTEAL; -1 = HARD.
    bool softVoiceStealing(const Program& p) const {
        const int v = synth[0] > 0 ? synth[0] - 2 : p.s8(ofs::VoiceSteal);
        return v != -1;
    }
    // DCA1-3 smoothing mode 1 EMU / 2 FAST (+1 of program bits 1-2 of +0x192) (0x463da6).
    int dca13Mode(const Program& p) const {
        return synth[1] > 0 ? synth[1] : int(p.bits(ofs::EmuFlags, 1, 2)) + 1;
    }
    // DCA4 smoothing 0 EMU / 1 HARD (0x4629cb).
    int dca4Mode(const Program& p) const {
        return synth[2] > 0 ? ((synth[2] - 1) & 1) : int(p.bits(ofs::EmuFlags3, 1, 1));
    }
    // Muffle 0 OFF / 1 ON (0x4641a0).
    int muffleMode(const Program& p) const {
        return synth[3] > 0 ? synth[3] - 1 : int((uint32_t(int32_t(p.s8(ofs::EmuFlags2))) >> 2) & 1);
    }
    // DC blocking 0 SMART / 1 ON / 2 OFF (0x463dd1).
    int dcBlockMode(const Program& p) const {
        return synth[4] > 0 ? synth[4] - 1 : int(p.bits(ofs::EmuFlags, 3, 2));
    }

    // Change a value (FUN_00452be8 / FUN_00452cac): returns true if it changed (the original
    // then notifies listeners with the index, synth indices as 0x10 + i).
    bool setGuiBool(int i, bool v) {
        if (i < 0 || i > 5 || gui[i] == (v ? 1 : 0)) return false;
        gui[i] = v ? 1 : 0;
        return true;
    }
    bool setSynth(int i, int v) {
        if (i < 0 || i > 4 || synth[i] == v) return false;
        synth[i] = v;
        return true;
    }
    bool setPort(int i, int v) {
        if (i < 0 || i >= kNumPort || port[i] == v) return false;
        port[i] = v;
        return true;
    }

    // Parse INI text ([gui] and [synth] sections; keys and sections are case insensitive;
    // missing/invalid values keep the defaults, like ReadInteger/StrToIntDef).
    void loadIni(std::string_view text) {
        for (int i = 0; i < kNumGui; i++) gui[i] = kGuiDefaults[i];
        for (int i = 0; i < kNumSynth; i++) synth[i] = kSynthDefaults[i];
        for (int i = 0; i < kNumPort; i++) port[i] = kPortDefaults[i];
        zoom = kMinZoom;
        hd = false;
        std::string section;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string_view::npos) end = text.size();
            std::string line = trim(text.substr(pos, end - pos));
            pos = end + 1;
            if (line.empty() || line[0] == ';') continue;
            if (line[0] == '[') {
                const size_t close = line.find(']');
                section = lower(trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = lower(trim(line.substr(0, eq)));
            const std::string val = trim(line.substr(eq + 1));
            if (section == "gui") {
                for (int i = 0; i < kNumGui; i++)
                    if (key == lower(kGuiKeys[i])) gui[i] = strToIntDef(val, kGuiDefaults[i]);
            } else if (section == "synth") {
                for (int i = 0; i < kNumSynth; i++)
                    if (key == lower(kSynthKeys[i])) synth[i] = strToIntDef(val, kSynthDefaults[i]);
            } else if (section == "port") {
                for (int i = 0; i < kNumPort; i++)
                    if (key == lower(kPortKeys[i])) port[i] = strToIntDef(val, kPortDefaults[i]);
                if (key == "zoom") zoom = strToIntDef(val, kMinZoom);
                if (key == "hd") hd = strToIntDef(val, 0) != 0;
            }
        }
    }

    // INI text as written by the original (TIniFile.WriteInteger for every key), plus [port].
    std::string saveIni() const {
        std::string s = "[gui]\r\n";
        for (int i = 0; i < kNumGui; i++)
            s += std::string(kGuiKeys[i]) + "=" + std::to_string(gui[i]) + "\r\n";
        s += "[synth]\r\n";
        for (int i = 0; i < kNumSynth; i++)
            s += std::string(kSynthKeys[i]) + "=" + std::to_string(synth[i]) + "\r\n";
        s += "[port]\r\n";
        for (int i = 0; i < kNumPort; i++)
            s += std::string(kPortKeys[i]) + "=" + std::to_string(port[i]) + "\r\n";
        s += "zoom=" + std::to_string(zoomPercent()) + "\r\n";
        s += std::string("hd=") + (hd ? "1" : "0") + "\r\n";
        return s;
    }

private:
    static std::string trim(std::string_view s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
        return std::string(s.substr(a, b - a));
    }
    static std::string lower(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }
    // Delphi 5 StrToIntDef (System._ValLong): leading blanks skipped; either an optional sign
    // followed by decimal digits (-2147483648..2147483647) or a hex number introduced by "$",
    // "x", "X" or "0x"/"0X" (up to $FFFFFFFF, wraps to negative); anything else -> default.
    static int strToIntDef(const std::string& s, int def) {
        size_t i = 0;
        while (i < s.size() && s[i] == ' ') i++;
        bool neg = false, hex = false;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
            neg = s[i] == '-';
            i++;
        } else if (i < s.size() && (s[i] == '$' || s[i] == 'x' || s[i] == 'X')) {
            hex = true;
            i++;
        } else if (i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
            hex = true;
            i += 2;
        }
        if (i >= s.size()) return def;
        unsigned long long v = 0;
        for (; i < s.size(); i++) {
            const int c = std::tolower(static_cast<unsigned char>(s[i]));
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else return def;
            v = v * (hex ? 16 : 10) + unsigned(d);
            if (v > 0xffffffffULL) return def;
        }
        if (hex) return static_cast<int>(static_cast<uint32_t>(v));
        if (neg ? v > 0x80000000ULL : v > 0x7fffffffULL) return def;
        return static_cast<int>(neg ? -static_cast<long long>(v) : static_cast<long long>(v));
    }
};

}  // namespace sq8l
