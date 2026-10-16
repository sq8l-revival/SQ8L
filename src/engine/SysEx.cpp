// SQ-80 / ESQ-1 SysEx conversion, transcribed from the original (unit_453adc, unit_450db8).
// Byte-level code: every helper reproduces the 8-bit register arithmetic of the asm.
#include "SysEx.h"

#include <cstring>

namespace sq8l::sysex {

namespace {

inline uint8_t mask8(int bits) { return static_cast<uint8_t>((1u << bits) - 1u); }  // bits 1..8

// FUN_00451210: unsigned field from two nybbles (low first).
inline uint8_t nyb(uint8_t lo, uint8_t hi, int bits) {
    return static_cast<uint8_t>((lo | (hi << 4)) & mask8(bits));
}

// FUN_0045122c: signed (sign-extended) field from two nybbles.
inline uint8_t snyb(uint8_t lo, uint8_t hi, int bits) {
    const uint8_t m = mask8(bits);
    uint8_t v = static_cast<uint8_t>((lo | (hi << 4)) & m);
    if (bits < 8) {
        const uint8_t ext = (v >> (bits - 1)) ? 0xff : 0x00;
        v = static_cast<uint8_t>((ext & static_cast<uint8_t>(~m)) | v);
    }
    return v;
}

// FUN_00451268: unsigned field -> two nybbles.
inline void pack(uint8_t v, int bits, uint8_t* lo, uint8_t* hi) {
    v &= mask8(bits);
    *lo = v & 0x0f;
    *hi = v >> 4;
}

// FUN_00451294: signed field -> two nybbles (sign moved to bit bits-1).
inline void spack(uint8_t v, int bits, uint8_t* lo, uint8_t* hi) {
    const uint8_t sign = static_cast<uint8_t>((v & 0x80u) >> (8 - bits));
    v = static_cast<uint8_t>((v & mask8(bits - 1)) | sign);
    *lo = v & 0x0f;
    *hi = v >> 4;
}

// Delphi "x div 2" on a signed byte (sar + adc: rounds toward zero).
inline uint8_t half(uint8_t v) { return static_cast<uint8_t>(static_cast<int8_t>(v) / 2); }

inline int clampS63(int v) {  // FUN_00450db8
    if (v >= 63) return 63;
    if (v <= -63) return -63;
    return v;
}
inline int clampU(int v, int hi) {  // FUN_00450da4 / 0450dd0 / 0450de4 / 0450df8
    if (v <= 0) return 0;
    return v < hi ? v : hi;
}

// FUN_00453ab4: SQ-80 pan nybble 0..15 -> -63..+63.
uint8_t panFromSq80(uint8_t n) {
    const int v = clampU(n, 15);
    if (v == 0) return static_cast<uint8_t>(-63);
    if (v == 15) return 63;
    return static_cast<uint8_t>((v - 8) * 8);
}

// FUN_00453ae8: pan -63..+63 -> SQ-80 nybble.
uint8_t panToSq80(int v) {
    v = clampS63(v);
    if (v <= -63) return 0;
    return static_cast<uint8_t>(clampU(v / 8 + 8, 15));
}

void setSource(uint8_t* p, uint8_t sq80) {
    const int16_t s = sourceFromSq80(sq80);
    p[0] = static_cast<uint8_t>(s);
    p[1] = static_cast<uint8_t>(static_cast<uint16_t>(s) >> 8);
}

uint8_t getSource(const uint8_t* p) {
    return sourceToSq80(static_cast<int16_t>(p[0] | (p[1] << 8)));
}

// FUN_00450e3c: one SQ-80 character -> text.
void appendSq80Char(std::string& out, uint8_t c) {
    switch (c) {
        case 0x21: out += "0."; return;
        case 0x23: out += "1."; return;
        case 0x25: out += "2."; return;
        case 0x28: out += "3."; return;
        case 0x29: out += "4."; return;
        case 0x3a: out += "5."; return;
        case 0x3b: out += "6."; return;
        case 0x5b: out += "7."; return;
        case 0x5c: out += "8."; return;
        case 0x5d: out += "9."; return;
        default:
            if (c >= 0x20 && c <= 0x5f) out += static_cast<char>(c);
            return;
    }
}

// FUN_00450fd4: parse one character from up to 2 chars of text.
uint8_t parseSq80Char(const char* s, size_t len, int* consumed) {
    uint8_t res = 0x20;
    *consumed = 0;
    if (len == 0) return res;
    uint8_t c = static_cast<uint8_t>(s[0]);
    if (c >= 'a' && c <= 'z') c = static_cast<uint8_t>(c - 0x20);
    size_t n = len;
    if (n > 1 && s[1] != '.') n = 1;
    *consumed = static_cast<int>(n);
    if (n == 1) {
        if (c >= 0x20 && c <= 0x5f) res = c;
        return res;
    }
    static const uint8_t kDigits[10] = {0x21, 0x23, 0x25, 0x28, 0x29, 0x3a, 0x3b, 0x5b, 0x5c, 0x5d};
    if (c >= '0' && c <= '9') return kDigits[c - '0'];
    *consumed = 1;
    if (c >= 0x20 && c <= 0x5f) res = c;
    return res;
}

void setNameFromChars(Program& p, const uint8_t* chars) {
    p.setName(nameFromSq80(chars, 6));
}

}  // namespace

bool parseHeader(const uint8_t* data, int* type) {
    if (!data || data[0] != 0xf0 || data[1] != 0x0f || data[2] != 0x02) return false;
    if (type) *type = data[4];
    return true;
}

bool writeHeader(uint8_t* data, uint8_t type, uint8_t channel) {
    if (!data || (type != kTypeSingle && type != kTypeBank)) return false;
    data[0] = 0xf0;
    data[1] = 0x0f;
    data[2] = 0x02;
    data[3] = channel;
    data[4] = type;
    return true;
}

int16_t sourceFromSq80(uint8_t s) {
    switch (s) {
        case 0: case 1: case 2: return s;                         // LFO1-3
        case 3: case 4: case 5: case 6: return int16_t(s + 1);    // ENV1-4
        case 7: return 8;                                         // VEL
        case 8: return 9;                                         // VEL2 -> VEL-X
        case 9: return 10;                                        // KYBD
        case 10: return 11;                                       // KYBD2
        case 11: return 0x11;                                     // WHEEL -> CC1
        case 12: return 0x34;                                     // PEDAL -> CC36 (as in the original)
        case 13: return 0x12;                                     // XCTRL -> CC2 (breath)
        case 14: return 0x0c;                                     // PRESS
        default: return -1;                                       // OFF
    }
}

uint8_t sourceToSq80(int16_t src) {
    const int s = src;
    if (s > 0x0b) {
        if (s > 0x12) {
            if (s == 0x34) return 0x0c;
            if (s == 0x90) return 0x0e;
            return 0x0f;
        }
        if (s == 0x12) return 0x0d;
        if (s == 0x0c) return 0x0e;
        if (s == 0x11) return 0x0b;
        return 0x0f;
    }
    if (s == 0x0b) return 0x0a;
    if (s < 0 || s > 0x0a) return 0x0f;
    switch (s) {
        case 0: case 1: case 2: return uint8_t(s);
        case 3: return 0x0f;  // LFO4 has no SQ-80 equivalent
        case 4: case 5: case 6: case 7: return uint8_t(s - 1);
        case 8: return 7;
        case 9: return 8;
        default: return 9;    // 10
    }
}

std::string nameFromSq80(const uint8_t* chars, int n) {
    std::string out;
    for (int i = 0; i < n; i++) appendSq80Char(out, chars[i]);
    return out;
}

std::vector<uint8_t> nameToSq80(std::string_view s, int n) {
    std::vector<uint8_t> buf(n > 0 ? size_t(n) : 0, 0);
    if (n <= 0) return buf;
    const size_t len = s.size();
    size_t i = 0;
    int k = 0;
    while (i < len && k < n) {
        const size_t avail = len - i < 2 ? len - i : 2;  // Copy(s, i + 1, 2)
        int consumed = 0;
        const uint8_t c = parseSq80Char(s.data() + i, avail, &consumed);
        if (c != 0x20) buf[size_t(k++)] = c;
        i += size_t(consumed);
    }
    return buf;
}

void programFromNybbles(const uint8_t* src, Program& prog) {
    uint8_t* p = prog.bytes;
    initProgram(prog);
    // (The original calls a bit-field setter on +0x192 here and discards its result.)
    {
        uint8_t chars[6];
        for (int j = 0; j < 6; j++) chars[j] = nyb(src[2 * j], src[2 * j + 1], 8);
        setNameFromChars(prog, chars);
    }
    for (int e = 0; e < 4; e++) {
        const uint8_t* b = src + 0x0c + 0x14 * e;
        uint8_t* d = p + ofs::env(e);
        d[0] = 0;
        for (int j = 0; j < 3; j++) d[1 + j] = half(snyb(b[2 * j], b[2 * j + 1], 8));
        for (int j = 0; j < 4; j++) d[7 + j] = nyb(b[6 + 2 * j], b[7 + 2 * j], 6);
        d[0x0a] = static_cast<uint8_t>(d[0x0a] + ((b[0x0d] >> 3) << 6));
        d[5] = static_cast<uint8_t>(nyb(b[0x0e], b[0x0f], 8) >> 2);
        d[6] = nyb(b[0x10], b[0x11], 6);
        d[0x0b] = nyb(b[0x12], b[0x13], 6);
    }
    for (int l = 0; l < 3; l++) {
        const uint8_t* b = src + 0x5c + 8 * l;
        uint8_t* d = p + ofs::lfo(l);
        const uint8_t w = nyb(b[0], b[1], 8);
        d[0] = w & 0x3f;
        d[3] = w >> 6;
        const uint8_t x = nyb(b[2], b[3], 8);
        const uint8_t y = nyb(b[4], b[5], 8);
        d[4] = x & 0x3f;
        d[6] = y & 0x3f;
        setSource(d + 7, static_cast<uint8_t>((y >> 6) + ((x & 0xc0) >> 4)));
        d[9] = 0x3f;
        const uint8_t z = nyb(b[6], b[7], 8);
        d[5] = z & 0x3f;
        d[1] = static_cast<uint8_t>((z >> 7) - 1);
        d[2] = (z >> 6) & 1;
        d[0x0d] = 0;
    }
    for (int o = 0; o < 3; o++) {
        const uint8_t* b = src + 0x74 + 0x14 * o;
        uint8_t* d = p + ofs::osc(o);
        const uint8_t v = nyb(b[0], b[1], 7);
        d[0] = static_cast<uint8_t>(v / 12 - 3);
        d[1] = static_cast<uint8_t>(v % 12);
        d[2] = static_cast<uint8_t>(nyb(b[2], b[3], 8) >> 3);
        setSource(d + 5, b[4]);
        setSource(d + 8, b[5]);
        d[7] = half(snyb(b[6], b[7], 8));
        d[0x0a] = half(snyb(b[8], b[9], 8));
        d[3] = nyb(b[0x0a], b[0x0b], 8);
        d[4] = 0;
        const uint8_t w = nyb(b[0x0c], b[0x0d], 8);
        d[0x0e] = static_cast<uint8_t>((w & 0x7f) >> 1);
        d[0x0f] = w >> 7;
        setSource(d + 0x10, b[0x0e]);
        setSource(d + 0x13, b[0x0f]);
        d[0x12] = half(snyb(b[0x10], b[0x11], 8));
        d[0x15] = half(snyb(b[0x12], b[0x13], 8));
    }
    {
        const uint8_t* b = src + 0xb0;
        uint8_t* m = p + ofs::Sync;
        m[1] = b[0x01] >> 3;
        m[0] = b[0x03] >> 3;
        m[4] = b[0x09] >> 3;
        m[2] = b[0x0b] >> 3;
        m[5] = b[0x0d] >> 3;
        m[6] = b[0x0f] >> 3;
        m[7] = b[0x1b] >> 3;
        m[3] = static_cast<uint8_t>(((b[0x0f] << 4) + b[0x0e]) & 0x3f);
        uint8_t* a = p + ofs::Dca4Level;
        a[0] = static_cast<uint8_t>(nyb(b[0], b[1], 7) >> 1);
        a[1] = panFromSq80(b[0x19]);
        setSource(a + 5, b[0x18]);
        a[7] = snyb(b[0x1a], b[0x1b], 7);
        uint8_t* f = p + ofs::filter(0);
        f[0] = nyb(b[2], b[3], 7);
        f[1] = nyb(b[4], b[5], 5);
        setSource(f + 4, b[6]);
        setSource(f + 7, b[7]);
        f[6] = snyb(b[8], b[9], 7);
        f[9] = snyb(b[0x0a], b[0x0b], 7);
        f[2] = static_cast<uint8_t>(nyb(b[0x0c], b[0x0d], 7) >> 1);
    }
    if (static_cast<int8_t>(p[ofs::AmMod]) > 0) {
        p[ofs::osc(0) + ofs::Osc::Enable] = 0;
        p[ofs::osc(0) + ofs::Osc::Level] = 0x3f;
        p[ofs::osc(1) + ofs::Osc::Enable] = 1;
        p[ofs::osc(1) + ofs::Osc::Level] = 0x3f;
    }
}

void programToNybbles(const Program& prog, uint8_t* dst, bool esq1) {
    const uint8_t* p = prog.bytes;
    std::memset(dst, 0, kProgramNybbles);
    {
        const std::vector<uint8_t> chars = nameToSq80(prog.name(), 6);
        for (int j = 0; j < 6; j++) pack(chars[size_t(j)], 8, &dst[2 * j], &dst[2 * j + 1]);
    }
    for (int e = 0; e < 4; e++) {
        uint8_t* b = dst + 0x0c + 0x14 * e;
        const uint8_t* d = p + ofs::env(e);
        for (int j = 0; j < 3; j++)
            spack(static_cast<uint8_t>(d[1 + j] * 2), 8, &b[2 * j], &b[2 * j + 1]);
        for (int j = 0; j < 4; j++) pack(d[7 + j], 6, &b[6 + 2 * j], &b[7 + 2 * j]);
        const uint32_t t4 = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(d[0x0a])));
        b[0x0d] = static_cast<uint8_t>(b[0x0d] + static_cast<uint8_t>((t4 >> 6) << 3));
        pack(static_cast<uint8_t>(d[5] << 2), 8, &b[0x0e], &b[0x0f]);
        pack(d[6], 6, &b[0x10], &b[0x11]);
        pack(d[0x0b], 6, &b[0x12], &b[0x13]);
    }
    for (int l = 0; l < 3; l++) {
        uint8_t* b = dst + 0x5c + 8 * l;
        const uint8_t* d = p + ofs::lfo(l);
        pack(static_cast<uint8_t>(clampU(static_cast<int8_t>(d[0] & 0x7f), 0x3f)), 6, &b[0], &b[1]);
        pack(d[4], 6, &b[2], &b[3]);
        pack(d[6], 6, &b[4], &b[5]);
        pack(d[5] & 0x3f, 6, &b[6], &b[7]);
        b[1] |= static_cast<uint8_t>((d[3] & 3) << 2);
        const uint8_t s = getSource(d + 7);
        b[3] |= s & 0x0c;
        b[5] |= static_cast<uint8_t>((s & 3) << 2);
        const uint8_t reset = static_cast<int8_t>(d[1]) >= 0 ? 1 : 0;
        const uint8_t human = static_cast<int8_t>(d[2]) > 0 ? 1 : 0;
        b[7] = static_cast<uint8_t>((reset << 3) | b[7] | (human << 2));
    }
    for (int o = 0; o < 3; o++) {
        uint8_t* b = dst + 0x74 + 0x14 * o;
        const uint8_t* d = p + ofs::osc(o);
        int v = (static_cast<int8_t>(d[0]) + 3) * 12 + static_cast<int8_t>(d[1]);
        int f = static_cast<int8_t>(d[2]);
        if (f < 0) {
            v--;
            f += 0x20;
            if (v < 0) v = 0;
        }
        pack(static_cast<uint8_t>(v), 7, &b[0], &b[1]);
        pack(static_cast<uint8_t>(f << 3), 8, &b[2], &b[3]);
        b[4] = getSource(d + 5);
        b[5] = getSource(d + 8);
        spack(static_cast<uint8_t>(d[7] * 2), 8, &b[6], &b[7]);
        spack(static_cast<uint8_t>(d[0x0a] * 2), 8, &b[8], &b[9]);
        int w = static_cast<int16_t>(d[3] | (d[4] << 8));
        if (esq1 && w >= 0x20) w = 0x1f;
        pack(static_cast<uint8_t>(w), 8, &b[0x0a], &b[0x0b]);
        pack(static_cast<uint8_t>(d[0x0e] * 2), 7, &b[0x0c], &b[0x0d]);
        b[0x0d] |= static_cast<uint8_t>(d[0x0f] << 3);
        b[0x0e] = getSource(d + 0x10);
        b[0x0f] = getSource(d + 0x13);
        spack(static_cast<uint8_t>(d[0x12] * 2), 8, &b[0x10], &b[0x11]);
        spack(static_cast<uint8_t>(d[0x15] * 2), 8, &b[0x12], &b[0x13]);
    }
    {
        uint8_t* b = dst + 0xb0;
        const uint8_t* a = p + ofs::Dca4Level;
        pack(static_cast<uint8_t>(a[0] * 2), 7, &b[0], &b[1]);
        b[0x19] = panToSq80(static_cast<int8_t>(a[1]));
        b[0x18] = getSource(a + 5);
        spack(static_cast<uint8_t>(clampS63(static_cast<int8_t>(a[7]))), 7, &b[0x1a], &b[0x1b]);
        const uint8_t* f = p + ofs::filter(0);
        pack(f[0], 7, &b[2], &b[3]);
        pack(static_cast<uint8_t>(clampU(static_cast<int8_t>(f[1]), 31)), 5, &b[4], &b[5]);
        b[6] = getSource(f + 4);
        b[7] = getSource(f + 7);
        spack(static_cast<uint8_t>(clampS63(static_cast<int8_t>(f[6]))), 7, &b[8], &b[9]);
        spack(static_cast<uint8_t>(clampS63(static_cast<int8_t>(f[9]))), 7, &b[0x0a], &b[0x0b]);
        pack(static_cast<uint8_t>(clampU(static_cast<int8_t>(f[2]), 63) * 2), 7, &b[0x0c], &b[0x0d]);
        const uint8_t* m = p + ofs::Sync;
        int am = static_cast<int8_t>(m[1]);
        if (am > 1) am = 1;
        const int sync = static_cast<int8_t>(m[0]);
        if (am > 0 && sync > 0) am = 0;
        b[1] |= static_cast<uint8_t>(am << 3);
        b[3] |= static_cast<uint8_t>(sync << 3);
        b[0x09] |= static_cast<uint8_t>(m[4] << 3);
        b[0x0b] |= static_cast<uint8_t>(m[2] << 3);
        b[0x0d] |= static_cast<uint8_t>(m[5] << 3);
        b[0x1b] |= static_cast<uint8_t>(m[7] << 3);
        pack(m[3], 6, &b[0x0e], &b[0x0f]);
        b[0x0f] |= static_cast<uint8_t>(m[6] << 3);
    }
}

