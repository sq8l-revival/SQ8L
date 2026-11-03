// Edit buffer and parameter table, transcribed from the original CeditBuf / CparamEditor.
#include "EditBuffer.h"

#include <cstring>

#include "SoundLibrary.h"
#include "SysEx.h"

namespace sq8l {

// ---------------------------------------------------------------- parameter table

namespace {

// FUN_004600b8: n 3-byte slots described as "int16 + int8".
void descSlots(uint8_t* p, int n) {
    for (int i = 0; i < n; i++) {
        p[3 * i] = 1;
        p[3 * i + 1] = 0;
    }
}

std::vector<uint8_t> buildDescriptor() {
    // FUN_004600e4. One byte per body byte: bits 0-1 = 0 byte / 1 word / 2 special / 3 bit
    // fields; bit 7 set = unsigned.
    std::vector<uint8_t> d(ofs::BodySize, 0);
    uint8_t* b = d.data();
    for (int i = 0; i < 3; i++) {
        uint8_t* o = b + 0x18 * i;
        o[3] = 1;
        o[4] = 0;
        descSlots(o + 5, 2);
        descSlots(o + 0x10, 2);
    }
    for (int i = 0; i < 4; i++) b[0x48 + 0x0e * i + 0x0c] = 0x23;
    for (int i = 0; i < 4; i++) {
        uint8_t* l = b + 0x80 + 0x10 * i;
        descSlots(l + 7, 1);
        descSlots(l + 0x0a, 1);
        l[5] = 0x1f;
        l[0x0d] = 0x57;
        l[0x0e] = 0x80;
    }
    for (int i = 0; i < 3; i++) descSlots(b + 0xc0 + 0x0c * i, 4);
    for (int i = 0; i < 2; i++) {
        uint8_t* f = b + 0xe8 + 0x0e * i;
        descSlots(f, 2);
        descSlots(f + 7, 1);
    }
    descSlots(b + 0x102, 2);
    descSlots(b + 0x112, 1);
    descSlots(b + 0x115, 1);
    b[0x119] = 7;
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 5; k++) {
            b[0x11e + 0x10 * i + 2 * k] = 1;
            b[0x11e + 0x10 * i + 2 * k + 1] = 0;
        }
    b[0x170] = 0x53;
    b[0x174] = 3;
    b[0x17e] = 0x80;
    descSlots(b + 0x184 + 2, 2);
    return d;
}

std::vector<ParamField> buildTable(const std::vector<uint8_t>& desc) {
    // FUN_0045fc94 (fill pass).
    std::vector<ParamField> t;
    const uint32_t n = static_cast<uint32_t>(desc.size());
    uint32_t i = 0;
    while (i < n) {
        const uint8_t d = desc[i];
        uint32_t step = 1;
        const uint8_t kind = d & 3;
        const uint8_t unsignedFlag = static_cast<uint8_t>(~d & 0x80);
        if (kind < 2) {
            t.push_back({i, 0, static_cast<uint8_t>((8u << kind) | unsignedFlag)});
            step = 1u << kind;
        } else if (kind == 2) {
            switch ((d >> 2) & 0x0f) {
                case 0:
                    t.push_back({i, 0, static_cast<uint8_t>(unsignedFlag | 0x20)});
                    step = 4;
                    break;
                case 1:
                    for (int k = 0; k < 4; k++) t.push_back({i, static_cast<uint8_t>(2 * k), 2});
                    break;
                case 2:
                    t.push_back({i, 0, 7});
                    t.push_back({i, 7, 1});
                    break;
                default:
                    break;
            }
        } else {
            uint8_t a = d >> 2;
            int bitsLeft = 6;
            uint8_t pos = 0;
            while (pos < 8 && bitsLeft > 0) {
                const uint8_t code = a & 3;
                uint8_t w;
                if (code < 3) {
                    w = static_cast<uint8_t>(code + 1);
                    a >>= 2;
                    bitsLeft -= 2;
                } else {
                    w = (a & 4) ? 6 : 4;
                    a >>= 3;
                    bitsLeft -= 3;
                }
                t.push_back({i, pos, w});
                pos = static_cast<uint8_t>(pos + w);
            }
            for (; pos < 8; pos++) t.push_back({i, pos, 1});
        }
        i += step;
    }
    return t;
}

}  // namespace

