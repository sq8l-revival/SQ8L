// Native full-render checker for the C++ engine (no Python, runs on any architecture).
//
//   sq8l_render_check --regression tests/regression
//       Renders the MIDI cases in tests/regression/cases and compares a 64-bit FNV-1a hash
//       of the output with the hash of the ORIGINAL plugin's output (expected.txt).
//       Self-contained: does not need the original DLL.
//   sq8l_render_check tests/golden_raw
//       Compares with full golden renders (tests/export_golden_raw.py, needs the original).
//   sq8l_render_check --polyphony tests/regression
//       Playable voices (EMU -> VOICES and OPTIONS -> Polyphony, port additions): a program
//       without the parameter renders exactly like one asking for the original's 8; every
//       regression case renders identically with 12..64 voices when it never takes over a
//       sounding voice with 8; 1 and 4 voices stay finite; the instance override renders like
//       the same value in the program; then a stress test (40 held notes, 64 voices) on
//       programs of every bank.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "Synth.h"

namespace {

std::vector<uint8_t> readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

// Renders a case (golden_raw / regression format); returns L then R, p = end of MIDI data.
// `voices`: playable voices (8 = the original), set in the program (EMU -> VOICES) or, with
// `viaOverride`, as this instance's override (OPTIONS -> Polyphony); both must render the
// same. `steals`: notes that took a sounding voice.
void render(const std::vector<uint8_t>& buf, size_t& p, std::vector<float>& L, std::vector<float>& R,
            int voices = 8, uint32_t* steals = nullptr, bool viaOverride = false) {
    auto rd = [&](void* dst, size_t k) {
        std::memcpy(dst, buf.data() + p, k);
        p += k;
    };
    int32_t prog, nblocks;
    rd(&prog, 4);
    rd(&nblocks, 4);
    sq8l::Synth synth(44100.0f);
    synth.setSampleRate(44100.0f);
    synth.setProgram(prog);
    if (viaOverride)
        synth.setPolyphonyOverride(voices);
    else
        synth.editBuffer().current().setU8(sq8l::ofs::Polyphony, unsigned(voices));
    for (int b = 0; b < nblocks; b++) {
        int32_t frames, nev;
        rd(&frames, 4);
        rd(&nev, 4);
        std::vector<sq8l::RawMidiEvent> ev(static_cast<size_t>(nev));
        for (auto& e : ev) {
            rd(&e.deltaFrames, 4);
            uint8_t m[4];
            rd(m, 4);
            e.data[0] = m[0], e.data[1] = m[1], e.data[2] = m[2], e.noteOffVelocity = 0;
        }
        if (nev) synth.processEvents(ev.data(), nev);
        std::vector<float> l(static_cast<size_t>(frames)), r(static_cast<size_t>(frames));
        synth.process(l.data(), r.data(), frames, true);
        L.insert(L.end(), l.begin(), l.end());
        R.insert(R.end(), r.begin(), r.end());
    }
    if (steals) *steals = synth.master().stealCount();
}

uint64_t fnv1a64(const void* data, size_t n, uint64_t h = 0xCBF29CE484222325ull) {
    const auto* b = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

int regression(const std::string& dir) {
    std::ifstream exp(dir + "/expected.txt");
    if (!exp) {
        std::fprintf(stderr, "no %s/expected.txt\n", dir.c_str());
        return 2;
    }
    std::string line;
    int cases = 0, bad = 0;
    long long samples = 0;
    while (std::getline(exp, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream is(line);
        std::string name, hex;
        size_t n = 0;
        is >> name >> n >> hex;
        const std::vector<uint8_t> buf = readAll(dir + "/cases/" + name);
        size_t p = 0;
        std::vector<float> L, R;
        render(buf, p, L, R);
        uint64_t h = fnv1a64(L.data(), L.size() * 4);
        h = fnv1a64(R.data(), R.size() * 4, h);
        char got[17];
        std::snprintf(got, sizeof got, "%016llx", static_cast<unsigned long long>(h));
        cases++;
        samples += static_cast<long long>(L.size());
        if (L.size() != n || hex != got) {
            bad++;
            std::printf("DIFF %s (samples %zu/%zu, hash %s, expected %s)\n", name.c_str(), L.size(), n, got,
                        hex.c_str());
        }
    }
    std::printf("%d/%d regression renders identical to the original (%lld samples per channel)\n", cases - bad,
                cases, samples);
    return bad ? 1 : 0;
}

int golden(const std::string& dir) {
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) {
        std::fprintf(stderr, "no %s\n", dir.c_str());
        return 2;
    }
    int files = 0, bad = 0;
    long long samples = 0;
    for (const std::filesystem::directory_entry& de : it) {
        const std::string name = de.path().filename().string();
        if (name.size() < 5 || name.substr(name.size() - 4) != ".bin") continue;
        const std::vector<uint8_t> buf = readAll(dir + "/" + name);
        size_t p = 0;
        std::vector<float> L, R;
        render(buf, p, L, R);
        const size_t total = L.size();
        const bool same = buf.size() - p == total * 8 && std::memcmp(buf.data() + p, L.data(), total * 4) == 0 &&
                          std::memcmp(buf.data() + p + total * 4, R.data(), total * 4) == 0;
        files++;
        samples += static_cast<long long>(total);
        if (!same) {
            bad++;
            std::printf("DIFF %s\n", name.c_str());
        }
    }
    std::printf("%d/%d renders bit-exact (%lld samples per channel)\n", files - bad, files, samples);
    return bad ? 1 : 0;
}

uint64_t hashLR(const std::vector<float>& L, const std::vector<float>& R) {
    return fnv1a64(R.data(), R.size() * 4, fnv1a64(L.data(), L.size() * 4));
}

bool finite(const std::vector<float>& v) {
    for (float x : v)
        if (!std::isfinite(x)) return false;
    return true;
}

int polyphony(const std::string& dir) {
    std::ifstream exp(dir + "/expected.txt");
    if (!exp) {
        std::fprintf(stderr, "no %s/expected.txt\n", dir.c_str());
        return 2;
    }
    const int more[] = {12, 16, 24, 32, 48, 64};  // more than the original's 8
    const int fewer[] = {1, 4};                   // fewer: these do change the render
    int cases = 0, free = 0, bad = 0, stealing = 0;
    std::string line;
    while (std::getline(exp, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream is(line);
        std::string name;
        is >> name;
        const std::vector<uint8_t> buf = readAll(dir + "/cases/" + name);
        size_t p = 0;
        std::vector<float> L8, R8;
        uint32_t steals = 0;
        render(buf, p, L8, R8, 8, &steals);
        const uint64_t h8 = hashLR(L8, R8);
        cases++;
        if (steals) stealing++;
        else free++;
        {
            // A program of the original (and of every bank file, SysEx import and pre-0.90
            // record) has 0 here: it must play exactly like an explicit 8.
            p = 0;
            std::vector<float> L0, R0;
            render(buf, p, L0, R0, 0);
            if (hashLR(L0, R0) != h8) {
                bad++;
                std::printf("DIFF %s: no VOICES in the program vs an explicit 8\n", name.c_str());
            }
        }
        for (int n : more) {
            p = 0;
            std::vector<float> L, R;
            render(buf, p, L, R, n);
            if (!finite(L) || !finite(R) || L.size() != L8.size()) {
                bad++;
                std::printf("BAD %s with %d voices: non-finite output or wrong length\n", name.c_str(), n);
            } else if (!steals && hashLR(L, R) != h8) {
                bad++;
                std::printf("DIFF %s with %d voices (no voice taken over with 8)\n", name.c_str(), n);
            }
            if (n == 16) {  // the instance's override = the same value in the program
                p = 0;
                std::vector<float> Lo, Ro;
                render(buf, p, Lo, Ro, n, nullptr, true);
                if (hashLR(Lo, Ro) != hashLR(L, R)) {
                    bad++;
                    std::printf("DIFF %s: 16 voices from OPTIONS vs from the program\n", name.c_str());
                }
            }
        }
        for (int n : fewer) {
            p = 0;
            std::vector<float> L, R;
            render(buf, p, L, R, n);
            if (!finite(L) || !finite(R) || L.size() != L8.size()) {
                bad++;
                std::printf("BAD %s with %d voices: non-finite output or wrong length\n", name.c_str(), n);
            }
        }
    }
    std::printf("polyphony: %d cases; %d never take over a voice with 8 and render identically with "
                "12/16/24/32/48/64 voices (%s), %d do (more voices change them, output finite); "
                "no VOICES = 8; 1 and 4 voices finite; OPTIONS override = the program's value\n",
                cases, free, bad ? "FAILED" : "ok", stealing);

    // The host chunk carries the instance's override (OPTIONS -> Polyphony), so every plugin
    // format recalls it, and carries nothing when there is none: that chunk must stay exactly
    // the original's bytes.
    int chunkBad = 0;
    {
        sq8l::Synth a(44100.0f);
        const std::vector<uint8_t> plain = a.getChunk();
        a.setPolyphonyOverride(23);
        const std::vector<uint8_t> with = a.getChunk();
        size_t diff = 0;
        for (size_t i = 0; i < plain.size() && i < with.size(); i++)
            if (plain[i] != with[i]) diff++;
        sq8l::Synth b(44100.0f);
        b.setChunk(with.data(), with.size());
        const int restored = b.polyphonyOverride();
        b.setChunk(plain.data(), plain.size());       // a project without an override
        const int cleared = b.polyphonyOverride();
        if (plain[0x1b] || plain[0x1c] || diff != 2 || restored != 23 || cleared != 0) {
            chunkBad++;
            std::printf("BAD chunk: spare bytes %02x %02x, %zu bytes differ, restored %d, cleared %d\n",
                        plain[0x1b], plain[0x1c], diff, restored, cleared);
        }
        std::printf("chunk: no override = the original's bytes; an override adds %zu bytes and comes "
                    "back as %d; a chunk without one restores \"set by program\" (%s)\n",
                    diff, restored, chunkBad ? "FAILED" : "ok");
    }

    // Stress: 40 held notes with 64 voices, programs from every bank, then the releases
    // (and the same with the original's 8 voices, for the level). With 64 voices every held
    // note must sound: no note takes over another.
    int maxActive = 0, programs = 0, stressBad = 0;
    float peak = 0, peak8 = 0;
    for (int run = 0; run < 2; run++)
    for (int prog = 0; prog < 512; prog += 23) {
        sq8l::Synth synth(44100.0f);
        synth.setSampleRate(44100.0f);
        synth.setProgram(prog);
        synth.editBuffer().current().setU8(sq8l::ofs::Polyphony, run ? 8u : 64u);
        std::vector<float> l(512), r(512);
        std::vector<float> all;
        for (int phase = 0; phase < 2; phase++) {
            std::vector<sq8l::RawMidiEvent> ev(40);
            for (int k = 0; k < 40; k++) {
                ev[size_t(k)].deltaFrames = k * 3;
                ev[size_t(k)].data[0] = phase ? 0x80 : 0x90;
                ev[size_t(k)].data[1] = uint8_t(28 + k);
                ev[size_t(k)].data[2] = phase ? 0 : 100;
                ev[size_t(k)].noteOffVelocity = 0;
            }
            synth.processEvents(ev.data(), 40);
            for (int b = 0; b < 172; b++) {  // 2 s
                synth.process(l.data(), r.data(), 512, true);
                if (phase == 0 && !run) maxActive = std::max(maxActive, int(synth.master().activeVoiceCount()));
                all.insert(all.end(), l.begin(), l.end());
                all.insert(all.end(), r.begin(), r.end());
            }
        }
        if (!run) programs++;
        if (!finite(all)) {
            stressBad++;
            std::printf("BAD stress program %d: non-finite output\n", prog);
        }
        for (float x : all) (run ? peak8 : peak) = std::max(run ? peak8 : peak, std::fabs(x));
    }
    std::printf("stress: %d programs, 40 held notes: up to %d voices sounding with 64, output finite; "
                "peak %.2f (with 8 voices: %.2f), %s\n",
                programs, maxActive, double(peak), double(peak8), stressBad || maxActive != 40 ? "FAILED" : "ok");
    return bad || chunkBad || stressBad || maxActive != 40 ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "--regression") return regression(argv[2]);
    if (argc > 2 && std::string(argv[1]) == "--polyphony") return polyphony(argv[2]);
    return golden(argc > 1 ? argv[1] : "tests/golden_raw");
}
