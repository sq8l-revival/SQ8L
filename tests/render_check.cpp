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
//   sq8l_render_check --direction
//       Wave direction (OSC -> DIR, a port addition): a reversed oscillator reads its
//       wavesample backwards (every table index the mirror of the forward one), a reversed
//       one-shot halts where it halts forwards and stays halted across control ticks, a
//       reversed looped wave keeps running, and reversing a whole program changes the sound
//       without breaking it. Needs no files.
//   sq8l_render_check --output-gain tests/regression
//       OPTIONS -> Output gain (port addition, issue #52): every cut scales the uncut
//       render by exactly its power of two, and the cut travels in the host chunk.
//   sq8l_render_check --tempo-sync
//       LFO tempo sync (LFO -> SYNC, port addition, issue #42): rate per division and tempo,
//       FM stepping through the divisions, and the phase locked to the song position with
//       RESET=OFF; envelope segments lasting their note value (ENV -> SYNC). Needs no files.
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
// `cut`: this instance's output cut in dB (OPTIONS -> Output gain), 0 = none.
void render(const std::vector<uint8_t>& buf, size_t& p, std::vector<float>& L, std::vector<float>& R,
            int voices = 8, uint32_t* steals = nullptr, bool viaOverride = false, int cut = 0) {
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
    synth.setOutputCut(cut);
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

// (port, issue #52) OPTIONS -> Output gain: the per-instance cut of the output. Every step is
// a whole power of two, so a cut must scale the uncut render by exactly that factor; the cut
// travels in the host chunk like the polyphony override, and a newly created instance starts
// at the cut SQ8L.ini remembers, which the chunk of a project then overrides.
int outputGain(const std::string& dir) {
    std::ifstream exp(dir + "/expected.txt");
    if (!exp) {
        std::fprintf(stderr, "no %s/expected.txt\n", dir.c_str());
        return 2;
    }
    int cases = 0, bad = 0;
    std::string line;
    while (std::getline(exp, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream is(line);
        std::string name;
        is >> name;
        const std::vector<uint8_t> buf = readAll(dir + "/cases/" + name);
        size_t p = 0;
        std::vector<float> L0, R0;
        render(buf, p, L0, R0);
        cases++;
        for (int db : sq8l::Settings::kOutputCutsDb) {
            if (db == 0) continue;
            const float g = sq8l::Settings::outputGainFor(db);
            p = 0;
            std::vector<float> L, R;
            render(buf, p, L, R, 8, nullptr, false, db);
            if (L.size() != L0.size() || R.size() != R0.size()) {
                bad++;
                std::printf("BAD %s at -%d dB: wrong length\n", name.c_str(), db);
                continue;
            }
            // Exact below the top of the subnormal range: scaling by a power of two only
            // changes the exponent, down to where a sample would lose its last bits.
            size_t off = 0;
            for (size_t i = 0; i < L0.size() && !off; i++)
                for (const auto& pair : {std::pair<float, float>{L0[i], L[i]}, {R0[i], R[i]}}) {
                    const float want = pair.first * g;
                    if (std::fabs(want) >= 1e-30f ? pair.second != want : std::fabs(pair.second) > 1e-30f)
                        off = i + 1;
                }
            if (off) {
                bad++;
                std::printf("DIFF %s at -%d dB: sample %zu is not the uncut render times %g\n",
                            name.c_str(), db, off - 1, double(g));
            }
        }
    }
    std::printf("output gain: %d cases, every cut (-6/-12/-18/-24 dB) scales the uncut render "
                "exactly (%s)\n", cases, bad ? "FAILED" : "ok");

    // The chunk carries the cut, and carries nothing when there is none: such a chunk must
    // stay exactly the original's bytes.
    int chunkBad = 0;
    {
        sq8l::Synth a(44100.0f);
        const std::vector<uint8_t> plain = a.getChunk();
        a.setOutputCut(18);
        const std::vector<uint8_t> with = a.getChunk();
        size_t diff = 0;
        for (size_t i = 0; i < plain.size() && i < with.size(); i++)
            if (plain[i] != with[i]) diff++;
        sq8l::Synth b(44100.0f);
        b.setChunk(with.data(), with.size());
        const int restored = b.outputCut();
        b.setChunk(plain.data(), plain.size());  // a project saved without a cut
        const int cleared = b.outputCut();
        // A step this build does not know (a chunk from a later version) reads as no cut.
        std::vector<uint8_t> odd = with;
        odd[0x1e] = 9;
        b.setChunk(odd.data(), odd.size());
        const int unknown = b.outputCut();
        if (plain[0x1e] || diff != 1 || restored != 18 || cleared != 0 || unknown != 0) {
            chunkBad++;
            std::printf("BAD chunk: spare byte %02x, %zu bytes differ, restored %d, cleared %d, "
                        "unknown step %d\n", plain[0x1e], diff, restored, cleared, unknown);
        }
        // Where a newly created instance starts: the cut SQ8L.ini remembers, and no cut when
        // the key holds a step that is not on offer. A chunk always wins over it.
        sq8l::Settings cfg;
        cfg.port[sq8l::Settings::kPortOutputCut] = 12;
        const int fromIni = sq8l::Synth(44100.0f, nullptr, &cfg).outputCut();
        cfg.port[sq8l::Settings::kPortOutputCut] = 7;
        const int fromBadIni = sq8l::Synth(44100.0f, nullptr, &cfg).outputCut();
        cfg.port[sq8l::Settings::kPortOutputCut] = 12;
        sq8l::Synth c(44100.0f, nullptr, &cfg);
        c.setChunk(plain.data(), plain.size());
        const int chunkWins = c.outputCut();
        if (fromIni != 12 || fromBadIni != 0 || chunkWins != 0) {
            chunkBad++;
            std::printf("BAD start: from the ini %d, from a bad ini %d, chunk over the ini %d\n",
                        fromIni, fromBadIni, chunkWins);
        }
        std::printf("chunk: no cut = the original's bytes; a cut adds %zu byte and comes back as "
                    "-%d dB; a chunk without one, or with an unknown step, is no cut; a new "
                    "instance starts at the ini's cut and a chunk overrides it (%s)\n",
                    diff, restored, chunkBad ? "FAILED" : "ok");
    }
    return bad || chunkBad ? 1 : 0;
}

// (port) Wave direction (OSC -> DIR, ofs::Osc::Dir): the oscillator reads its wavesample
// backwards. A looped wave fills its table, so with the same note and frequency the index a
// reversed oscillator reads at step k is the exact mirror of the forward one:
//
//   forward acc = k*freq                 -> index  a           (k*freq = a << shift | b)
//   reverse acc = (N << shift) - 1 - k*freq -> index  N - 1 - a
//
// so the two indices sum to N-1 at every step until the first wrap. That is checked here
// sample by sample, plus the inertness of the parameter and what a whole program renders.
int direction() {
    int bad = 0;
    constexpr uint32_t kSteps = 2000;

    // The DOC clocked at its own rate: one render() is exactly one oscillator step.
    auto indices = [](int wave, bool reverse, std::vector<uint32_t>& idx, uint32_t& tableSize) {
        sq8l::Doc d(1, 1);
        d.setSampleRate(sq8l::Doc::kDocRate);
        d.setOscReverse(0, 0, reverse);
        sq8l::DocVoiceParams* p = d.params(0);
        for (int i = 0; i < 3; i++) p->osc[i] = sq8l::DocOscParams{};
        p->osc[0].enabled = -1;
        p->osc[0].wave = wave;
        p->osc[0].level = 0x7f00;
        d.startVoice(0, 60, -1, -1, -1);
        d.update(0);
        const sq8l::DocOsc& o = d.voice[0].osc[0];
        tableSize = static_cast<uint32_t>(o.tableSize);
        idx.clear();
        for (uint32_t k = 0; k < kSteps; k++) {
            idx.push_back(o.acc >> (o.shift & 31));
            d.render(0);
            if (o.halted != 0) break;
        }
    };

    for (int wave : {0, 2, 20, 33, 44}) {  // looped waves (< 54): the table is the whole loop
        std::vector<uint32_t> f, r;
        uint32_t nf = 0, nr = 0;
        indices(wave, false, f, nf);
        indices(wave, true, r, nr);
        const size_t n = std::min(f.size(), r.size());
        size_t mismatch = 0;
        for (size_t k = 0; k < n; k++)
            if (f[k] + r[k] != nf - 1) mismatch++;
        const bool moved = n > 1 && f[1] != f[0] && r[1] != r[0];
        if (nf != nr || n < kSteps || mismatch || !moved) {
            bad++;
            std::printf("BAD wave %d: table %u/%u, %zu steps, %zu not mirrored, moved %d\n", wave, nf, nr, n,
                        mismatch, int(moved));
        }
    }
    std::printf("direction: 5 looped waves, %u steps each, every reversed table index the mirror of the "
                "forward one (%s)\n",
                kSteps, bad ? "FAILED" : "ok");

    // One-shot waves (>= 54) must stop reversed exactly as they stop forwards, and STAY
    // stopped: update() runs every control tick and clears `halted` unconditionally, so a
    // one-shot only stays dead because its accumulator is parked on the terminator and keeps
    // reading that zero. Reverse used to wrap back to live data instead, and the voice played
    // the wave backwards again on every tick -- a one-shot turned into a loop.
    int oneShotBad = 0;
    auto haltRun = [](int wave, bool reverse, int& haltStep, int& movedAfter) {
        sq8l::Doc d(1, 1);
        d.setSampleRate(sq8l::Doc::kDocRate);
        d.setOscReverse(0, 0, reverse);
        sq8l::DocVoiceParams* p = d.params(0);
        for (int i = 0; i < 3; i++) p->osc[i] = sq8l::DocOscParams{};
        p->osc[0].enabled = -1;
        p->osc[0].wave = wave;
        p->osc[0].level = 0x7f00;
        d.startVoice(0, 60, -1, -1, -1);
        d.update(0);
        const sq8l::DocOsc& o = d.voice[0].osc[0];
        haltStep = -1;
        movedAfter = 0;
        uint32_t prev = o.acc;
        for (int k = 0; k < 120000; k++) {
            d.render(0);
            if (o.halted != 0 && haltStep < 0) haltStep = k;
            if (haltStep >= 0 && o.acc != prev) movedAfter++;
            prev = o.acc;
            if ((k % 64) == 63) d.update(0);  // the control tick the master runs
        }
    };
    for (int wave = 54; wave < 75; wave++) {
        int hf = 0, hr = 0, mf = 0, mr = 0;
        haltRun(wave, false, hf, mf);
        haltRun(wave, true, hr, mr);
        // the same length within a step (the wrapped position costs forward one extra sample)
        if (hf < 0 || hr < 0 || std::abs(hf - hr) > 1 || mf != 0 || mr != 0) {
            oneShotBad++;
            std::printf("BAD one-shot wave %d: halt fwd %d rev %d, moved after halt fwd %d rev %d\n", wave, hf, hr,
                        mf, mr);
        }
    }
    std::printf("one-shot: waves 54-74 reversed halt where they halt forwards and stay halted across "
                "control ticks (%s)\n",
                oneShotBad ? "FAILED" : "ok");

    // A looped wave has no terminator, so reverse keeps going, as it does forwards.
    int loopBad = 0;
    for (int wave : {0, 2, 20, 44, 53}) {
        int h = 0, m = 0;
        haltRun(wave, true, h, m);
        if (h >= 0) {
            loopBad++;
            std::printf("BAD looped wave %d reversed: halted at step %d\n", wave, h);
        }
    }
    std::printf("looped: 5 waves reversed keep running, as they do forwards (%s)\n", loopBad ? "FAILED" : "ok");
    oneShotBad += loopBad;

    // A whole program: the byte the original never writes leaves the render untouched, and
    // turning it on changes the sound without breaking it.
    int progBad = 0;
    auto renderProg = [](int prog, int dir, std::vector<float>& L, std::vector<float>& R) {
        sq8l::Synth synth(44100.0f);
        synth.setSampleRate(44100.0f);
        synth.setProgram(prog);
        for (int i = 0; i < 3; i++)
            synth.editBuffer().current().setU8(sq8l::ofs::osc(i) + sq8l::ofs::Osc::Dir, uint8_t(dir));
        std::vector<sq8l::RawMidiEvent> ev(1);
        ev[0].deltaFrames = 0;
        ev[0].data[0] = 0x90;
        ev[0].data[1] = 60;
        ev[0].data[2] = 100;
        ev[0].noteOffVelocity = 0;
        synth.processEvents(ev.data(), 1);
        std::vector<float> l(512), r(512);
        for (int b = 0; b < 86; b++) {  // 1 s
            synth.process(l.data(), r.data(), 512, true);
            L.insert(L.end(), l.begin(), l.end());
            R.insert(R.end(), r.begin(), r.end());
        }
    };
    int changed = 0, programs = 0;
    for (int prog = 0; prog < 512; prog += 23) {
        std::vector<float> L0, R0, L1, R1;
        renderProg(prog, 0, L0, R0);
        renderProg(prog, 1, L1, R1);
        programs++;
        if (!finite(L1) || !finite(R1) || L1.size() != L0.size()) {
            progBad++;
            std::printf("BAD program %d reversed: non-finite output or wrong length\n", prog);
        }
        if (hashLR(L0, R0) != hashLR(L1, R1)) changed++;
    }
    if (changed == 0) {
        progBad++;
        std::printf("BAD: reversing every oscillator changed no program\n");
    }
    std::printf("programs: %d rendered, %d sound different reversed, output finite (%s)\n", programs, changed,
                progBad ? "FAILED" : "ok");
    return bad || oneShotBad || progBad ? 1 : 0;
}

// LFO1 of the INIT program with SYNC on, playing keys at 44.1 kHz in blocks of 512.
struct SyncRun {
    sq8l::Synth synth{44100.0f};
    double quarters = 0;  // the song position at the start of the next block

    SyncRun(int freq, int reset, int fmAmount = 0, bool reverse = false) {
        synth.setSampleRate(44100.0f);
        synth.setProgram(0);
        sq8l::Program& p = synth.editBuffer().current();
        sq8l::initProgram(p);
        uint8_t* l = p.bytes + sq8l::ofs::lfo(0);
        l[sq8l::ofs::Lfo::Freq] = uint8_t(freq);
        l[sq8l::ofs::Lfo::Reset] = uint8_t(reset);
        l[sq8l::ofs::Lfo::Modes] = uint8_t(sq8l::ofs::Lfo::SyncBit | (reverse ? 0x10 : 0));  // FM mode UNI
        if (fmAmount) p.setMod(sq8l::ofs::lfo(0) + sq8l::ofs::Lfo::FreqMod, {sq8l::ModSource::Keyb, int8_t(fmAmount)});
    }
    void note(int key) {
        sq8l::RawMidiEvent e{};
        e.data[0] = 0x90;
        e.data[1] = uint8_t(key);
        e.data[2] = 100;
        synth.processEvents(&e, 1);
    }
    // hostPosition: the transport plays and the host reports where; else the engine runs on
    void run(int blocks, double bpm, bool hostPosition) {
        std::vector<float> l(512), r(512);
        for (int b = 0; b < blocks; b++) {
            synth.master().setTransport(bpm, hostPosition, quarters);
            synth.process(l.data(), r.data(), 512, true);
            quarters += 512 * bpm / 60 / 44100;
        }
    }
    const sq8l::Lfo* lfo(int key) {
        for (int i = 0; i < sq8l::Master::kMaxVoices; i++) {
            const sq8l::Voice& v = synth.master().voice(i);
            if (v.active != 0 && v.key == key) return &synth.modules().lfo[v.slot][0];
        }
        return nullptr;
    }
};

// LFO tempo sync (LFO -> SYNC, port addition, issue #42): one cycle per division at the host's
// tempo, FREQ above the last division plays the last, FM steps through the divisions, the
// rate stops at the free LFO's top, and with RESET=OFF the phase follows the song position,
// the host's or, with the transport stopped, the one the engine runs on. Needs no files.
int tempoSync() {
    // An LFO tick: two control updates of Round(sr / 83.592575 * 512) / 1024 samples each.
    const double tick = 2 * std::nearbyint(double(float(44100.0 / double(83.592575f) * 512.0))) / 1024 / 44100;
    const double cycle = std::ldexp(1.0, 30);
    auto expectedInc = [&](double bpm, double quarters) {
        return std::min(cycle * bpm / 60 * tick / quarters, std::ldexp(1.0, 29));
    };
    int bad = 0;
    auto checkInc = [&](const char* what, const sq8l::Lfo* l, double want) {
        const double got = l ? l->phaseInc : 0;
        if (!l || std::fabs(got - want) > 1 + 1e-5 * want) {
            bad++;
            std::printf("BAD %s: phase increment %.0f, want %.0f\n", what, got, want);
        }
    };

    // index into kSyncQuarters, bpm
    const struct { int index; double bpm; const char* name; } rates[] = {
        {22, 120, "1/4 at 120"}, {18, 93, "1T at 93"}, {14, 140, "2 bars at 140"},
        {0, 60, "16 bars at 60"}, {33, 175, "32T at 175"}, {26, 87.5, "16D at 87.5"}};
    for (const auto& c : rates) {
        SyncRun s(c.index, 0);
        s.note(60);
        s.run(20, c.bpm, false);
        checkInc(c.name, s.lfo(60), expectedInc(c.bpm, sq8l::kSyncQuarters[c.index]));
    }
    {
        SyncRun s(100, 0);  // a free rate left in the byte
        s.note(60);
        s.run(20, 60, false);
        checkInc("FREQ 100 plays 64T", s.lfo(60), expectedInc(60, 1.0 / 24));
        SyncRun t(35, 0);
        t.note(60);
        t.run(20, 200, false);
        checkInc("64T at 200 stops at half a cycle per tick", t.lfo(60), std::ldexp(1.0, 29));
    }
    // KEYB (key 60) * amount 2 * 2 = 240 / 256, rounded: one division either way
    for (int amount : {2, -2}) {
        SyncRun s(22, 0, amount);
        s.note(60);
        s.run(20, 120, false);
        checkInc(amount > 0 ? "FM +1 division (8D)" : "FM -1 division (2T)", s.lfo(60),
                 expectedInc(120, sq8l::kSyncQuarters[22 + amount / 2]));
    }
    std::printf("rate: %zu divisions and tempos, FREQ past the last, the cap, FM both ways (%s)\n",
                std::size(rates), bad ? "FAILED" : "ok");

    // Two notes 7 blocks apart, RESET=OFF: both at the phase of the song position, which is the
    // phase one cycle per quarter has at the end; the last tick may be up to one tick back.
    int lockBad = 0;
    auto lockRun = [&](const char* what, bool hostPosition, double start, bool reverse) {
        SyncRun s(22, 0xff, 0, reverse);
        s.quarters = start;
        s.note(60);
        s.run(7, 120, hostPosition);
        s.note(64);
        s.run(30, 120, hostPosition);
        const double frac = s.quarters - std::floor(s.quarters);
        const uint32_t want = uint32_t(frac * cycle) * (reverse ? uint32_t(-1) : 1u);
        const double slack = 2 * expectedInc(120, 1) + 1;
        for (int key : {60, 64}) {
            const sq8l::Lfo* l = s.lfo(key);
            const uint32_t d = l ? (l->phase - want) & 0x3fffffffu : 0x20000000u;
            const double off = std::min<double>(d, cycle - d);
            if (!l || l->syncLock != 1 || off > slack) {
                lockBad++;
                std::printf("BAD %s, key %d: %.0f from the song position's phase (allowed %.0f)\n", what, key, off,
                            slack);
            }
        }
    };
    lockRun("host position", true, 17.3, false);
    lockRun("transport stopped", false, 0, false);
    lockRun("host position, REV", true, 5.6, true);
    std::printf("lock: notes started apart share the song position's phase: from the host, stopped, "
                "reversed (%s)\n", lockBad ? "FAILED" : "ok");

    // Envelopes: ENV1 of INIT with SYNC on (ENV4 for the release: the voice ends with ENV4,
    // INIT's in a tick, and ENV1 stops there); how long a segment really lasts, in samples, from
    // the end of the previous one (or from note on / off) to the end of its own. Measured in
    // blocks of 32, against the note value at the tempo. Slack: a block and two ticks, one for
    // rounding to whole ticks and one because a segment's first tick is the one it starts on.
    int envBad = 0;
    struct EnvCase {
        const char* what;
        int t1, t2, t4, t1v, tk, key;
        double bpm;
        sq8l::Env::Next stageEnd;  // the segment measured ends when `next` leaves this
        double quarters;
    };
    const EnvCase envCases[] = {
        {"T1 13 = 1/4 at 120", 13, 0, 0, 0, 0, 60, 120, sq8l::Env::Next::Decay, 1},
        {"T1V ignored", 13, 0, 0, 63, 0, 60, 120, sq8l::Env::Next::Decay, 1},
        {"T1 19 = 1 bar at 90", 19, 0, 0, 0, 0, 60, 90, sq8l::Env::Next::Decay, 4},
        {"T1 50 plays 16X", 50, 0, 0, 0, 0, 60, 999, sq8l::Env::Next::Decay, 64},
        {"T2 7 = 1/16, TK ignored", 0, 7, 0, 0, 63, 96, 120, sq8l::Env::Next::Decay2, 0.25},
        {"T4 16 = 1/2 at 120", 0, 0, 16, 0, 0, 60, 120, sq8l::Env::Next::Finish, 2},
    };
    for (const EnvCase& c : envCases) {
        sq8l::Synth synth(44100.0f);
        synth.setSampleRate(44100.0f);
        synth.setProgram(0);
        sq8l::Program& p = synth.editBuffer().current();
        sq8l::initProgram(p);
        const int index = c.stageEnd == sq8l::Env::Next::Finish ? 3 : 0;
        uint8_t* e = p.bytes + sq8l::ofs::env(index);
        e[sq8l::ofs::Env::T1] = uint8_t(c.t1);
        e[sq8l::ofs::Env::T2] = uint8_t(c.t2);
        e[sq8l::ofs::Env::T4] = uint8_t(c.t4);
        e[sq8l::ofs::Env::T1V] = uint8_t(c.t1v);
        e[sq8l::ofs::Env::TK] = uint8_t(c.tk);
        e[sq8l::ofs::Env::Flags] = sq8l::ofs::Env::SyncBit;
        sq8l::RawMidiEvent ev{};
        ev.data[0] = 0x90;
        ev.data[1] = uint8_t(c.key);
        ev.data[2] = 127;
        synth.processEvents(&ev, 1);
        const sq8l::Env* env = nullptr;
        std::vector<float> l(32), r(32);
        long start = 0, end = -1;
        bool released = false;
        for (long s = 0; s < 44100L * 10 && end < 0; s += 32) {
            synth.master().setTransport(c.bpm, false, 0);
            synth.process(l.data(), r.data(), 32, true);
            for (int i = 0; !env && i < sq8l::Master::kMaxVoices; i++)
                if (synth.master().voice(i).active != 0) env = &synth.modules().env[synth.master().voice(i).slot][index];
            if (!env) continue;
            if (c.stageEnd == sq8l::Env::Next::Finish) {
                if (!released && env->next == sq8l::Env::Next::Sustain && env->sustaining) {
                    ev.data[2] = 0;  // note off once the envelope sustains
                    synth.processEvents(&ev, 1);
                    released = true;
                    start = s + 32;
                } else if (released && !env->active) {  // `finished` lasts only within its tick
                    end = s + 32;
                }
            } else if (c.stageEnd == sq8l::Env::Next::Decay2 && env->next != sq8l::Env::Next::Decay && start == 0) {
                start = s + 32;  // T1 is over, T2 runs
            } else if (env->next != c.stageEnd && (c.stageEnd == sq8l::Env::Next::Decay || start > 0)) {
                end = s + 32;
            }
        }
        const double want = c.quarters * 60 / c.bpm * 44100, slack = 2 * tick * 44100 + 32;
        if (end < 0 || std::fabs((end - start) - want) > slack) {
            envBad++;
            std::printf("BAD %s: %ld samples, want %.0f (allowed %.0f)\n", c.what, end < 0 ? -1 : end - start, want,
                        slack);
        }
    }
    std::printf("envelopes: %zu segments last their note value: T1, T1V ignored, past the last, T2 with TK "
                "ignored, T4 (%s)\n", std::size(envCases), envBad ? "FAILED" : "ok");
    return bad || lockBad || envBad ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "--regression") return regression(argv[2]);
    if (argc > 2 && std::string(argv[1]) == "--polyphony") return polyphony(argv[2]);
    if (argc > 1 && std::string(argv[1]) == "--direction") return direction();
    if (argc > 2 && std::string(argv[1]) == "--output-gain") return outputGain(argv[2]);
    if (argc > 1 && std::string(argv[1]) == "--tempo-sync") return tempoSync();
    return golden(argc > 1 ? argv[1] : "tests/golden_raw");
}