void programFromSq80Bytes(const uint8_t* src, Program& prog, uint8_t emuFlags, uint8_t emuFlags3,
                          uint8_t emuFlags2, int8_t levelOffset) {
    uint8_t* p = prog.bytes;
    initProgram(prog);
    p[ofs::EmuFlags] = emuFlags;
    p[ofs::EmuFlags3] = emuFlags3;
    p[ofs::EmuFlags2] = emuFlags2;
    setNameFromChars(prog, src);
    for (int e = 0; e < 4; e++) {
        const uint8_t* b = src + 6 + 10 * e;
        uint8_t* d = p + ofs::env(e);
        d[0] = 0;
        for (int j = 0; j < 3; j++) d[1 + j] = half(b[j]);
        for (int j = 0; j < 4; j++) d[7 + j] = b[3 + j] & 0x3f;
        d[0x0a] = static_cast<uint8_t>(d[0x0a] + ((b[6] & 0x80) >> 1));
        d[5] = b[7] >> 2;
        d[6] = b[8] & 0x3f;
        d[0x0b] = b[9] & 0x3f;
    }
    for (int l = 0; l < 3; l++) {
        const uint8_t* b = src + 0x2e + 4 * l;
        uint8_t* d = p + ofs::lfo(l);
        d[0] = b[0] & 0x3f;
        d[3] = b[0] >> 6;
        d[4] = b[1] & 0x3f;
        d[6] = b[2] & 0x3f;
        setSource(d + 7, static_cast<uint8_t>((b[2] >> 6) + ((b[1] & 0xc0) >> 4)));
        d[9] = 0x3f;
        d[5] = b[3] & 0x3f;
        d[1] = static_cast<uint8_t>((b[3] >> 7) - 1);
        d[2] = (b[3] >> 6) & 1;
        d[0x0d] = 0;
    }
    for (int o = 0; o < 3; o++) {
        const uint8_t* b = src + 0x3a + 10 * o;
        uint8_t* d = p + ofs::osc(o);
        const uint8_t v = b[0] & 0x7f;
        d[0] = static_cast<uint8_t>(v / 12 - 3);
        d[1] = static_cast<uint8_t>(v % 12);
        d[2] = b[1] >> 3;
        setSource(d + 5, b[2] & 0x0f);
        setSource(d + 8, b[2] >> 4);
        d[7] = half(b[3]);
        d[0x0a] = half(b[4]);
        d[3] = b[5];
        d[4] = 0;
        d[0x0e] = static_cast<uint8_t>((b[6] & 0x7f) >> 1);
        d[0x0f] = b[6] >> 7;
        setSource(d + 0x10, b[7] & 0x0f);
        setSource(d + 0x13, b[7] >> 4);
        d[0x12] = half(b[8]);
        d[0x15] = half(b[9]);
    }
    {
        const uint8_t* b = src + 0x58;
        uint8_t* m = p + ofs::Sync;
        m[1] = b[0] >> 7;
        m[0] = b[1] >> 7;
        m[4] = b[4] >> 7;
        m[2] = b[5] >> 7;
        m[5] = b[6] >> 7;
        m[6] = b[7] >> 7;
        m[7] = b[0x0d] >> 7;
        m[3] = b[7] & 0x3f;
        uint8_t* a = p + ofs::Dca4Level;
        a[0] = static_cast<uint8_t>(clampU(((b[0] & 0x7f) >> 1) + levelOffset, 63));
        a[1] = panFromSq80(b[0x0c] >> 4);
        setSource(a + 5, b[0x0c] & 0x0f);
        a[7] = b[0x0d] & 0x7f;
        uint8_t* f = p + ofs::filter(0);
        f[0] = b[1] & 0x7f;
        f[1] = b[2] & 0x1f;
        setSource(f + 4, b[3] & 0x0f);
        setSource(f + 7, b[3] >> 4);
        f[6] = b[4] & 0x7f;
        f[9] = b[5] & 0x7f;
        f[2] = static_cast<uint8_t>((b[6] & 0x7f) >> 1);
    }
    if (static_cast<int8_t>(p[ofs::AmMod]) > 0) {
        p[ofs::osc(0) + ofs::Osc::Enable] = 0;
        p[ofs::osc(0) + ofs::Osc::Level] = 0x3f;
        p[ofs::osc(1) + ofs::Osc::Enable] = 1;
        p[ofs::osc(1) + ofs::Osc::Level] = 0x3f;
    }
}

std::vector<uint8_t> makeSingleDump(const Program& p) {
    std::vector<uint8_t> out(kSingleDumpSize);
    writeHeader(out.data(), kTypeSingle, 0);
    programToNybbles(p, out.data() + kHeaderSize, false);
    out[kSingleDumpSize - 1] = 0xf7;
    return out;
}

}  // namespace sq8l::sysex
