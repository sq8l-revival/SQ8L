// Edit buffer (original class CeditBuf, unit editBuffer 0x4602c4..0x461164) and the generic
// parameter accessor used by the editor (CparamEditor, unit paramEditor / unit_4600e0).
//
// The edit buffer keeps a ring of 4 program slots; every operation that loads a new program
// (select, chunk, SysEx import, compare) first advances the ring, so voices still playing the
// previous program keep reading valid memory. A 5th slot holds the program saved while COMPARE
// is active. The 0x6c-byte extension block at program+0x1b0 is mirrored in a separate buffer:
// it is captured from every program loaded from the library, a chunk or COMPARE, and written
// back into the program before it is stored (WRITE) or saved in a chunk.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Program.h"

namespace sq8l {

class SoundLibrary;

// ---------------------------------------------------------------- parameter table
// One addressable parameter of the program body (offset relative to program + 0x22).
// type: 0x88 s8, 0x08 u8, 0x90 s16, 0x10 u16, 0xa0 s32, 0x20 u32, 1..7 = bit field width.
struct ParamField {
    uint32_t offset;
    uint8_t shift;
    uint8_t type;
};
constexpr int kNumParams = 398;
// The 398 entries built at edit buffer creation (FUN_004600e4 + FUN_0045fc94).
const std::vector<ParamField>& paramTable();
// The 0x18e-byte field descriptor the table is built from (FUN_004600e4).
const std::vector<uint8_t>& paramDescriptor();
int findParam(uint32_t bodyOffset);                       // FUN_0045fc60 (first entry, -1 if none)
int32_t getParam(const Program& p, int index);            // FUN_0045fa38
void setParam(Program& p, int index, int32_t value);      // FUN_0045fb30 (without callback)

// ---------------------------------------------------------------- edit buffer
class EditBuffer {
public:
    // Events sent to the owner (original FUN_00460998, registered by the master with
    // FUN_004610f4). `slot` is only set for SlotChanged/NextSlot.
    enum class Event : int {
        ProgramLoaded = 0,   // new program in the edit buffer (select, chunk, SysEx import)
        ProgramWritten = 1,  // edit buffer written to the library
        CompareOn = 2,
        CompareOff = 3,
        SlotChanged = 4,     // ring advanced: `slot` is the new current program
        NextSlot = 5,        // `slot` is the ring slot that will be reused next
    };
    using Listener = std::function<void(Event, const Program* slot)>;

    static constexpr int kRingSize = 4;
    static constexpr size_t kChunkSize = 0x23b;     // 0x1f header + 540
    static constexpr size_t kChunkHeaderSize = 0x1f;
    static constexpr size_t kOldChunkSize = 0x220;  // pre-0.90 chunk = old program record

    // Constructor FUN_004603b8: slot 0 current, INIT program, modified.
    explicit EditBuffer(SoundLibrary* library);

    Program& current() { return slots_[ring_]; }
    const Program& current() const { return slots_[ring_]; }
    int currentSlot() const { return ring_; }
    const Program& slot(int i) const { return slots_[i]; }  // 0..3 ring, 4 compare backup

    // INIT button (FUN_00460554): INIT program into the current slot, no ring advance, no event.
    void initProgram();
    // FUN_004605ac: reset zone block 0/1 of the extension buffer (-1: both).
    void resetExtZone(int zone);

    // Bank / program position (FUN_00460630, 460658, 460674).
    void setBank(int bank);
    int bank() const { return bank_; }
    int programNumber() const { return prog_; }
    int libraryIndex(int prog = -1, int bank = -1) const;
    static void splitIndex(int index, int& bank, int& prog);

    // Names (FUN_004606b4 / FUN_0046072c): which = 0 name, 1 second name.
    std::string name(int which = 0) const;
    void setName(int which, const std::string& s);

    // Library interaction.
    bool selectProgram(int prog, int bank);   // FUN_00460db8 (setProgram)
    bool selectIndex(int index);              // master FUN_004631f0: index 0..511
    bool writeProgram(int prog, int bank);    // FUN_00460e74 (WRITE)
    void compare(int prog, int bank);         // FUN_00460f2c (COMPARE on / other slot)
    void compareOff();                        // FUN_0046104c
    bool comparing() const { return compare_ > 0; }

    // Host chunk (effGetChunk / effSetChunk): FUN_00460b84 / FUN_00460b40.
    const std::vector<uint8_t>& getChunk();
    int setChunk(const uint8_t* data, size_t size);  // bytes consumed (0x23b / 0x220) or 0

    // SQ-80/ESQ-1 single program SysEx (FUN_00460c68 / FUN_00460d10).
    bool importSysex(const uint8_t* data, size_t size, bool checkHeader);
    std::vector<uint8_t> exportSysex() const;

    // Parameter edit through the parameter table (CparamEditor + FUN_0046095c): sets the
    // modified flag. No event is sent (the original forwarding callback is never registered).
    int32_t param(int index) const { return getParam(current(), index); }
    void setParamValue(int index, int32_t value);

    bool modified() const { return modified_; }
    void setModified(bool m) { modified_ = m; }   // FUN_00460954
    uint32_t programWord() const { return static_cast<uint32_t>(uint16_t(prog_)) << 16; }  // FUN_0046093c

    uint8_t* ext() { return ext_; }               // FUN_004606ac
    void applyExt();                              // FUN_004608dc: buffer -> program ext
    void captureExt(bool force);                  // FUN_00460868: program ext -> buffer

    Listener listener;

    // State image in the original object layout (for differential tests): see the .cpp.
    static constexpr size_t kStateSize = 0xb8c;
    void saveState(uint8_t* image) const;
    void loadState(const uint8_t* image);

private:
    void advanceRing();  // FUN_00460498
    void emit(Event e, const Program* slot = nullptr) {
        if (listener) listener(e, slot);
    }

    SoundLibrary* lib_;
    Program slots_[kRingSize + 1];   // +0x004 (ring) and +0x874 (compare backup)
    int32_t ring_ = 0;               // +0xa90
    uint8_t ext_[ofs::ExtSize] = {}; // +0xa98
    uint8_t extBackup_[ofs::ExtSize] = {}; // +0xb04
    int32_t bank_ = 0;               // +0xb70
    int32_t prog_ = 0;               // +0xb74
    bool keepExtBuffer_ = false;     // +0xb84 (never set by the original)
    bool modified_ = false;          // +0xb85
    int32_t compare_ = 0;            // +0xb88
    std::vector<uint8_t> chunk_;     // +0xb8c
};

}  // namespace sq8l
