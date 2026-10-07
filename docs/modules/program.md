# Sound program record (540 bytes)

C++: `src/engine/Program.{h,cpp}` (`struct Program { uint8_t bytes[540]; }`, offsets in
`namespace ofs`, `initProgram`, `convertOldProgram`). Tests: `tests/test_program.py`.

The SQ8L stores a sound program as a raw 0x21c-byte record. The same bytes are used in the edit
buffer, in the 512-slot library, in library/bank files (`*.8XL`, `SQ8L_backup.dat`) and in the
host chunk, so the port keeps the layout byte for byte. All multi-byte values are little
endian and unaligned.

| range | size | content |
|---|---|---|
| 0x000 | 2 | version, always 2 (`word`) |
| 0x002 | 16 | name: Delphi `ShortString[15]` (length byte + up to 15 chars) |
| 0x012 | 16 | second `ShortString[15]` (read/write API exists, unused by the 0.91b GUI: empty) |
| 0x022 | 0x18e | parameter body (the base of the original `CparamEditor`) |
| 0x1b0 | 0x6c | extension block (zones, see below); mirrored by the edit buffer |

## Encodings

* `s8`/`u8`/`s16`: as in the field table. Unused bytes are 0 in programs made by the SQ8L.
* **Modulation slot** (3 bytes): `int16 source`, `int8 amount`. Source -1 (0xffff) = OFF.
  INIT/`clearModSlots` (FUN_0045377c) writes 0xffff into the source and leaves the amount.
* **Mod source numbers** (table of names built at 0x4c4ce0 by FUN_0044ffa0):
  -1 OFF, 0-3 LFO1-4, 4-7 ENV1-4, 8 VEL, 9 VEL-X, 10 KEYB, 11 KEYB2, 12 PRESS (poly key
  pressure), 13-15 MAT1-3, 16+n = MIDI CC n (0..127; 17 WHEEL, 18 BREATH, 20 PEDAL, 23 VOLUME,
  ...), 144 CHPRES (channel pressure), 145 BEND. The GUI range is -1..145.
* Names: the ShortString assignment (`FUN_0040291c(dst, s, 15)`) writes the length byte and the
  characters only; bytes after the new length keep their old contents (e.g. a 2-char name
  written over "INIT" leaves "IT" behind). `Program::setName` reproduces this. Reading uses the
  full length byte (a corrupt length > 15 reads into the following bytes).

## INIT program (FUN_004539c0)

`initProgram()` = FUN_00453814 (body + header) + FUN_004537c0 (extension):
zero 0x1b0 bytes; per oscillator DCA LEVEL 50, OSC1 enabled, all 4 slots OFF; envelopes
L1=L2=L3=63; LFOs FREQ 24, RESET OFF, DELAY 63, L2 63, slots OFF; matrices, filter and DCA4
slots OFF; FILTER FREQ 127 (both filters); DCA4 level 63; +0x14a/+0x15a/+0x16a = 1;
+0x18e = 60, +0x182 = 4, +0x183 = 16, BEND 2, +0x192 = 1 (AMBUG), +0x196 = 0, +0x1a0 = 120,
+0x1a1 = 2, +0x1a3 = 32; version 2; name "INIT". Extension: zero, then two 0x32-byte zones at
+0x1b8 and +0x1ea with byte +1 = 0xff and 9 OFF slots at +0x17.
The library holds INIT in every slot that has nothing else (bank D 40..127 after load).

## Parameter table (CparamEditor)

The editor addresses the body through a 398-entry table (`paramTable()`), built at edit
buffer creation from a one-byte-per-body-byte descriptor (`paramDescriptor()`, FUN_004600e4):
bits 0-1 of a descriptor byte: 0 byte, 1 word, 2 special (sub-type 0 dword, 1 four 2-bit
fields, 2 7+1 bits), 3 bit fields (packed 2-bit width codes); bit 7 set = unsigned. Entry
types: 0x88 s8, 0x08 u8, 0x90 s16, 0x10 u16, 0xa0 s32, 0x20 u32, 1..7 = bit field width.
`getParam`/`setParam` reproduce FUN_0045fa38/FUN_0045fb30 (verified for all 398 entries).
The "#" column below is the parameter index.

## Field table

Absolute offset, body offset (offset - 0x22), parameter index, type, meaning, GUI range
(from the LCD page templates of unit_47e330), INIT value, notes (SQ80 = how the SysEx import
fills the field, see "SysEx" below). Fields marked "not in the GUI" have no LCD page in
0.91b; they are presumably prepared for the SQ8X (layers, 2nd filter ...). Their values must
still be preserved.

