// Sound library, transcribed from the original CsoundLib (unit soundLibrary).
#include "SoundLibrary.h"

#include <cstring>

#include "SysEx.h"

namespace sq8l {

namespace {

constexpr uint8_t kLibMagic[9] = {8, 'S', 'Q', '8', 'L', '.', 'L', 'I', 'B'};  // 0x489bd0

int32_t rd32(const uint8_t* p) {
    return static_cast<int32_t>(uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
                                uint32_t(p[3]) << 24);
}
void wr32(uint8_t* p, int32_t v) {
    for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i));
}

// The original reads file images without size checks; bytes past the end of a short file
// read as zero here.
void copyIn(uint8_t* dst, const uint8_t* data, size_t size, size_t off, size_t n) {
    std::memset(dst, 0, n);
    if (off < size) std::memcpy(dst, data + off, size - off < n ? size - off : n);
}

bool hasMagic(const uint8_t* data, size_t size) {
    // CompareMem(data + 4, magic, data[4] + 1): only an exact "\x08SQ8L.LIB" matches.
    return size >= 13 && std::memcmp(data + 4, kLibMagic, sizeof kLibMagic) == 0;
}

}  // namespace

SoundLibrary::SoundLibrary() {
    initLibrary(true);
    restoreBackup(nullptr, 0);
}

SoundLibrary::SoundLibrary(const std::vector<uint8_t>& backupFile) {
    initLibrary(true);
    restoreBackup(backupFile.empty() ? nullptr : backupFile.data(), backupFile.size());
}

void SoundLibrary::initHeader() {
    std::memset(header_, 0, kHeaderSize);
    wr32(header_, 2);
    std::memcpy(header_ + 4, kLibMagic, sizeof kLibMagic);
    wr32(header_ + 0x14, 0x100);
    static const uint8_t kInit[5] = {4, 'I', 'N', 'I', 'T'};
    std::memcpy(header_ + 0x18, kInit, 5);
    header_[0x28] = 0;
}

void SoundLibrary::notify(int param) {
    // CglobalData_v004: v003(0, param) calls every listener, then +0x0c := 0.
    if (onChanged) onChanged(param);
    clean_ = false;
}

void SoundLibrary::initLibrary(bool allBanks) {
    initHeader();
    const int n = allBanks ? kNumPrograms : kNumUserPrograms;
    for (int i = 0; i < n; i++) initProgram(programs_[i]);
    clean_ = false;
}

void SoundLibrary::initBank(int bank) {
    if (bank < 0 || bank >= kNumBanks) return;
    for (int i = 0; i < kBankSize; i++) initProgram(programs_[bank * kBankSize + i]);
    clean_ = false;
}

void SoundLibrary::loadFactory() {
    // Bank C: the decompressed SQ8L library blob (type 2, 128 programs).
    for (int i = 0; i < kBankSize; i++)
        std::memcpy(programs_[0x100 + i].bytes, data::kFactoryBankC[i], kProgramSize);
    // Bank D: FUN_00454aec(raw SQ-80 program, slot, 1, 0, 4, -12) for the 40 SQ-80 sounds.
    for (int i = 0; i < kFactoryD; i++)
        std::memcpy(programs_[0x180 + i].bytes, data::kFactoryBankD[i], kProgramSize);
    notify(-1);
}

bool SoundLibrary::copyBanks(int srcBank, int dstBank, int count) {
    bool ok = true;
    for (int b = 0; b < count; b++) {
        if (!ok) continue;
        // FUN_00455468: always succeeds; invalid slots are skipped.
        for (int i = 0; i < kBankSize; i++) {
            const Program* s = program((srcBank + b) * kBankSize + i);
            Program* d = program((dstBank + b) * kBankSize + i);
            if (s && d) std::memcpy(d->bytes, s->bytes, kProgramSize);
        }
    }
    return ok;
}

void SoundLibrary::restoreBackup(const uint8_t* data, size_t size) {
    loadFactory();
    const bool ok = (data && size > 0) ? loadLibrary(data, size) : false;
    if (!ok) copyBanks(2, 0, 2);
    clean_ = true;
}

std::vector<uint8_t> SoundLibrary::saveBackup() {
    std::vector<uint8_t> out = saveLibrary();
    clean_ = true;
    return out;
}

bool SoundLibrary::loadOldBank(const uint8_t* data, size_t size, int bank) {
    if (bank < 0) bank = 0;
    else if (bank >= kNumBanks) return false;
    if (rd32(data) != 1) return false;
    uint8_t old[kOldProgramSize];
    for (int i = 0; i < kBankSize; i++) {
        copyIn(old, data, size, 0x14 + size_t(i) * kOldProgramSize, kOldProgramSize);
        convertOldProgram(old, programs_[bank * kBankSize + i]);
    }
    clean_ = false;
    return true;
}

