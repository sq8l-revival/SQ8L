# Settings (SQ8L.ini)

C++: `src/engine/Settings.h` (`struct Settings`). Test: `tests/test_settings.py`.

Original: `CplugConfig` (unit plugConfig 0x45269c..0x452db4, size 76), a `CglobalData`
singleton (global pointer at 0x489bf8) created when the DLL is loaded and destroyed at unload.

* Load (`CplugConfig_v005`): file `<dll dir>\SQ8L\SQ8L.ini`. If it does not exist, an empty
  file is created. Every value is read with `TIniFile.ReadInteger` (= StrToIntDef of the
  profile string): 6 `[gui]` values into +0x20.., 5 `[synth]` values into +0x38...
* Save (`CplugConfig_v006`, at DLL unload): only if the file exists, `WriteInteger` of all 11
  values, then `UpdateFile`.
* Changing a value (`FUN_00452be8` gui bool, `FUN_00452cac` synth int) notifies listeners
  with the index (gui) or 0x10 + index (synth). The master listener (`0x462004`) re-reads the
  [synth] values into master+0xfe4.. (`FUN_004620e4`) on 0x10..0x14.

Integer parsing (Delphi 5 `_ValLong`, verified against the original): optional blanks; then
either an optional sign and decimal digits (-2147483648..2147483647) or a hex number
introduced by `$`, `x`, `X` or `0x` (up to `$FFFFFFFF`, wrapping to negative; no sign
allowed before a hex prefix); anything else gives the default. Keys and sections are case
insensitive on Windows (the C++ parser too).

## [gui] (editor only)

| # | key | default | meaning |
|---|---|---|---|
| 0 | restMouseMenu | 1 | mouse position restored after popup menus (> 0 = on) |
| 1 | restMouseKnob | 1 | (port) hide the cursor while a knob is turned (issue #25); the original restored the mouse position instead. The key keeps its name so that an ini shared with the original still round-trips |
| 2 | keyCaptMode | 1 | keyboard capture while editing the name: -1 off, 0, 1 (readme E.10) |
| 3 | compareOnWrite | 1 | COMPARE checked in the WRITE dialog |
| 4 | swapProgUpDn | 0 | swap the program up/down arrow buttons |
| 5 | rmbScrollDisplay | 1 | right click on the display scrolls the page |

## [synth] (global overrides of program parameters)

Value 0 = "set by program". **The key names do not match their meaning**: the options menu
(`menu_emuModeClick`, tag = index * 16 + value) and the voice code use the index, the ini key
names are shifted. Meaning by index:

| # | ini key | master field | meaning | values |
|---|---|---|---|---|
| 0 | voiceStealMode | +0xfe4 | voice stealing | 0 program VSTEAL (0x195), 1 HARD, 2 SOFT (default) |
| 1 | muffleMode | +0xfec | DCA1-3 smoothing | 0 program (0x192 bits 1-2), 1 EMU, 2 FAST |
| 2 | oscDcaMode | +0xff0 | DCA4 smoothing | 0 program (0x196 bit 1), 1 EMU, 2 HARD |
| 3 | dca4Mode | +0xfe8 | muffle filter | 0 program (0x13b bit 2), 1 OFF, 2 ON |
| 4 | dcbMode | +0xff4 | DC blocking filter | 0 program (0x192 bits 3-4), 1 SMART, 2 ON, 3 OFF |

Effective values as computed in plugCore (helpers in `Settings`, derived from the asm, not
differentially tested since they live in the voice code):

* soft voice stealing (FUN_00463388): `v = s0 > 0 ? s0 - 2 : prog[0x195]`; soft unless v == -1;
* DCA1-3 mode stored in the voice (0x463da6): `s1 > 0 ? s1 : (prog[0x192] >> 1 & 3) + 1`
  (1 EMU, 2 FAST);
* DCA4 mode (0x4629cb): `s2 > 0 ? (s2 - 1) & 1 : prog[0x196] >> 1 & 1`;
* muffle (0x4641a0): `s3 > 0 ? s3 - 1 : prog[0x13b] >> 2 & 1`;
* DC blocking (0x463dd1): `s4 > 0 ? s4 - 1 : prog[0x192] >> 3 & 3` (0 SMART: on when SYNC,
  0x170, is set).

Verified: values read by the original from 8 ini variants (original file, changed values,
missing keys/sections, empty/missing file, number formats and ranges) and the copy into the
master fields.