| off | body | # | type | field | range | INIT | notes |
|---|---|---|---|---|---|---|---|
| 0x022 | 0x000 | 0 | s8 | OSC1 OCT | -3..+5 | 0 | SQ80 octave (pitch byte / 12 - 3) |
| 0x023 | 0x001 | 1 | s8 | OSC1 SEMI | -12..+12 | 0 | SQ80 0..11 (pitch byte % 12) |
| 0x024 | 0x002 | 2 | s8 | OSC1 FINE | -31..+31 | 0 | SQ80 0..31 (byte >> 3) |
| 0x025 | 0x003 | 3 | s16 | OSC1 WAVE | 0..74 | 0 | SQ80 wave number |
| 0x027 | 0x005 | 4 | s16 | OSC1 MOD1 (pitch) source | -1..145 | -1 | slot: s16 source |
| 0x029 | 0x007 | 5 | s8 | OSC1 MOD1 amount | -63..+63 | 0 | SQ80 amount / 2 |
| 0x02a | 0x008 | 6 | s16 | OSC1 MOD2 (pitch) source | -1..145 | -1 |  |
| 0x02c | 0x00a | 7 | s8 | OSC1 MOD2 amount | -63..+63 | 0 |  |
| 0x02d | 0x00b | 8 | s8 | (unused) |  | 0 | always 0 |
| 0x02e | 0x00c | 9 | s8 | (unused) |  | 0 | always 0 |
| 0x02f | 0x00d | 10 | s8 | (unused) |  | 0 | always 0 |
| 0x030 | 0x00e | 11 | s8 | DCA1 LEVEL | 0..63 | 50 | SQ80 (byte & 0x7f) >> 1 |
| 0x031 | 0x00f | 12 | s8 | DCA1 OUTPUT (enable) | 0..1 | 1 | SQ80 bit 7 |
| 0x032 | 0x010 | 13 | s16 | DCA1 MOD1 source | -1..145 | -1 |  |
| 0x034 | 0x012 | 14 | s8 | DCA1 MOD1 amount | -63..+63 | 0 |  |
| 0x035 | 0x013 | 15 | s16 | DCA1 MOD2 source | -1..145 | -1 |  |
| 0x037 | 0x015 | 16 | s8 | DCA1 MOD2 amount | -63..+63 | 0 |  |
| 0x038 | 0x016 | 17 | s8 | (unused) |  | 0 | always 0 |
| 0x039 | 0x017 | 18 | s8 | (unused) |  | 0 | always 0 |
| 0x03a | 0x018 | 19 | s8 | OSC2 OCT | -3..+5 | 0 | SQ80 octave (pitch byte / 12 - 3) |
| 0x03b | 0x019 | 20 | s8 | OSC2 SEMI | -12..+12 | 0 | SQ80 0..11 (pitch byte % 12) |
| 0x03c | 0x01a | 21 | s8 | OSC2 FINE | -31..+31 | 0 | SQ80 0..31 (byte >> 3) |
| 0x03d | 0x01b | 22 | s16 | OSC2 WAVE | 0..74 | 0 | SQ80 wave number |
| 0x03f | 0x01d | 23 | s16 | OSC2 MOD1 (pitch) source | -1..145 | -1 | slot: s16 source |
| 0x041 | 0x01f | 24 | s8 | OSC2 MOD1 amount | -63..+63 | 0 | SQ80 amount / 2 |
| 0x042 | 0x020 | 25 | s16 | OSC2 MOD2 (pitch) source | -1..145 | -1 |  |
| 0x044 | 0x022 | 26 | s8 | OSC2 MOD2 amount | -63..+63 | 0 |  |
| 0x045 | 0x023 | 27 | s8 | (unused) |  | 0 | always 0 |
| 0x046 | 0x024 | 28 | s8 | (unused) |  | 0 | always 0 |
| 0x047 | 0x025 | 29 | s8 | (unused) |  | 0 | always 0 |
| 0x048 | 0x026 | 30 | s8 | DCA2 LEVEL | 0..63 | 50 | SQ80 (byte & 0x7f) >> 1 |
| 0x049 | 0x027 | 31 | s8 | DCA2 OUTPUT (enable) | 0..1 | 0 | SQ80 bit 7 |
| 0x04a | 0x028 | 32 | s16 | DCA2 MOD1 source | -1..145 | -1 |  |
| 0x04c | 0x02a | 33 | s8 | DCA2 MOD1 amount | -63..+63 | 0 |  |
| 0x04d | 0x02b | 34 | s16 | DCA2 MOD2 source | -1..145 | -1 |  |
| 0x04f | 0x02d | 35 | s8 | DCA2 MOD2 amount | -63..+63 | 0 |  |
| 0x050 | 0x02e | 36 | s8 | (unused) |  | 0 | always 0 |
| 0x051 | 0x02f | 37 | s8 | (unused) |  | 0 | always 0 |
| 0x052 | 0x030 | 38 | s8 | OSC3 OCT | -3..+5 | 0 | SQ80 octave (pitch byte / 12 - 3) |
| 0x053 | 0x031 | 39 | s8 | OSC3 SEMI | -12..+12 | 0 | SQ80 0..11 (pitch byte % 12) |
| 0x054 | 0x032 | 40 | s8 | OSC3 FINE | -31..+31 | 0 | SQ80 0..31 (byte >> 3) |
| 0x055 | 0x033 | 41 | s16 | OSC3 WAVE | 0..74 | 0 | SQ80 wave number |
| 0x057 | 0x035 | 42 | s16 | OSC3 MOD1 (pitch) source | -1..145 | -1 | slot: s16 source |
| 0x059 | 0x037 | 43 | s8 | OSC3 MOD1 amount | -63..+63 | 0 | SQ80 amount / 2 |
| 0x05a | 0x038 | 44 | s16 | OSC3 MOD2 (pitch) source | -1..145 | -1 |  |
| 0x05c | 0x03a | 45 | s8 | OSC3 MOD2 amount | -63..+63 | 0 |  |
| 0x05d | 0x03b | 46 | s8 | (unused) |  | 0 | always 0 |
| 0x05e | 0x03c | 47 | s8 | (unused) |  | 0 | always 0 |
| 0x05f | 0x03d | 48 | s8 | (unused) |  | 0 | always 0 |
| 0x060 | 0x03e | 49 | s8 | DCA3 LEVEL | 0..63 | 50 | SQ80 (byte & 0x7f) >> 1 |
| 0x061 | 0x03f | 50 | s8 | DCA3 OUTPUT (enable) | 0..1 | 0 | SQ80 bit 7 |
| 0x062 | 0x040 | 51 | s16 | DCA3 MOD1 source | -1..145 | -1 |  |
| 0x064 | 0x042 | 52 | s8 | DCA3 MOD1 amount | -63..+63 | 0 |  |
| 0x065 | 0x043 | 53 | s16 | DCA3 MOD2 source | -1..145 | -1 |  |
| 0x067 | 0x045 | 54 | s8 | DCA3 MOD2 amount | -63..+63 | 0 |  |
| 0x068 | 0x046 | 55 | s8 | (unused) |  | 0 | always 0 |
| 0x069 | 0x047 | 56 | s8 | (unused) |  | 0 | always 0 |
| 0x06a | 0x048 | 57 | s8 | ENV1 L0 (start level) | -63..+63 | 0 | SQ8L only (0 from SysEx) |
| 0x06b | 0x049 | 58 | s8 | ENV1 L1 | -63..+63 | 63 | SQ80 signed byte / 2 |
| 0x06c | 0x04a | 59 | s8 | ENV1 L2 | -63..+63 | 63 |  |
| 0x06d | 0x04b | 60 | s8 | ENV1 L3 | -63..+63 | 63 |  |
| 0x06e | 0x04c | 61 | s8 | ENV1 (unknown, not in the GUI) |  | 0 | one factory program uses 1/3 |
| 0x06f | 0x04d | 62 | s8 | ENV1 LV | 0..127 | 0 | bits 0-5 amount, bit 6 X (exponential); SQ80 byte >> 2 |
| 0x070 | 0x04e | 63 | s8 | ENV1 T1V | 0..63 | 0 |  |
| 0x071 | 0x04f | 64 | s8 | ENV1 T1 | 0..63 | 0 |  |
| 0x072 | 0x050 | 65 | s8 | ENV1 T2 | 0..63 | 0 |  |
| 0x073 | 0x051 | 66 | s8 | ENV1 T3 | 0..63 | 0 |  |
| 0x074 | 0x052 | 67 | s8 | ENV1 T4 | 0..127 | 0 | bits 0-5 time, bit 6 = 2nd release (SQ80 bit 7) |
| 0x075 | 0x053 | 68 | s8 | ENV1 TK | 0..63 | 0 |  |
| 0x076 | 0x054 | 69 | bits 0:1 1:3 4:1 5:1 6:1 7:1 | ENV1 mode bits |  | 0x00 | b0 CYC, b1-3 SHAPE (OFF EXP EXP2 EXP3 EXP4 TAN TAN2 TAN3), b4 T1V mode (T1/SMT) |
| 0x077 | 0x055 | 75 | s8 | ENV1 SMTH | 0..63 | 0 |  |
| 0x078 | 0x056 | 76 | s8 | ENV2 L0 (start level) | -63..+63 | 0 | SQ8L only (0 from SysEx) |
| 0x079 | 0x057 | 77 | s8 | ENV2 L1 | -63..+63 | 63 | SQ80 signed byte / 2 |
| 0x07a | 0x058 | 78 | s8 | ENV2 L2 | -63..+63 | 63 |  |
| 0x07b | 0x059 | 79 | s8 | ENV2 L3 | -63..+63 | 63 |  |
| 0x07c | 0x05a | 80 | s8 | ENV2 (unknown, not in the GUI) |  | 0 | one factory program uses 1/3 |
| 0x07d | 0x05b | 81 | s8 | ENV2 LV | 0..127 | 0 | bits 0-5 amount, bit 6 X (exponential); SQ80 byte >> 2 |
| 0x07e | 0x05c | 82 | s8 | ENV2 T1V | 0..63 | 0 |  |
| 0x07f | 0x05d | 83 | s8 | ENV2 T1 | 0..63 | 0 |  |
| 0x080 | 0x05e | 84 | s8 | ENV2 T2 | 0..63 | 0 |  |
| 0x081 | 0x05f | 85 | s8 | ENV2 T3 | 0..63 | 0 |  |
| 0x082 | 0x060 | 86 | s8 | ENV2 T4 | 0..127 | 0 | bits 0-5 time, bit 6 = 2nd release (SQ80 bit 7) |
| 0x083 | 0x061 | 87 | s8 | ENV2 TK | 0..63 | 0 |  |
| 0x084 | 0x062 | 88 | bits 0:1 1:3 4:1 5:1 6:1 7:1 | ENV2 mode bits |  | 0x00 | b0 CYC, b1-3 SHAPE (OFF EXP EXP2 EXP3 EXP4 TAN TAN2 TAN3), b4 T1V mode (T1/SMT) |
| 0x085 | 0x063 | 94 | s8 | ENV2 SMTH | 0..63 | 0 |  |
| 0x086 | 0x064 | 95 | s8 | ENV3 L0 (start level) | -63..+63 | 0 | SQ8L only (0 from SysEx) |
| 0x087 | 0x065 | 96 | s8 | ENV3 L1 | -63..+63 | 63 | SQ80 signed byte / 2 |
| 0x088 | 0x066 | 97 | s8 | ENV3 L2 | -63..+63 | 63 |  |
| 0x089 | 0x067 | 98 | s8 | ENV3 L3 | -63..+63 | 63 |  |
| 0x08a | 0x068 | 99 | s8 | ENV3 (unknown, not in the GUI) |  | 0 | one factory program uses 1/3 |
| 0x08b | 0x069 | 100 | s8 | ENV3 LV | 0..127 | 0 | bits 0-5 amount, bit 6 X (exponential); SQ80 byte >> 2 |
| 0x08c | 0x06a | 101 | s8 | ENV3 T1V | 0..63 | 0 |  |
| 0x08d | 0x06b | 102 | s8 | ENV3 T1 | 0..63 | 0 |  |
| 0x08e | 0x06c | 103 | s8 | ENV3 T2 | 0..63 | 0 |  |
| 0x08f | 0x06d | 104 | s8 | ENV3 T3 | 0..63 | 0 |  |
| 0x090 | 0x06e | 105 | s8 | ENV3 T4 | 0..127 | 0 | bits 0-5 time, bit 6 = 2nd release (SQ80 bit 7) |
| 0x091 | 0x06f | 106 | s8 | ENV3 TK | 0..63 | 0 |  |
| 0x092 | 0x070 | 107 | bits 0:1 1:3 4:1 5:1 6:1 7:1 | ENV3 mode bits |  | 0x00 | b0 CYC, b1-3 SHAPE (OFF EXP EXP2 EXP3 EXP4 TAN TAN2 TAN3), b4 T1V mode (T1/SMT) |
| 0x093 | 0x071 | 113 | s8 | ENV3 SMTH | 0..63 | 0 |  |
| 0x094 | 0x072 | 114 | s8 | ENV4 L0 (start level) | -63..+63 | 0 | SQ8L only (0 from SysEx) |
| 0x095 | 0x073 | 115 | s8 | ENV4 L1 | -63..+63 | 63 | SQ80 signed byte / 2 |
| 0x096 | 0x074 | 116 | s8 | ENV4 L2 | -63..+63 | 63 |  |
| 0x097 | 0x075 | 117 | s8 | ENV4 L3 | -63..+63 | 63 |  |
| 0x098 | 0x076 | 118 | s8 | ENV4 (unknown, not in the GUI) |  | 0 | one factory program uses 1/3 |
| 0x099 | 0x077 | 119 | s8 | ENV4 LV | 0..127 | 0 | bits 0-5 amount, bit 6 X (exponential); SQ80 byte >> 2 |
| 0x09a | 0x078 | 120 | s8 | ENV4 T1V | 0..63 | 0 |  |
| 0x09b | 0x079 | 121 | s8 | ENV4 T1 | 0..63 | 0 |  |
| 0x09c | 0x07a | 122 | s8 | ENV4 T2 | 0..63 | 0 |  |
| 0x09d | 0x07b | 123 | s8 | ENV4 T3 | 0..63 | 0 |  |
| 0x09e | 0x07c | 124 | s8 | ENV4 T4 | 0..127 | 0 | bits 0-5 time, bit 6 = 2nd release (SQ80 bit 7) |
| 0x09f | 0x07d | 125 | s8 | ENV4 TK | 0..63 | 0 |  |
| 0x0a0 | 0x07e | 126 | bits 0:1 1:3 4:1 5:1 6:1 7:1 | ENV4 mode bits |  | 0x00 | b0 CYC, b1-3 SHAPE (OFF EXP EXP2 EXP3 EXP4 TAN TAN2 TAN3), b4 T1V mode (T1/SMT) |
| 0x0a1 | 0x07f | 132 | s8 | ENV4 SMTH | 0..63 | 0 |  |
| 0x0a2 | 0x080 | 133 | s8 | LFO1 FREQ | 0..127 | 24 | SQ80 0..63 |
| 0x0a3 | 0x081 | 134 | s8 | LFO1 RESET | -1..63 | -1 | -1 = OFF; SQ80 bit 7 -> 0 (reset) / -1 |
| 0x0a4 | 0x082 | 135 | s8 | LFO1 HUMAN | 0..6 | 0 | OFF ON 1x 2x 4x 8x 16x; SQ80 bit 6 |
| 0x0a5 | 0x083 | 136 | s8 | LFO1 WAVE | 0..82 | 0 | 0-4 TRI SAW SQR NOI BIP, 5-74 osc waves 00..69, 75-82 EXP EXP2 EXP3 EXP4 TAN TAN2 TAN3 TAN4; SQ80 0..3 |
| 0x0a6 | 0x084 | 137 | s8 | LFO1 L1 | 0..63 | 0 |  |
| 0x0a7 | 0x085 | 138 | bits 0:6 6:1 7:1 | LFO1 DELAY / delay mode | 0..63 / 0..1 | 0x3f | bits 0-5 DELAY, bit 6 DLYM (EMU/SMTH) |
| 0x0a8 | 0x086 | 141 | s8 | LFO1 L2 | 0..63 | 63 |  |
| 0x0a9 | 0x087 | 142 | s16 | LFO1 MOD source (amplitude) | -1..145 | -1 | SQ80 4-bit source |
| 0x0ab | 0x089 | 143 | s8 | LFO1 MOD amount | -63..+63 | 0 | 63 from SysEx |
| 0x0ac | 0x08a | 144 | s16 | LFO1 FREQMOD source | -1..145 | -1 |  |
| 0x0ae | 0x08c | 145 | s8 | LFO1 FREQMOD amount | -127..+127 | 0 |  |
| 0x0af | 0x08d | 146 | bits 0:2 2:2 4:2 6:1 7:1 | LFO1 modes |  | 0x00 | b0-1 AM mode, b2-3 FM mode (UNI BIP PHS SMT), b4-5 PLAY (FWD REV 1XF 1XR) |
| 0x0b0 | 0x08e | 151 | u8 | LFO1 PHS | 0..127 | 0 | unsigned; 64..127 = twin mode |
| 0x0b1 | 0x08f | 152 | s8 | LFO1 SMTH | 0..63 | 0 |  |
| 0x0b2 | 0x090 | 153 | s8 | LFO2 FREQ | 0..127 | 24 | SQ80 0..63 |
| 0x0b3 | 0x091 | 154 | s8 | LFO2 RESET | -1..63 | -1 | -1 = OFF; SQ80 bit 7 -> 0 (reset) / -1 |
| 0x0b4 | 0x092 | 155 | s8 | LFO2 HUMAN | 0..6 | 0 | OFF ON 1x 2x 4x 8x 16x; SQ80 bit 6 |
| 0x0b5 | 0x093 | 156 | s8 | LFO2 WAVE | 0..82 | 0 | 0-4 TRI SAW SQR NOI BIP, 5-74 osc waves 00..69, 75-82 EXP EXP2 EXP3 EXP4 TAN TAN2 TAN3 TAN4; SQ80 0..3 |
| 0x0b6 | 0x094 | 157 | s8 | LFO2 L1 | 0..63 | 0 |  |
| 0x0b7 | 0x095 | 158 | bits 0:6 6:1 7:1 | LFO2 DELAY / delay mode | 0..63 / 0..1 | 0x3f | bits 0-5 DELAY, bit 6 DLYM (EMU/SMTH) |
| 0x0b8 | 0x096 | 161 | s8 | LFO2 L2 | 0..63 | 63 |  |
| 0x0b9 | 0x097 | 162 | s16 | LFO2 MOD source (amplitude) | -1..145 | -1 | SQ80 4-bit source |
| 0x0bb | 0x099 | 163 | s8 | LFO2 MOD amount | -63..+63 | 0 | 63 from SysEx |
| 0x0bc | 0x09a | 164 | s16 | LFO2 FREQMOD source | -1..145 | -1 |  |
| 0x0be | 0x09c | 165 | s8 | LFO2 FREQMOD amount | -127..+127 | 0 |  |
| 0x0bf | 0x09d | 166 | bits 0:2 2:2 4:2 6:1 7:1 | LFO2 modes |  | 0x00 | b0-1 AM mode, b2-3 FM mode (UNI BIP PHS SMT), b4-5 PLAY (FWD REV 1XF 1XR) |
| 0x0c0 | 0x09e | 171 | u8 | LFO2 PHS | 0..127 | 0 | unsigned; 64..127 = twin mode |
| 0x0c1 | 0x09f | 172 | s8 | LFO2 SMTH | 0..63 | 0 |  |
| 0x0c2 | 0x0a0 | 173 | s8 | LFO3 FREQ | 0..127 | 24 | SQ80 0..63 |
| 0x0c3 | 0x0a1 | 174 | s8 | LFO3 RESET | -1..63 | -1 | -1 = OFF; SQ80 bit 7 -> 0 (reset) / -1 |
| 0x0c4 | 0x0a2 | 175 | s8 | LFO3 HUMAN | 0..6 | 0 | OFF ON 1x 2x 4x 8x 16x; SQ80 bit 6 |
| 0x0c5 | 0x0a3 | 176 | s8 | LFO3 WAVE | 0..82 | 0 | 0-4 TRI SAW SQR NOI BIP, 5-74 osc waves 00..69, 75-82 EXP EXP2 EXP3 EXP4 TAN TAN2 TAN3 TAN4; SQ80 0..3 |
| 0x0c6 | 0x0a4 | 177 | s8 | LFO3 L1 | 0..63 | 0 |  |
| 0x0c7 | 0x0a5 | 178 | bits 0:6 6:1 7:1 | LFO3 DELAY / delay mode | 0..63 / 0..1 | 0x3f | bits 0-5 DELAY, bit 6 DLYM (EMU/SMTH) |
| 0x0c8 | 0x0a6 | 181 | s8 | LFO3 L2 | 0..63 | 63 |  |
| 0x0c9 | 0x0a7 | 182 | s16 | LFO3 MOD source (amplitude) | -1..145 | -1 | SQ80 4-bit source |
| 0x0cb | 0x0a9 | 183 | s8 | LFO3 MOD amount | -63..+63 | 0 | 63 from SysEx |
| 0x0cc | 0x0aa | 184 | s16 | LFO3 FREQMOD source | -1..145 | -1 |  |
| 0x0ce | 0x0ac | 185 | s8 | LFO3 FREQMOD amount | -127..+127 | 0 |  |
| 0x0cf | 0x0ad | 186 | bits 0:2 2:2 4:2 6:1 7:1 | LFO3 modes |  | 0x00 | b0-1 AM mode, b2-3 FM mode (UNI BIP PHS SMT), b4-5 PLAY (FWD REV 1XF 1XR) |
| 0x0d0 | 0x0ae | 191 | u8 | LFO3 PHS | 0..127 | 0 | unsigned; 64..127 = twin mode |
| 0x0d1 | 0x0af | 192 | s8 | LFO3 SMTH | 0..63 | 0 |  |
| 0x0d2 | 0x0b0 | 193 | s8 | LFO4 FREQ | 0..127 | 24 | SQ80 0..63 |
| 0x0d3 | 0x0b1 | 194 | s8 | LFO4 RESET | -1..63 | -1 | -1 = OFF; SQ80 bit 7 -> 0 (reset) / -1 |
| 0x0d4 | 0x0b2 | 195 | s8 | LFO4 HUMAN | 0..6 | 0 | OFF ON 1x 2x 4x 8x 16x; SQ80 bit 6 |
| 0x0d5 | 0x0b3 | 196 | s8 | LFO4 WAVE | 0..82 | 0 | 0-4 TRI SAW SQR NOI BIP, 5-74 osc waves 00..69, 75-82 EXP EXP2 EXP3 EXP4 TAN TAN2 TAN3 TAN4; SQ80 0..3 |
| 0x0d6 | 0x0b4 | 197 | s8 | LFO4 L1 | 0..63 | 0 |  |
| 0x0d7 | 0x0b5 | 198 | bits 0:6 6:1 7:1 | LFO4 DELAY / delay mode | 0..63 / 0..1 | 0x3f | bits 0-5 DELAY, bit 6 DLYM (EMU/SMTH) |
| 0x0d8 | 0x0b6 | 201 | s8 | LFO4 L2 | 0..63 | 63 |  |
| 0x0d9 | 0x0b7 | 202 | s16 | LFO4 MOD source (amplitude) | -1..145 | -1 | SQ80 4-bit source |
| 0x0db | 0x0b9 | 203 | s8 | LFO4 MOD amount | -63..+63 | 0 | 63 from SysEx |
| 0x0dc | 0x0ba | 204 | s16 | LFO4 FREQMOD source | -1..145 | -1 |  |
| 0x0de | 0x0bc | 205 | s8 | LFO4 FREQMOD amount | -127..+127 | 0 |  |
| 0x0df | 0x0bd | 206 | bits 0:2 2:2 4:2 6:1 7:1 | LFO4 modes |  | 0x00 | b0-1 AM mode, b2-3 FM mode (UNI BIP PHS SMT), b4-5 PLAY (FWD REV 1XF 1XR) |
| 0x0e0 | 0x0be | 211 | u8 | LFO4 PHS | 0..127 | 0 | unsigned; 64..127 = twin mode |
| 0x0e1 | 0x0bf | 212 | s8 | LFO4 SMTH | 0..63 | 0 |  |
| 0x0e2 | 0x0c0 | 213 | s16 | MAT1 M1 source | -1..145 | -1 |  |
| 0x0e4 | 0x0c2 | 214 | s8 | MAT1 M1 amount | -63..+63 | 0 |  |
| 0x0e5 | 0x0c3 | 215 | s16 | MAT1 M2 source | -1..145 | -1 |  |
| 0x0e7 | 0x0c5 | 216 | s8 | MAT1 M2 amount | -63..+63 | 0 |  |
| 0x0e8 | 0x0c6 | 217 | s16 | MAT1 M3 source | -1..145 | -1 |  |
| 0x0ea | 0x0c8 | 218 | s8 | MAT1 M3 amount | -63..+63 | 0 |  |
| 0x0eb | 0x0c9 | 219 | s16 | MAT1 AMP source | -1..145 | -1 |  |
| 0x0ed | 0x0cb | 220 | s8 | MAT1 AMP amount | -63..+63 | 0 |  |
| 0x0ee | 0x0cc | 221 | s16 | MAT2 M1 source | -1..145 | -1 |  |
| 0x0f0 | 0x0ce | 222 | s8 | MAT2 M1 amount | -63..+63 | 0 |  |
| 0x0f1 | 0x0cf | 223 | s16 | MAT2 M2 source | -1..145 | -1 |  |
| 0x0f3 | 0x0d1 | 224 | s8 | MAT2 M2 amount | -63..+63 | 0 |  |
| 0x0f4 | 0x0d2 | 225 | s16 | MAT2 M3 source | -1..145 | -1 |  |
| 0x0f6 | 0x0d4 | 226 | s8 | MAT2 M3 amount | -63..+63 | 0 |  |
| 0x0f7 | 0x0d5 | 227 | s16 | MAT2 AMP source | -1..145 | -1 |  |
| 0x0f9 | 0x0d7 | 228 | s8 | MAT2 AMP amount | -63..+63 | 0 |  |
| 0x0fa | 0x0d8 | 229 | s16 | MAT3 M1 source | -1..145 | -1 |  |
| 0x0fc | 0x0da | 230 | s8 | MAT3 M1 amount | -63..+63 | 0 |  |
| 0x0fd | 0x0db | 231 | s16 | MAT3 M2 source | -1..145 | -1 |  |
| 0x0ff | 0x0dd | 232 | s8 | MAT3 M2 amount | -63..+63 | 0 |  |
| 0x100 | 0x0de | 233 | s16 | MAT3 M3 source | -1..145 | -1 |  |
| 0x102 | 0x0e0 | 234 | s8 | MAT3 M3 amount | -63..+63 | 0 |  |
| 0x103 | 0x0e1 | 235 | s16 | MAT3 AMP source | -1..145 | -1 |  |
| 0x105 | 0x0e3 | 236 | s8 | MAT3 AMP amount | -63..+63 | 0 |  |
| 0x106 | 0x0e4 | 237 | s8 | FILTER FREQ | 0..127 | 127 |  |
| 0x107 | 0x0e5 | 238 | s8 | FILTER RES | 0..31 | 0 |  |
| 0x108 | 0x0e6 | 239 | s8 | FILTER KEYBD | -63..+63 | 0 | SQ80 0..63 ((byte & 0x7f) >> 1) |
| 0x109 | 0x0e7 | 240 | s8 | FILTER (unused) |  | 0 |  |
| 0x10a | 0x0e8 | 241 | s16 | FILTER MOD1 source | -1..145 | -1 |  |
| 0x10c | 0x0ea | 242 | s8 | FILTER MOD1 amount | -127..+127 | 0 | SQ80 signed 7 bit, not halved |
| 0x10d | 0x0eb | 243 | s16 | FILTER MOD2 source | -1..145 | -1 |  |
| 0x10f | 0x0ed | 244 | s8 | FILTER MOD2 amount | -127..+127 | 0 |  |
| 0x110 | 0x0ee | 245 | s8 | FILTER (unused) |  | 0 |  |
| 0x111 | 0x0ef | 246 | s16 | FILTER MOD3 source (not in the GUI) |  | -1 |  |
| 0x113 | 0x0f1 | 247 | s8 | FILTER MOD3 amount |  | 0 |  |
| 0x114 | 0x0f2 | 248 | s8 | filter 2 (unused) FREQ | 0..127 | 127 |  |
| 0x115 | 0x0f3 | 249 | s8 | filter 2 (unused) RES | 0..31 | 0 |  |
| 0x116 | 0x0f4 | 250 | s8 | filter 2 (unused) KEYBD | -63..+63 | 0 | SQ80 0..63 ((byte & 0x7f) >> 1) |
| 0x117 | 0x0f5 | 251 | s8 | filter 2 (unused) (unused) |  | 0 |  |
| 0x118 | 0x0f6 | 252 | s16 | filter 2 (unused) MOD1 source | -1..145 | -1 |  |
| 0x11a | 0x0f8 | 253 | s8 | filter 2 (unused) MOD1 amount | -127..+127 | 0 | SQ80 signed 7 bit, not halved |
| 0x11b | 0x0f9 | 254 | s16 | filter 2 (unused) MOD2 source | -1..145 | -1 |  |
| 0x11d | 0x0fb | 255 | s8 | filter 2 (unused) MOD2 amount | -127..+127 | 0 |  |
| 0x11e | 0x0fc | 256 | s8 | filter 2 (unused) (unused) |  | 0 |  |
| 0x11f | 0x0fd | 257 | s16 | filter 2 (unused) MOD3 source (not in the GUI) |  | -1 |  |
| 0x121 | 0x0ff | 258 | s8 | filter 2 (unused) MOD3 amount |  | 0 |  |
| 0x122 | 0x100 | 259 | s8 | block 0x122 (not in the GUI) |  | 0 | filled only by the pre-0.90 import |
| 0x123 | 0x101 | 260 | s8 | block 0x122 +1 |  | 0 |  |
| 0x124 | 0x102 | 261 | s16 | block 0x122 slot 1 source |  | -1 |  |
| 0x126 | 0x104 | 262 | s8 | block 0x122 slot 1 amount |  | 0 |  |
| 0x127 | 0x105 | 263 | s16 | block 0x122 slot 2 source |  | -1 |  |
| 0x129 | 0x107 | 264 | s8 | block 0x122 slot 2 amount |  | 0 |  |
| 0x12a | 0x108 | 265 | s8 | (unused) |  | 0 |  |
| 0x12b | 0x109 | 266 | s8 | (unused) |  | 0 |  |
| 0x12c | 0x10a | 267 | s8 | (unused) |  | 0 |  |
| 0x12d | 0x10b | 268 | s8 | (unused) |  | 0 |  |
| 0x12e | 0x10c | 269 | s8 | (unused) |  | 0 |  |
| 0x12f | 0x10d | 270 | s8 | (unused) |  | 0 |  |
| 0x130 | 0x10e | 271 | s8 | (unused) |  | 0 |  |
| 0x131 | 0x10f | 272 | s8 | (unused) |  | 0 |  |
| 0x132 | 0x110 | 273 | s8 | DCA4 ENV4.AMT (FINAL) | 0..63 | 63 | SQ80 (byte & 0x7f) >> 1; bank D: -12 |
| 0x133 | 0x111 | 274 | s8 | DCA4 PAN | -63..+63 | 0 | SQ80 nybble 0..15: 0 -> -63, 15 -> 63, else (n-8)*8 |
| 0x134 | 0x112 | 275 | s16 | DCA4 AMOD source | -1..145 | -1 |  |
| 0x136 | 0x114 | 276 | s8 | DCA4 AMOD amount | -63..+63 | 0 |  |
| 0x137 | 0x115 | 277 | s16 | PAN MOD source | -1..145 | -1 |  |
| 0x139 | 0x117 | 278 | s8 | PAN MOD amount | -127..+127 | 0 | SQ80 signed 7 bit |
| 0x13a | 0x118 | 279 | s8 | SAT (saturation) | -1..15 | 0 | -1 OFF, 0 EMU, 1..15; voice: amp FUN_0045e89c |
| 0x13b | 0x119 | 280 | bits 0:2 2:1 3:1 4:1 5:1 6:1 7:1 | emulation bits 2 |  | 0x00 | b0-1 ?, b2 MUFFLE (OFF/ON), b3-7 ? |
| 0x13c | 0x11a | 287 | s8 | filter key mode |  | 0 | value-1 -> filter setKeyParam (FUN_0045f560); 0 in all sounds |
| 0x13d | 0x11b | 288 | s8 | (unused) |  | 0 |  |
| 0x13e | 0x11c | 289 | s8 | (unused) |  | 0 |  |
| 0x13f | 0x11d | 290 | s8 | (unused) |  | 0 |  |
| 0x140 | 0x11e | 291 | s16 | block 0x140+0x0: s16 #0 |  | 0 | not in the GUI |
| 0x142 | 0x120 | 292 | s16 | block 0x140+0x0: s16 #1 |  | 0 | not in the GUI |
| 0x144 | 0x122 | 293 | s16 | block 0x140+0x0: s16 #2 |  | 0 | not in the GUI |
| 0x146 | 0x124 | 294 | s16 | block 0x140+0x0: s16 #3 |  | 0 | not in the GUI |
| 0x148 | 0x126 | 295 | s16 | block 0x140+0x0: s16 #4 |  | 0 | not in the GUI |
| 0x14a | 0x128 | 296 | s8 | block 0x140+0x0: +0xa |  | 1 | INIT 1 |
| 0x14b | 0x129 | 297 | s8 | block 0x140+0x0: +0xb |  | 0 |  |
| 0x14c | 0x12a | 298 | s8 | block 0x140+0x0: +0xc |  | 0 |  |
| 0x14d | 0x12b | 299 | s8 | block 0x140+0x0: +0xd |  | 0 |  |
| 0x14e | 0x12c | 300 | s8 | block 0x140+0x0: +0xe |  | 0 |  |
| 0x14f | 0x12d | 301 | s8 | block 0x140+0x0: +0xf |  | 0 |  |
| 0x150 | 0x12e | 302 | s16 | block 0x140+0x10: s16 #0 |  | 0 | not in the GUI |
| 0x152 | 0x130 | 303 | s16 | block 0x140+0x10: s16 #1 |  | 0 | not in the GUI |
| 0x154 | 0x132 | 304 | s16 | block 0x140+0x10: s16 #2 |  | 0 | not in the GUI |
| 0x156 | 0x134 | 305 | s16 | block 0x140+0x10: s16 #3 |  | 0 | not in the GUI |
| 0x158 | 0x136 | 306 | s16 | block 0x140+0x10: s16 #4 |  | 0 | not in the GUI |
| 0x15a | 0x138 | 307 | s8 | block 0x140+0x10: +0xa |  | 1 | INIT 1 |
| 0x15b | 0x139 | 308 | s8 | block 0x140+0x10: +0xb |  | 0 |  |
| 0x15c | 0x13a | 309 | s8 | block 0x140+0x10: +0xc |  | 0 |  |
| 0x15d | 0x13b | 310 | s8 | block 0x140+0x10: +0xd |  | 0 |  |
| 0x15e | 0x13c | 311 | s8 | block 0x140+0x10: +0xe |  | 0 |  |
| 0x15f | 0x13d | 312 | s8 | block 0x140+0x10: +0xf |  | 0 |  |
| 0x160 | 0x13e | 313 | s16 | block 0x140+0x20: s16 #0 |  | 0 | not in the GUI |
| 0x162 | 0x140 | 314 | s16 | block 0x140+0x20: s16 #1 |  | 0 | not in the GUI |
| 0x164 | 0x142 | 315 | s16 | block 0x140+0x20: s16 #2 |  | 0 | not in the GUI |
| 0x166 | 0x144 | 316 | s16 | block 0x140+0x20: s16 #3 |  | 0 | not in the GUI |
| 0x168 | 0x146 | 317 | s16 | block 0x140+0x20: s16 #4 |  | 0 | not in the GUI |
| 0x16a | 0x148 | 318 | s8 | block 0x140+0x20: +0xa |  | 1 | INIT 1 |
| 0x16b | 0x149 | 319 | s8 | block 0x140+0x20: +0xb |  | 0 |  |
| 0x16c | 0x14a | 320 | s8 | block 0x140+0x20: +0xc |  | 0 |  |
| 0x16d | 0x14b | 321 | s8 | block 0x140+0x20: +0xd |  | 0 |  |
| 0x16e | 0x14c | 322 | s8 | block 0x140+0x20: +0xe |  | 0 |  |
| 0x16f | 0x14d | 323 | s8 | block 0x140+0x20: +0xf |  | 0 |  |
| 0x170 | 0x14e | 324 | s8 | SYNC | 0..1 | 0 | SQ80 bit 7 of filter FREQ byte |
| 0x171 | 0x14f | 325 | s8 | AM (XMOD) | 0..1 | 0 | SQ80 bit 7 of DCA4 byte; import forces OSC1 off/63, OSC2 on/63 |
| 0x172 | 0x150 | 326 | s8 | MONO | 0..1 | 0 | SQ80 bit 7 of filter MOD2 amount byte |
| 0x173 | 0x151 | 327 | s8 | GLIDE | 0..63 | 0 |  |
| 0x174 | 0x152 | 328 | s8 | RESTART VC | 0..1 | 0 | SQ80 bit 7 of filter MOD1 amount byte |
| 0x175 | 0x153 | 329 | s8 | RESTART ENV | 0..1 | 0 | SQ80 bit 7 of KEYBD byte |
| 0x176 | 0x154 | 330 | s8 | RESTART OSC | 0..1 | 0 | SQ80 bit 7 of GLIDE byte |
| 0x177 | 0x155 | 331 | s8 | CYC (all envelopes) | 0..1 | 0 | SQ80 bit 7 of pan mod amount byte |
| 0x178 | 0x156 | 332 | s8 | (not in the GUI) |  | 0 |  |
| 0x179 | 0x157 | 333 | s8 | (not in the GUI) |  | 0 |  |
| 0x17a | 0x158 | 334 | s8 | (not in the GUI) |  | 0 |  |
| 0x17b | 0x159 | 335 | s8 | (not in the GUI) |  | 0 |  |
| 0x17c | 0x15a | 336 | s8 | (not in the GUI) |  | 0 |  |
| 0x17d | 0x15b | 337 | s8 | (not in the GUI) |  | 0 |  |
| 0x17e | 0x15c | 338 | s8 | (not in the GUI) |  | 0 |  |
| 0x17f | 0x15d | 339 | s8 | (not in the GUI) |  | 0 |  |
| 0x180 | 0x15e | 340 | s8 | (not in the GUI) |  | 0 |  |
| 0x181 | 0x15f | 341 | s8 | (not in the GUI) |  | 0 |  |
| 0x182 | 0x160 | 342 | s8 | (not in the GUI) |  | 4 | INIT 4 |
| 0x183 | 0x161 | 343 | s8 | (not in the GUI) |  | 16 | INIT 16 |
| 0x184 | 0x162 | 344 | s8 | (not in the GUI) |  | 0 |  |
| 0x185 | 0x163 | 345 | s8 | (not in the GUI) |  | 0 |  |
| 0x186 | 0x164 | 346 | s8 | (not in the GUI) |  | 0 |  |
| 0x187 | 0x165 | 347 | s8 | (not in the GUI) |  | 0 |  |
| 0x188 | 0x166 | 348 | s8 | (not in the GUI) |  | 0 |  |
| 0x189 | 0x167 | 349 | s8 | (not in the GUI) |  | 0 |  |
| 0x18a | 0x168 | 350 | s8 | (not in the GUI) |  | 0 |  |
| 0x18b | 0x169 | 351 | s8 | (not in the GUI) |  | 0 |  |
| 0x18c | 0x16a | 352 | s8 | (not in the GUI) |  | 0 |  |
| 0x18d | 0x16b | 353 | s8 | (not in the GUI) |  | 0 |  |
| 0x18e | 0x16c | 354 | s8 | (not in the GUI) |  | 60 | INIT 60 |
| 0x18f | 0x16d | 355 | s8 | (not in the GUI) |  | 0 |  |
| 0x190 | 0x16e | 356 | s8 | BEND range | 0..36 | 2 | voice: * x >> 5 |
| 0x191 | 0x16f | 357 | s8 | BEND mode | 0..6 | 0 | ALL HELD NEW NEWH HELD2 NEW2 NEWH2 |
| 0x192 | 0x170 | 358 | bits 0:1 1:2 3:2 5:1 6:1 7:1 | emulation bits |  | 0x01 | b0 AMBUG, b1-2 DCA1-3 smoothing (EMU/FAST), b3-4 DC-BLOCK (SMART/ON/OFF) |
| 0x193 | 0x171 | 364 | s8 | (not in the GUI) |  | 0 |  |
| 0x194 | 0x172 | 365 | s8 | (not in the GUI) |  | 0 |  |
| 0x195 | 0x173 | 366 | s8 | VSTEAL | -1..0 | 0 | -1 HARD, 0 SOFT |
| 0x196 | 0x174 | 367 | bits 0:1 1:1 2:1 3:1 4:1 5:1 6:1 7:1 | emulation bits 3 |  | 0x00 | b0 voice flag (+0x70 of the voice), b1 DCA4 smoothing (EMU/HARD) |
| 0x197 | 0x175 | 375 | u8 | VOICES (port addition) | 1..64 | 0 | playable voices, EMU page; 0 (the original, every import) = the original's 8, see below |
| 0x198 | 0x176 | 376 | s8 | (not in the GUI) |  | 0 |  |
| 0x199 | 0x177 | 377 | s8 | (not in the GUI) |  | 0 |  |
| 0x19a | 0x178 | 378 | s8 | (not in the GUI) |  | 0 |  |
| 0x19b | 0x179 | 379 | s8 | (not in the GUI) |  | 0 |  |
| 0x19c | 0x17a | 380 | s8 | (not in the GUI) |  | 0 |  |
| 0x19d | 0x17b | 381 | s8 | (not in the GUI) |  | 0 |  |
| 0x19e | 0x17c | 382 | s8 | (not in the GUI) |  | 0 |  |
| 0x19f | 0x17d | 383 | s8 | (not in the GUI) |  | 0 |  |
| 0x1a0 | 0x17e | 384 | u8 | (not in the GUI) |  | 120 | u8, INIT 120 |
| 0x1a1 | 0x17f | 385 | s8 | (not in the GUI) |  | 2 | INIT 2 |
| 0x1a2 | 0x180 | 386 | s8 | (not in the GUI) |  | 0 |  |
| 0x1a3 | 0x181 | 387 | s8 | (not in the GUI) |  | 32 | INIT 32 |
| 0x1a4 | 0x182 | 388 | s8 | (not in the GUI) |  | 0 |  |
| 0x1a5 | 0x183 | 389 | s8 | (not in the GUI) |  | 0 |  |
| 0x1a6 | 0x184 | 390 | s8 | (not in the GUI) |  | 0 |  |
| 0x1a7 | 0x185 | 391 | s8 | (not in the GUI) |  | 0 |  |
| 0x1a8 | 0x186 | 392 | s16 | block 0x1a6 slot 1 source |  | -1 |  |
| 0x1aa | 0x188 | 393 | s8 | (not in the GUI) |  | 0 |  |
| 0x1ab | 0x189 | 394 | s16 | block 0x1a6 slot 2 source |  | -1 |  |
| 0x1ad | 0x18b | 395 | s8 | (not in the GUI) |  | 0 |  |
| 0x1ae | 0x18c | 396 | s8 | (not in the GUI) |  | 0 |  |
| 0x1af | 0x18d | 397 | s8 | (not in the GUI) |  | 0 |  |