const std::vector<uint8_t>& paramDescriptor() {
    static const std::vector<uint8_t> d = buildDescriptor();
    return d;
}

const std::vector<ParamField>& paramTable() {
    static const std::vector<ParamField> t = buildTable(paramDescriptor());
    return t;
}

int findParam(uint32_t bodyOffset) {
    const auto& t = paramTable();
    for (size_t i = 0; i < t.size(); i++)
        if (t[i].offset == bodyOffset) return static_cast<int>(i);
    return -1;
}

int32_t getParam(const Program& prog, int index) {
    const auto& t = paramTable();
    if (index < 0 || index >= static_cast<int>(t.size())) return 0;
    const ParamField& f = t[size_t(index)];
    const uint8_t* p = prog.bytes + ofs::Body + f.offset;
    switch (f.type) {
        case 0x88: return static_cast<int8_t>(p[0]);
        case 0x90: return static_cast<int16_t>(p[0] | (p[1] << 8));
        case 0xa0:
        case 0x20:
            return static_cast<int32_t>(uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
                                        uint32_t(p[3]) << 24);
        case 0x08: return p[0];
        case 0x10: return p[0] | (p[1] << 8);
        default: return (p[0] >> f.shift) & ((1 << (f.type & 7)) - 1);
    }
}

void setParam(Program& prog, int index, int32_t v) {
    const auto& t = paramTable();
    if (index < 0 || index >= static_cast<int>(t.size())) return;
    const ParamField& f = t[size_t(index)];
    uint8_t* p = prog.bytes + ofs::Body + f.offset;
    switch (f.type) {
        case 0x88:
        case 0x08: p[0] = static_cast<uint8_t>(v); break;
        case 0x90:
        case 0x10:
            p[0] = static_cast<uint8_t>(v);
            p[1] = static_cast<uint8_t>(static_cast<uint32_t>(v) >> 8);
            break;
        case 0xa0:
        case 0x20:
            for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i));
            break;
        default: {
            const uint8_t m = static_cast<uint8_t>((1u << (f.type & 7)) - 1u);
            p[0] = static_cast<uint8_t>((p[0] & static_cast<uint8_t>(~(m << f.shift))) |
                                        static_cast<uint8_t>((static_cast<uint8_t>(v) & m) << f.shift));
            break;
        }
    }
}

// ---------------------------------------------------------------- edit buffer

namespace {
constexpr uint8_t kEditMagic[10] = {9, 'S', 'Q', '8', 'L', '.', 'E', 'D', 'I', 'T'};  // 0x489be0
}

EditBuffer::EditBuffer(SoundLibrary* library) : lib_(library) {
    std::memset(slots_, 0, sizeof slots_);
    initProgram();
}

void EditBuffer::initProgram() {
    sq8l::initProgram(current());
    modified_ = true;
    completedReplacement();
}

void EditBuffer::resetExtZone(int zone) {
    if (zone >= 2) return;
    if (zone < 0) {
        for (int z = 0; z < 2; z++) resetExtZone(z);
        return;
    }
    // FUN_004539dc(ext + 8 + zone * 0x32)
    uint8_t* b = ext_ + 8 + 0x32 * zone;
    std::memset(b + 2, 0, 3);
    std::memset(b + 0x17, 0, 0x1b);
    b[5] = 0x40;
    b[6] = 0;
    modified_ = true;
}

void EditBuffer::setBank(int bank) {
    if (bank <= 0) bank_ = 0;
    else if (bank >= 4) bank_ = 3;
    else bank_ = bank;
}

int EditBuffer::libraryIndex(int prog, int bank) const {
    if (prog < 0) prog = prog_;
    if (bank < 0) bank = bank_;
    return (bank << 7) + prog;
}

