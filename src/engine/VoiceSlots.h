// Voice slots of the engine. The original has 16 (the CplugMaster and Cdoc arrays): 8
// playable voices plus 8 more where the voices taken by soft voice stealing fade out.
//
// The port keeps that layout for the differential tests (Master's constructor) and uses its
// own for the plugin (Master::setPortLayout): kMaxPlayableVoices playable slots 0..63 and
// the same number of fade slots at kFadeSlotBase (64..127), so every voice that was playing
// can still fade out. The playable voices of a program (EMU -> VOICES, a port addition) are
// only a limit on the allocator, so changing them never stops a note.
#pragma once

namespace sq8l {

constexpr int kOriginalVoiceSlots = 16;      // the original's arrays (and state layouts)
constexpr int kOriginalPlayableVoices = 8;   // FUN_00463358(8, 8)
constexpr int kOriginalFadeVoices = 8;
constexpr int kMinPlayableVoices = 1;
constexpr int kMaxPlayableVoices = 64;
constexpr int kFadeSlotBase = kMaxPlayableVoices;        // first fade slot of the port layout
constexpr int kMaxVoiceSlots = 2 * kMaxPlayableVoices;   // 128

}  // namespace sq8l