### Bit field details

| byte | bits | meaning | values |
|---|---|---|---|
| ENV +0x0c | 0 | CYC (this envelope has no sustain) | 0 OFF, 1 ON |
| | 1-3 | SHAPE | OFF EXP EXP2 EXP3 EXP4 TAN TAN2 TAN3 |
| | 4 | T1V mode | 0 T1 (velocity -> attack time), 1 SMT (velocity -> smoothing) |
| LFO +0x05 | 0-5 | DELAY | 0..63 |
| | 6 | delay mode | 0 EMU, 1 SMTH |
| LFO +0x0d | 0-1 | AM mode (MOD) | UNI BIP PHS SMT |
| | 2-3 | FM mode (FREQMOD) | UNI BIP PHS SMT |
| | 4-5 | PLAY | FWD REV 1XF 1XR |
| 0x13b | 2 | MUFFLE | 0 OFF, 1 ON (overridable, see settings.md) |
| 0x192 | 0 | AMBUG | 0 OFF, 1 ON |
| | 1-2 | DCA1-3 smoothing | 0 EMU, 1 FAST |
| | 3-4 | DC-BLOCK | 0 SMART, 1 ON, 2 OFF |
| 0x196 | 0 | read by the voice code into voice+0x70 (not in the GUI) | |
| | 1 | DCA4 smoothing | 0 EMU, 1 HARD |

