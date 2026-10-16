// SQ-80 / ESQ-1 SysEx conversion (original unit unit_453adc + helpers in unit_450db8).
//
// An ESQ-1/SQ-80 program is 102 bytes; in a SysEx dump each byte is sent as two nybbles
// (low first): 204 bytes. Dumps: F0 0F 02 <channel> <type> <data...> F7 with type 1 = single
// program (210 bytes total), type 2 = 40-program bank (8166 bytes total).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Program.h"

namespace sq8l::sysex {

constexpr size_t kHeaderSize = 5;
constexpr size_t kProgramNybbles = 0xcc;     // 204
constexpr size_t kSq80ProgramBytes = 0x66;   // 102 (raw, un-nybbled; used for the bank D data)
constexpr size_t kSingleDumpSize = 0xd2;     // 210 = 5 + 204 + F7
constexpr size_t kSingleMinSize = 0xd1;      // minimum accepted by the single import
constexpr size_t kBankPrograms = 40;
constexpr size_t kBankDumpSize = 0x1fe6;     // 8166 = 5 + 40 * 204 + F7
constexpr size_t kBankMinSize = 0x1fe5;
constexpr uint8_t kTypeSingle = 1, kTypeBank = 2;

// FUN_00453fb0: F0 0F 02 xx <type>. Returns false if the first three bytes don't match.
bool parseHeader(const uint8_t* data, int* type);
// FUN_00453fd8: writes F0 0F 02 <channel> <type>; only types 1 and 2 are accepted.
bool writeHeader(uint8_t* data, uint8_t type, uint8_t channel);

// Mod source mapping SQ-80 (0..15) <-> SQ8L (FUN_00453b14 / FUN_00453ba0).
int16_t sourceFromSq80(uint8_t src);
uint8_t sourceToSq80(int16_t src);

// Name conversion. SQ-80 characters with a decimal point ('!' -> "0.", '#' -> "1.", ...) are
// expanded; characters outside 0x20..0x5f are dropped (FUN_004510d4 / FUN_00450e3c).
std::string nameFromSq80(const uint8_t* chars, int n);
// FUN_0045113c: upper case, "d." -> special char, spaces dropped, zero padded to n bytes.
std::vector<uint8_t> nameToSq80(std::string_view name, int n);

// FUN_00454054: 204 nybbles -> program (starts from the INIT program).
void programFromNybbles(const uint8_t* nyb, Program& p);
// FUN_004545dc: program -> 204 nybbles. esq1: clamp wave numbers to 0..31 (ESQ-1 has 32
// waves; the original always passes false).
void programToNybbles(const Program& p, uint8_t* nyb, bool esq1 = false);
// FUN_00454aec: raw 102-byte program -> program. The factory bank D is built with
// (emuFlags = 1, emuFlags3 = 0, emuFlags2 = 4, levelOffset = -12).
void programFromSq80Bytes(const uint8_t* raw, Program& p, uint8_t emuFlags, uint8_t emuFlags3,
                          uint8_t emuFlags2, int8_t levelOffset);

// Single program dump as produced by the edit buffer export (FUN_00460d10): 210 bytes,
// channel 0.
std::vector<uint8_t> makeSingleDump(const Program& p);

}  // namespace sq8l::sysex