void EditBuffer::splitIndex(int index, int& bank, int& prog) {
    if (index < 0 || index >= 0x200) {
        bank = -1;
        prog = -1;
    } else {
        bank = index >> 7;
        prog = index & 0x7f;
    }
}

std::string EditBuffer::name(int which) const {
    if (which == 0) return current().name();
    if (which == 1) return current().name2();
    return std::string();
}

void EditBuffer::setName(int which, const std::string& s) {
    // Copy(s, 1, 15) -> ShortString -> PStrNCpy(.., 15)
    const std::string c = s.substr(0, 15);
    if (which == 0) current().setName(c);
    else if (which == 1) current().setName2(c);
}

void EditBuffer::advanceRing() {
    ring_ = (ring_ + 1) & 3;
    emit(Event::SlotChanged, &slots_[ring_]);
    emit(Event::NextSlot, &slots_[(ring_ + 1) & 3]);
}

void EditBuffer::applyExt() {
    std::memcpy(current().bytes + ofs::Ext, ext_, ofs::ExtSize);
}

void EditBuffer::captureExt(bool force) {
    if (force || !keepExtBuffer_) std::memcpy(ext_, current().bytes + ofs::Ext, ofs::ExtSize);
}

bool EditBuffer::selectProgram(int prog, int bank) {
    advanceRing();
    const int idx = libraryIndex(prog, bank);
    if (!lib_ || !lib_->readProgram(idx, current())) {
        completedReplacement(Replacement::FailedSelection); // ring already advanced
        return false;
    }
    prog_ = prog;
    if (bank >= 0) bank_ = bank;
    modified_ = false;
    captureExt(false);
    completedReplacement();
    emit(Event::ProgramLoaded);
    return true;
}

bool EditBuffer::selectIndex(int index) {
    int bank, prog;
    splitIndex(index, bank, prog);
    if (prog < 0 || bank < 0) return false;
    return selectProgram(prog, bank);
}

bool EditBuffer::writeProgram(int prog, int bank) {
    synchronize();
    applyExt();
    const bool ok = lib_ && lib_->writeProgram(libraryIndex(prog, bank), current());
    if (ok) {
        prog_ = prog;
        if (bank >= 0) bank_ = bank;
        modified_ = false;
        emit(Event::ProgramWritten);
    }
    return ok;
}

void EditBuffer::compare(int prog, int bank) {
    synchronize();
    if (compare_ <= 0) {
        if (lifecycle_) lifecycle_->backupCompare();
        slots_[kRingSize] = current();
        std::memcpy(extBackup_, ext_, ofs::ExtSize);
    }
    advanceRing();
    if (lib_ && lib_->readProgram(libraryIndex(prog, bank), current())) {
        compare_ = 1;
        captureExt(false);
        completedReplacement(Replacement::ComparePreview);
    } else {
        const bool wasComparing = comparing();
        compareOff();
        if (!wasComparing) completedReplacement(Replacement::FailedSelection);
    }
    emit(Event::CompareOn);
}

void EditBuffer::compareOff() {
    if (compare_ <= 0) return;
    advanceRing();
    current() = slots_[kRingSize];
    std::memcpy(ext_, extBackup_, ofs::ExtSize);
    compare_ = 0;
    completedReplacement(Replacement::CompareRestore);
    emit(Event::CompareOff);
}

void EditBuffer::copyChunk(Chunk& out) {
    synchronize();
    out.fill(0);
    uint8_t* b = out.data();
    b[0] = 2;
    std::memcpy(b + 4, kEditMagic, sizeof kEditMagic);
    b[0x18] = static_cast<uint8_t>(prog_);
    b[0x19] = static_cast<uint8_t>(static_cast<uint32_t>(prog_) >> 8);
    b[0x1a] = modified_ ? 1 : 0;
    applyExt();
    std::memcpy(b + kChunkHeaderSize, current().bytes, kProgramSize);
}