Other value lists: LFO WAVE 0-4 TRI SAW SQR NOI BIP, 5-74 oscillator waves 00..69,
75-82 EXP EXP2 EXP3 EXP4 TAN TAN2 TAN3 TAN4 (formatter 0x4612f0); LFO HUMAN OFF ON X1 X2 X4
X8 X16; ENV LV bit 6 = "X" (exponential), bits 0-5 amount; ENV T4 bit 6 = 2nd release ("R");
SAT -1 OFF, 0 EMU, 1..15 "+n"; VSTEAL -1 HARD, 0 SOFT; BEND mode ALL HELD NEW NEWH HELD2 NEW2
NEWH2.

## Extension block (0x1b0..0x21b)

| off | content |
|---|---|
| 0x1b0 | 8 bytes, 0 |
| 0x1b8 | zone 0 (0x32 bytes): +1 = 0xff after INIT, +0x17 9 mod slots |
| 0x1ea | zone 1 (0x32 bytes), same layout |

The edit buffer keeps a copy (`EditBuffer::ext()`, CeditBuf+0xa98); `resetExtZone(z)`
(FUN_004605ac -> FUN_004539dc) clears bytes +2..+4 and the 9 slots (to 0, not OFF) and sets
the word at +5 to 0x40. Bank C programs were saved by an older version and carry a different
zone pattern (slots shifted by one byte); the data is kept as is.

