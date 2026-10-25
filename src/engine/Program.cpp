// Program record helpers (original units unit_4537a4 / unit_453adc).
#include "Program.h"

#include <cstring>

namespace sq8l {

const double kSyncQuarters[kSyncDivisions] = {
    64, 60, 56, 52, 48, 44, 40, 36, 32, 28, 24, 20, 16, 12, 8,   // 16X .. 2X (bars)
    6, 4, 3, 8.0 / 3, 2, 1.5, 4.0 / 3, 1,                        // 1D 1 2D 1T 2 4D 2T 4
    0.75, 2.0 / 3, 0.5, 0.375, 1.0 / 3, 0.25,                    // 8D 4T 8 16D 8T 16
    0.1875, 1.0 / 6, 0.125, 0.09375, 1.0 / 12, 0.0625, 1.0 / 24  // 32D 16T 32 64D 32T 64 64T
};

std::string Program::shortString(size_t off) const {
    // FUN_00403cb4 (ShortString -> AnsiString): the full length byte is honoured.
    size_t n = bytes[off];
    if (off + 1 + n > kProgramSize) n = kProgramSize - off - 1;
    return std::string(reinterpret_cast<const char*>(bytes + off + 1), n);
}

void Program::setShortString(size_t off, std::string_view s) {
    // Delphi: ShortString[255] := s (FUN_00403cec), then FUN_0040291c(dst, tmp, 15).
    size_t n = s.size() > 255 ? 255 : s.size();
    if (n > 15) n = 15;
    bytes[off] = static_cast<uint8_t>(n);
    std::memcpy(bytes + off + 1, s.data(), n);
}

void clearModSlots(uint8_t* p, int count) {
    // FUN_0045377c: word 0xffff at the start of each 3-byte slot.
    for (int i = 0; i < count; i++) {
        p[3 * i] = 0xff;
        p[3 * i + 1] = 0xff;
    }
}

namespace {

// FUN_00453798: one 0x32-byte block of the extension area.
void initExtBlock(uint8_t* b) {
    std::memset(b, 0, 0x32);
    b[1] = 0xff;
    clearModSlots(b + 0x17, 9);
}

// FUN_00453814: parameter body and header.
void initBody(uint8_t* p) {
    std::memset(p, 0, 0x1b0);
    for (int i = 0; i < 3; i++) {
        uint8_t* o = p + ofs::osc(i);
        o[ofs::Osc::Level] = 0x32;
        if (i == 0) o[ofs::Osc::Enable] = 1;
        clearModSlots(o + ofs::Osc::PitchMod1, 2);
        clearModSlots(o + ofs::Osc::AmpMod1, 2);
    }
    for (int i = 0; i < 4; i++) {
        uint8_t* e = p + ofs::env(i);
        e[ofs::Env::L1] = 0x3f;
        e[ofs::Env::L2] = 0x3f;
        e[ofs::Env::L3] = 0x3f;
    }
    for (int i = 0; i < 4; i++) {
        uint8_t* l = p + ofs::lfo(i);
        l[ofs::Lfo::Freq] = 0x18;
        l[ofs::Lfo::Reset] = 0xff;
        l[ofs::Lfo::L1] = 0;
        l[ofs::Lfo::Delay] = 0x3f;
        l[ofs::Lfo::L2] = 0x3f;
        clearModSlots(l + ofs::Lfo::Mod, 1);
        clearModSlots(l + ofs::Lfo::FreqMod, 1);
        l[ofs::Lfo::Modes] = 0;
    }
    for (int i = 0; i < 3; i++) clearModSlots(p + ofs::matrix(i), 4);
    for (int i = 0; i < 2; i++) {
        uint8_t* f = p + ofs::filter(i);
        f[ofs::Filter::Freq] = 0x7f;
        clearModSlots(f + ofs::Filter::Mod1, 2);
        clearModSlots(f + ofs::Filter::Mod3, 1);
    }
    clearModSlots(p + 0x124, 2);
    p[ofs::Dca4Level] = 0x3f;
    clearModSlots(p + ofs::AmpMod, 1);
    clearModSlots(p + ofs::PanMod, 1);
    for (int i = 0; i < 3; i++) p[0x14a + 0x10 * i] = 1;
    p[0x18e] = 0x3c;
    p[0x182] = 4;
    p[0x183] = 0x10;
    p[ofs::BendRange] = 2;
    p[ofs::EmuFlags] = 1;
    p[ofs::EmuFlags3] = 0;
    p[0x1a0] = 0x78;
    p[0x1a1] = 2;
    p[0x1a3] = 0x20;
    // FUN_004537f0(p + 0x1a6)
    std::memset(p + 0x1a6, 0, 10);
    clearModSlots(p + 0x1a8, 2);
    p[0] = 2;
    p[1] = 0;
    static const uint8_t kInit[5] = {4, 'I', 'N', 'I', 'T'};
    std::memcpy(p + ofs::Name, kInit, 5);
}

}  // namespace

void initProgram(Program& prog) {
    uint8_t* p = prog.bytes;
    initBody(p);
    // FUN_004537c0(p + 0x1b0)
    uint8_t* ext = p + ofs::Ext;
    std::memset(ext, 0, ofs::ExtSize);
    for (int i = 0; i < 2; i++) initExtBlock(ext + 8 + 0x32 * i);
}

Program makeInitProgram() {
    Program p;
    initProgram(p);
    return p;
}

namespace {

// FUN_00453c24: n slots of the old format (4 bytes: int16 source, int8 amount, pad) -> 3 bytes.
void copyOldSlots(const uint8_t* s, uint8_t* d, int n) {
    for (int i = 0; i < n; i++, s += 4, d += 3) {
        d[0] = s[0];
        d[1] = s[1];
        d[2] = s[2];
    }
}

// FUN_00453c44: same, amount doubled (range -63..63 -> -127..127).
void copyOldSlots2(const uint8_t* s, uint8_t* d, int n) {
    for (int i = 0; i < n; i++, s += 4, d += 3) {
        d[0] = s[0];
        d[1] = s[1];
        d[2] = static_cast<uint8_t>(s[2] * 2);
    }
}

}  // namespace

void convertOldProgram(const uint8_t* src, Program& prog) {
    // FUN_00453c64. The old record stores most byte fields every 2 bytes (16-bit words).
    uint8_t* d = prog.bytes;
    initProgram(prog);
    {
        // FUN_0040291c(dst + 2, src + 0x202, 15)
        const uint8_t* s = src + 0x202;
        uint8_t n = s[0];
        if (n <= 15) {
            std::memcpy(d + 2, s, size_t(n) + 1);
        } else {
            d[2] = 15;
            std::memcpy(d + 3, s + 1, 15);
        }
    }
    for (int o = 0; o < 3; o++) {
        uint8_t* od = d + ofs::osc(o);
        const uint8_t* s = src + 0x10 * o;
        od[0] = s[0];
        od[1] = s[2];
        od[2] = s[4];
        od[3] = s[6];
        od[4] = s[7];
        copyOldSlots(s + 8, od + 5, 2);
        const uint8_t* s2 = src + 0x30 + 0x10 * o;
        od[0x0e] = s2[0];
        od[0x0f] = s2[2];
        copyOldSlots(s2 + 4, od + 0x10, 2);
    }
    for (int e = 0; e < 4; e++) {
        const uint8_t* s = src + 0x60 + 0x18 * e;
        uint8_t* ed = d + ofs::env(e);
        for (int j = 0; j < 5; j++) ed[j] = s[2 * j];
        ed[5] = s[0x0a];
        ed[6] = s[0x0c];
        for (int j = 0; j < 4; j++) ed[7 + j] = s[0x0e + 2 * j];
        ed[0x0b] = s[0x16];
    }
    for (int l = 0; l < 4; l++) {
        const uint8_t* s = src + 0xc0 + 0x18 * l;
        uint8_t* ld = d + ofs::lfo(l);
        ld[0] = s[0];
        ld[1] = static_cast<uint8_t>(s[2] - 1);
        ld[2] = s[4];
        ld[3] = s[6];
        ld[4] = s[8];
        ld[5] = s[0x0a];
        ld[6] = s[0x0c];
        copyOldSlots(s + 0x10, ld + 7, 1);
        copyOldSlots(s + 0x14, ld + 0x0a, 1);
    }
    copyOldSlots(src + 0x120, d + 0xe2, 3);
    for (int f = 0; f < 2; f++) {
        const uint8_t* s = src + 0x150 + 0x10 * f;
        uint8_t* fd = d + ofs::filter(f);
        fd[0] = s[0];
        fd[1] = s[2];
        fd[2] = s[4];
        fd[3] = s[6];
        copyOldSlots2(s + 8, fd + 4, 2);
    }
    {
        const uint8_t* s = src + 0x170;
        uint8_t* b = d + 0x122;
        b[0] = s[0];
        b[1] = s[2];
        copyOldSlots(s + 4, b + 2, 2);
    }
    {
        const uint8_t* s = src + 0x180;
        uint8_t* b = d + 0x132;
        b[0] = s[0];
        b[1] = s[2];
        copyOldSlots(s + 4, b + 2, 1);
        copyOldSlots2(s + 8, b + 5, 1);
    }
    d[0x13a] = src[0x1b6];
    for (int k = 0; k < 8; k++) d[0x170 + k] = src[0x190 + 2 * k];
    for (int k = 0; k < 7; k++) d[0x188 + k] = src[0x1a0 + 2 * k];
    d[0x190] = src[0x1ae];
    d[0x191] = src[0x1b0];
    d[0x192] = src[0x1b2] & 1;
}

}  // namespace sq8l