bool SoundLibrary::loadLibrary(const uint8_t* data, size_t size) {
    if (!data || size < 4) return false;
    bool ok = false;
    const int32_t type = rd32(data);
    if (type == 1) {
        ok = loadOldBank(data, size, -1);
    } else if (type == 2) {
        if (!hasMagic(data, size)) return false;  // Exit: no flag change, no notification
        copyIn(header_, data, size, 0, kHeaderSize);
        int32_t count = rd32(header_ + 0x14);
        wr32(header_ + 0x14, 0x100);
        if (count > 0x100) count = 0x100;
        for (int32_t i = 0; i < count; i++)
            copyIn(programs_[i].bytes, data, size, kHeaderSize + size_t(i) * kProgramSize,
                   kProgramSize);
        ok = true;
    }
    clean_ = false;
    notify(-1);
    return ok;
}

std::vector<uint8_t> SoundLibrary::saveLibrary() {
    std::vector<uint8_t> out(kLibraryFileSize);
    std::memcpy(out.data(), header_, kHeaderSize);  // (the magic written first is overwritten)
    for (int i = 0; i < kNumUserPrograms; i++)
        std::memcpy(out.data() + kHeaderSize + size_t(i) * kProgramSize, programs_[i].bytes,
                    kProgramSize);
    clean_ = false;
    return out;
}

bool SoundLibrary::loadBank(const uint8_t* data, size_t size, int bank) {
    if (!data || size < 4) return false;
    bool ok = false;
    const int32_t type = rd32(data);
    if (type == 1) {
        ok = loadOldBank(data, size, bank);
    } else if (type == 2) {
        if (!hasMagic(data, size)) return false;
        if (bank < 0) bank = 0;
        else if (bank >= kNumBanks) return false;
        int32_t count = size >= 0x18 ? rd32(data + 0x14) : 0;
        if (count > kBankSize) count = kBankSize;
        for (int32_t i = 0; i < count; i++)
            copyIn(programs_[bank * kBankSize + i].bytes, data, size,
                   kHeaderSize + size_t(i) * kProgramSize, kProgramSize);
        ok = true;
    }
    clean_ = false;
    notify(-1);
    return ok;
}

std::vector<uint8_t> SoundLibrary::saveBank(int bank) {
    if (bank < 0) bank = 0;
    else if (bank >= kNumBanks) return {};
    std::vector<uint8_t> out(kBankFileSize);
    std::memcpy(out.data(), header_, kHeaderSize);
    wr32(out.data() + 0x14, 0x80);
    for (int i = 0; i < kBankSize; i++)
        std::memcpy(out.data() + kHeaderSize + size_t(i) * kProgramSize,
                    programs_[bank * kBankSize + i].bytes, kProgramSize);
    clean_ = false;
    return out;
}

bool SoundLibrary::importSysexBank(const uint8_t* data, size_t size, int startIndex,
                                   bool checkHeader) {
    if (!data) return false;
    if (checkHeader) {
        int type = 0;
        if (size < sysex::kHeaderSize || !sysex::parseHeader(data, &type) || type != sysex::kTypeBank)
            return false;
    }
    if (size < sysex::kBankMinSize) return false;
    const uint8_t* src = data + sysex::kHeaderSize;
    for (int i = 0; i < int(sysex::kBankPrograms); i++, src += sysex::kProgramNybbles) {
        const int idx = i + startIndex;
        if (idx < 0) return false;  // (programs converted so far stay)
        Program* p = program(idx);
        if (!p) return false;
        sysex::programFromNybbles(src, *p);
    }
    clean_ = false;
    notify(-1);
    return true;
}

std::vector<uint8_t> SoundLibrary::exportSysexBank(int startIndex) {
    std::vector<uint8_t> out(sysex::kBankDumpSize, 0);
    sysex::writeHeader(out.data(), sysex::kTypeBank, 0);
    uint8_t* dst = out.data() + sysex::kHeaderSize;
    for (int i = 0; i < int(sysex::kBankPrograms); i++, dst += sysex::kProgramNybbles) {
        const Program* p = program(startIndex + i);
        if (!p) return out;  // the original returns the partially filled buffer (no F7)
        sysex::programToNybbles(*p, dst, false);
    }
    out[sysex::kBankDumpSize - 1] = 0xf7;
    return out;
}

Program* SoundLibrary::program(int index) {
    return (index >= 0 && index < kNumPrograms) ? &programs_[index] : nullptr;
}

const Program* SoundLibrary::program(int index) const {
    return (index >= 0 && index < kNumPrograms) ? &programs_[index] : nullptr;
}

std::string SoundLibrary::programName(int index) const {
    const Program* p = program(index);
    return p ? p->name() : std::string();
}

bool SoundLibrary::readProgram(int index, Program& out) const {
    const Program* p = program(index);
    if (!p) return false;
    out = *p;
    return true;
}

bool SoundLibrary::writeProgram(int index, const Program& in) {
    if (isWriteProtected(index)) return false;
    Program* p = program(index);
    if (!p) return false;
    *p = in;
    notify(index);
    return true;
}

}  // namespace sq8l