## Fields used by the voice code (plugCore)

A voice keeps a pointer to the program it plays (voice slot +0x24 = the current edit buffer
program, ring slot). Notable reads found so far: envelopes at 0x6a+14*i, LFOs at
0xa2+16*i, oscillator/DCA blocks, filter block 0x106, 0x13a (SAT -> amp FUN_0045e89c),
0x13c-1 (filter setKeyParam FUN_0045f560), 0x170..0x177 (modes), 0x190 (bend range,
`* x >> 5`), 0x192/0x196/0x13b bits (emulation modes, with the SQ8L.ini overrides),
0x195 (VSTEAL).

## SysEx (SQ80 / ESQ-1) mapping

C++: `src/engine/SysEx.{h,cpp}`; details in `docs/modules/library.md`. A 102-byte ESQ-1
program (204 nybbles, low nybble first) maps to the record as follows (byte offsets in the
102-byte image; "nyb" import = FUN_00454054, export = FUN_004545dc, raw-byte import used for
bank D = FUN_00454aec):

| SQ80 bytes | fields |
|---|---|
| 0-5 | name (6 chars; '!' '#' '%' '(' ')' ':' ';' '[' '\\' ']' = digits 0-9 with decimal point, exported/imported as "0.".."9."; spaces are dropped on export; chars outside 0x20..0x5f dropped) |
| 6 + 10*e | ENV e+1: L1 L2 L3 (signed, /2 rounding toward 0; export *2), T1..T4 (6 bit; T4 bit 7 = 2nd release -> bit 6), LV (>>2), T1V, TK |
| 0x2e + 4*l | LFO l+1 (3 LFOs): b0 = FREQ(6) + WAVE(2); b1 = L1(6) + mod source bits 3-2; b2 = L2(6) + mod source bits 1-0; b3 = DELAY(6) + HUMAN(bit 6) + RESET(bit 7) |
| 0x3a + 10*o | OSC o+1: b0 = octave*12+semi (7 bit), b1 = FINE<<3, b2 = pitch mod sources (lo/hi nybble), b3/b4 = pitch mod amounts (signed /2), b5 = WAVE, b6 = DCA LEVEL<<1 + OUTPUT bit 7, b7 = DCA mod sources, b8/b9 = DCA mod amounts |
| 0x58 | DCA4 level<<1 (bit 7 AM) |
| 0x59..0x5f | FILTER FREQ (bit 7 SYNC), RES (5 bit), mod sources, MOD1 amount (bit 7 RESTART VC), MOD2 amount (bit 7 MONO), KEYBD<<1 (bit 7 RESTART ENV), GLIDE (bit 7 RESTART OSC) |
| 0x64 | pan mod source (lo nybble), PAN (hi nybble) |
| 0x65 | pan mod amount (signed 7 bit; bit 7 CYC) |

