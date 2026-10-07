// Native full-render checker for the C++ engine (no Python, runs on any architecture).
//
//   sq8l_render_check --regression tests/regression
//       Renders the MIDI cases in tests/regression/cases and compares a 64-bit FNV-1a hash
//       of the output with the hash of the ORIGINAL plugin's output (expected.txt).
//       Self-contained: does not need the original DLL.
//   sq8l_render_check tests/golden_raw
//       Compares with full golden renders (tests/export_golden_raw.py, needs the original).
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
void render(const std::vector<uint8_t>& buf, size_t& p, std::vector<float>& L, std::vector<float>& R) {
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

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "--regression") return regression(argv[2]);
    return golden(argc > 1 ? argv[1] : "tests/golden_raw");
}