const std::vector<uint8_t>& EditBuffer::getChunk() {
    Chunk out;
    copyChunk(out);
    chunk_.assign(out.begin(), out.end());
    return chunk_;
}

int EditBuffer::setChunk(const uint8_t* data, size_t size, bool notifyCompletion) {
    if (size == kChunkSize) {
        // FUN_00460a60
        if (!data || data[0] != 2 || data[1] || data[2] || data[3]) return 0;
        if (std::memcmp(data + 4, kEditMagic, sizeof kEditMagic) != 0) return 0;
        advanceRing();
        std::memcpy(current().bytes, data + kChunkHeaderSize, kProgramSize);
        captureExt(false);
        prog_ = static_cast<int16_t>(data[0x18] | (data[0x19] << 8));
        modified_ = true;
        if (notifyCompletion) completedReplacement();
        emit(Event::ProgramLoaded);
        return static_cast<int>(kChunkSize);
    }
    if (size == kOldChunkSize) {
        // FUN_004609c8
        if (!data || data[0x200] != 1 || data[0x201] != 0) return 0;
        advanceRing();
        convertOldProgram(data, current());
        captureExt(false);
        modified_ = true;
        if (notifyCompletion) completedReplacement();
        emit(Event::ProgramLoaded);
        return static_cast<int>(kOldChunkSize);
    }
    return 0;
}

bool EditBuffer::importSysex(const uint8_t* data, size_t size, bool checkHeader) {
    if (!data) return false;
    if (checkHeader) {
        int type = 0;
        if (size < sysex::kHeaderSize || !sysex::parseHeader(data, &type) || type != sysex::kTypeSingle)
            return false;
    }
    if (size < sysex::kSingleMinSize) return false;
    advanceRing();
    sysex::programFromNybbles(data + sysex::kHeaderSize, current());
    modified_ = true;
    completedReplacement();
    emit(Event::ProgramLoaded);
    return true;
}

std::vector<uint8_t> EditBuffer::exportSysex() const {
    synchronize();
    return sysex::makeSingleDump(current());
}

void EditBuffer::setParamValue(int index, int32_t value) {
    setParam(current(), index, value);
    modified_ = true;
}

// Original object layout used by saveState/loadState (offsets in the CeditBuf instance):
//   +0x004 5 * 540 program slots, +0xa90 ring index, +0xa98 ext, +0xb04 ext backup,
//   +0xb70 bank, +0xb74 program, +0xb84 keep-ext flag, +0xb85 modified, +0xb88 compare.
// Other bytes (pointers, callbacks) are zero in the image.
void EditBuffer::saveState(uint8_t* img) const {
    std::memset(img, 0, kStateSize);
    std::memcpy(img + 4, slots_, sizeof slots_);
    std::memcpy(img + 0xa90, &ring_, 4);
    std::memcpy(img + 0xa98, ext_, ofs::ExtSize);
    std::memcpy(img + 0xb04, extBackup_, ofs::ExtSize);
    std::memcpy(img + 0xb70, &bank_, 4);
    std::memcpy(img + 0xb74, &prog_, 4);
    img[0xb84] = keepExtBuffer_ ? 1 : 0;
    img[0xb85] = modified_ ? 1 : 0;
    std::memcpy(img + 0xb88, &compare_, 4);
}

void EditBuffer::loadState(const uint8_t* img) {
    std::memcpy(slots_, img + 4, sizeof slots_);
    std::memcpy(&ring_, img + 0xa90, 4);
    ring_ &= 3;
    std::memcpy(ext_, img + 0xa98, ofs::ExtSize);
    std::memcpy(extBackup_, img + 0xb04, ofs::ExtSize);
    std::memcpy(&bank_, img + 0xb70, 4);
    std::memcpy(&prog_, img + 0xb74, 4);
    keepExtBuffer_ = img[0xb84] != 0;
    modified_ = img[0xb85] != 0;
    std::memcpy(&compare_, img + 0xb88, 4);
}

}  // namespace sq8l