Mod sources map SQ80 -> SQ8L (`sourceFromSq80`, FUN_00453b14): LFO1-3 -> 0-2, ENV1-4 -> 4-7,
VEL -> 8, VEL2 -> 9 (VEL-X), KYBD -> 10, KYBD2 -> 11, WHEEL -> 17 (CC1), PEDAL -> 52 (CC36,
sic), XCTRL -> 18 (CC2 breath), PRESS -> 12, 15 -> OFF. Back (`sourceToSq80`, FUN_00453ba0):
LFO4 and every source without an SQ80 equivalent -> 15 (OFF); 12 and 144 -> PRESS.
Quirks reproduced: the import starts from INIT, ENV L0 = 0, LFO MOD amount = 63, LFO4 and
filter 2 untouched; when AM is on, OSC1 is disabled and OSC1/OSC2 DCA levels are forced to
63 with OSC2 enabled; the export clears AM when SYNC is on, clamps filter KEYBD to 0..63 and
amounts to +-63, converts negative FINE by borrowing one semitone. The raw-byte import used
for bank D does *not* sign-extend the filter and pan mod amounts (values 64..127 for
negative amounts) and subtracts 12 from the DCA4 level.

## Pre-0.90 programs (0x220 bytes)

`convertOldProgram` (FUN_00453c64) converts the old record (most byte fields stored as
16-bit words, 4-byte mod slots, name ShortString at +0x202, version word 1 at +0x200) into
the current layout, starting from INIT: osc blocks at 16*o (+0x30 for the DCA part), env at
0x60+24*e, 4 LFOs at 0xc0+24*l (RESET = old - 1), 3 matrix slots at 0x120 -> 0xe2, filters at
0x150 (+16) with mod amounts doubled, 0x170 -> 0x122 block, 0x180 -> DCA4 (pan mod amount
doubled), 0x1b6 -> SAT, 0x190.. -> modes, 0x1a0.. -> 0x188.., 0x1ae.. -> 0x190 (BEND range,
mode, AMBUG bit). Verified on 400 random records.

