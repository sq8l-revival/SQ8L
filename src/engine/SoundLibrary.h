// Sound library (original class CsoundLib, unit soundLibrary 0x454f90..0x455eb0).
//
// 512 programs = 4 banks of 128: A, B (user, 0..255), C (SQ8L factory, 256..383) and
// D (SQ-80 factory, 384..511; only 0..39 used). The original keeps one global instance shared
// by all plugin instances; it is created at DLL load (factory banks + backup file restored)
// and saved back to SQ8L_backup.dat at DLL unload. File I/O is left to the caller: all
// load/save functions take/return byte buffers with the exact original file contents.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Program.h"

namespace sq8l {

namespace data {
extern const uint8_t kFactoryBankC[128][kProgramSize];
extern const uint8_t kFactoryBankD[40][kProgramSize];
}  // namespace data

class SoundLibrary {
public:
    static constexpr int kNumPrograms = 512;
    static constexpr int kBankSize = 128;
    static constexpr int kNumBanks = 4;
    static constexpr int kNumUserPrograms = 256;  // banks A, B (writable)
    static constexpr int kFactoryD = 40;          // used slots of bank D
    static constexpr size_t kHeaderSize = 0x3c;
    static constexpr size_t kLibraryFileSize = 0x21c3c;  // header + 256 programs
    static constexpr size_t kBankFileSize = 0x10e3c;     // header + 128 programs
    static constexpr size_t kOldBankDataSize = 0x14 + 128 * kOldProgramSize;  // pre-0.90 files
    static constexpr const char* kBackupFileName = "SQ8L_backup.dat";

    // What the original does at DLL load (FUN_00454fe4): header + all 512 slots INIT, factory
    // banks C/D, then the backup is restored (restoreBackup(nullptr, 0) if there is no file).
    SoundLibrary();
    explicit SoundLibrary(const std::vector<uint8_t>& backupFile);

    // Factory banks C and D (FUN_004552a4). Notifies listeners.
    void loadFactory();
    // CsoundLib_v005: factory + backup library file; without a usable file, banks A/B become
    // copies of C/D. Leaves the library "clean".
    void restoreBackup(const uint8_t* data, size_t size);
    // CsoundLib_v006: the bytes to write to SQ8L_backup.dat (an ordinary library file).
    std::vector<uint8_t> saveBackup();

    // Library files (*.8XL; *.dat): 256 user programs. FUN_00455534 / FUN_00455708.
    bool loadLibrary(const uint8_t* data, size_t size);
    std::vector<uint8_t> saveLibrary();
    // Bank files: 128 programs into/from bank 0..3. FUN_00455884 / FUN_00455998. The original
    // library allows loading into banks C/D (only the GUI refuses: isBankWriteProtected).
    bool loadBank(const uint8_t* data, size_t size, int bank);
    std::vector<uint8_t> saveBank(int bank);  // empty if bank >= 4 (bank < 0 means 0)
    // Init library (FUN_004550b0(false): user banks only) / init bank (FUN_004550f4).
    void initLibrary(bool allBanks = false);
    void initBank(int bank);
    // FUN_004554cc: copy `count` whole banks.
    bool copyBanks(int srcBank, int dstBank, int count);

    // SQ-80/ESQ-1 bank dumps (40 programs). FUN_00455a8c / FUN_00455b70.
    bool importSysexBank(const uint8_t* data, size_t size, int startIndex, bool checkHeader);
    std::vector<uint8_t> exportSysexBank(int startIndex);

    // Program access (FUN_00455c44 / 455cac / 455d30 / 455da8).
    Program* program(int index);
    const Program* program(int index) const;
    std::string programName(int index) const;
    bool readProgram(int index, Program& out) const;
    bool writeProgram(int index, const Program& in);  // refuses factory slots (>= 256)

    static bool isWriteProtected(int index) { return index >= 0x100; }    // FUN_00455d14
    static bool isBankWriteProtected(int bank) { return bank >= 2; }     // FUN_00455d20
    static int index(int prog, int bank) { return bank * kBankSize + prog; }  // FUN_00455d28

    const uint8_t* header() const { return header_; }
    // +0x0c of the original object: true right after the backup was restored/saved, cleared by
    // every modification that notifies.
    bool clean() const { return clean_; }
    void setClean(bool c) { clean_ = c; }

    // Change notification (CglobalData listeners): called with param -1 after bulk changes
    // (factory, library/bank load, SysEx bank import) and with the slot index after
    // writeProgram.
    std::function<void(int param)> onChanged;

    // Raw image access for tests: the 512 * 540 bytes starting at object offset 0x20.
    uint8_t* raw() { return programs_[0].bytes; }

private:
    void initHeader();         // FUN_00455058
    void notify(int param);    // vmt+0x10 (CglobalData_v004)
    bool loadOldBank(const uint8_t* data, size_t size, int bank);  // FUN_004553a4

    Program programs_[kNumPrograms];
    uint8_t header_[kHeaderSize];
    bool clean_ = false;
};

}  // namespace sq8l