## VOICES (port addition, 0x197)

The playable voices of the program (EMU page, `ofs::Polyphony`, parameter 375,
`programPolyphony`): 1..64, with **0 meaning the original's 8**. The byte is one the original
never writes — "not in the GUI", always 0 — so every program that predates the port reads as
8 without needing a version anywhere in the file formats:

* programs made by the SQ8L, and every library (`*.8XL`), bank and `SQ8L_backup.dat` file
  holding them: 0;
* `initProgram` zeroes the record, so INIT and everything built from it is 0;
* the SysEx import starts from INIT and maps only the SQ80's fields, so an imported program
  or bank is 0 — the SQ80 had 8 voices;
* `convertOldProgram` also starts from INIT: pre-0.90 records are 0;
* host chunks are the 540-byte record, so the value travels with a project; an old chunk has
  0 there. A value outside 1..64 is treated as 0 (no version of the port writes one).

The original preserves the byte: it copies whole records in and out of its library and files
and never touches the bytes it has no page for, so a program edited and written by the
original keeps the voices the port stored. The one path that cannot carry it is the SysEx
*export*, whose 204 nybbles are the SQ80's fixed layout.

Nothing else in a file format changed, and a program asking for 8 voices (0, or an explicit
8) renders bit-exactly like the original — see *More polyphony* in `master.md` for the engine
side. `OPTIONS -> Polyphony` overrides the parameter for one plugin instance and is not part
of a program: it travels in two spare bytes of the host chunk header (`library.md`).
