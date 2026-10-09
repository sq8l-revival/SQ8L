#include "Tuning.h"

#include <cmath>

#include "Doc.h"  // docPitchToFreq

namespace sq8l {

namespace {

// docPitchToFreq folds down by octaves at or above this, which would drop the note an octave
// instead of saturating, so the semitone split has to keep the lookup below it.
constexpr int32_t kPitchCeil = 0x8000;
// render() advances the DOC at most once per host sample, which needs phaseInc < 2^30.
constexpr uint32_t kPhaseLimit = 1u << 30;
// The bias the original applies to the pitch before the table lookup (Doc::update).
constexpr int32_t kAnchorBias = -16;

// The DOC frequency register that would sound exactly 12-ET, i.e. the smooth exponential that
// kDocPitchTable approximates with integers. Absolute tuning divides the real register by it to
// get the table's own rounding error, which is the whole of the SQ-80's per-key pitch offset.
//
// Derivation: a single-cycle wave at resolution 1 sounds at reg * docRate / 2^18, and wave 0
// anchors key k at pitch p = (k + 12) * 256 - 16, so
//
//   k           = (p + 16) / 256 - 12
//   (k - 69)/12 = (p + 16) / 3072 - 6.75
//   F(p)        = 440 * 2^((p+16)/3072 - 6.75) * 2^18 / docRate = kIdealRef * 2^(p/3072)
//   kIdealRef   = 440 * 2^(16/3072 - 6.75 + 18) / 38455.85546875
//
// It serves every wavesample record, not just wave 0: a record's semitone and fine offsets,
// its resolution and the number of cycles its table holds are all chosen so the instrument
// plays in tune, and all of them are exact powers of two, so they cancel out of reg / F(p).
// (Checked against the physical reg * docRate / 2^(res+17) model for wave 0: identical to
// 1e-6 cents, and A4 comes out at the SQ-80's own 439.9456 Hz, -0.214 cents.)
constexpr double kIdealRef = 0x1.bf78bc3559206p+4;  // 27.966976364508831

double idealRegister(int32_t pitch) {
    return kIdealRef * std::exp2(static_cast<double>(pitch) / 3072.0);
}

bool usable(double x) { return std::isfinite(x) && x > 0.0; }

}  // namespace

VoiceRetune computeRetune(const Tuning& t, int key, int32_t pBase, uint32_t nominalPhaseInc) {
    VoiceRetune out;  // {0, 1.0}: the caller's bit-exact path
    if (!t.active() || key < 0 || key > 127 || nominalPhaseInc == 0) return out;

    const int32_t anchor = pBase + kAnchorBias;
    if (anchor < 0 || anchor >= kPitchCeil) return out;
    const uint32_t fAnchor = docPitchToFreq(anchor);
    if (fAnchor == 0) return out;

    // The deviation to apply, as a frequency ratio.
    //
    // Relative takes the master's own retuning and leaves the SQ-80's per-key pitch offset in
    // place, so it assumes nothing about the master's reference pitch and a master in plain
    // 12-ET hands back exactly 1.0. Absolute additionally divides out that offset, which is
    // the pitch table's rounding error: the real register against the smooth exponential the
    // table approximates. Comparing registers rather than frequencies is what makes this work
    // across a multisample split, where the resolution and the cycles per table change
    // together and only cancel in the ratio.
    double deviation = t.ratio[key];
    if (!usable(deviation)) return out;  // a master reporting nonsense: leave the voice alone
    if (t.correctNativeOffsets) {
        const double ideal = idealRegister(anchor);
        if (!usable(ideal)) return out;
        deviation *= ideal / static_cast<double>(fAnchor);
    } else if (deviation == 1.0) {
        return out;  // exactly 12-ET and nothing else to correct: a no-op
    }
    if (!usable(deviation)) return out;

    const double off = 3072.0 * std::log2(deviation);  // 1/256 semitone
    if (!std::isfinite(off)) return out;

    // Whole semitones ride the pitch table like the bend does; the residual rides the clock,
    // which keeps the ratio inside the phaseInc headroom at any supported sample rate.
    int32_t n = static_cast<int32_t>(std::lround(off / 256.0));
    while (n != 0 && (anchor + n * 256 < 0 || anchor + n * 256 >= kPitchCeil)) n += n > 0 ? -1 : 1;

    const uint32_t fActual = docPitchToFreq(anchor + n * 256);
    if (fActual == 0) return out;

    // The denominator is the register the engine will really use, so the ratio absorbs the
    // table's quantization, its own rounding, and any clamping of n above.
    double ratio =
        static_cast<double>(fAnchor) * std::exp2(off / 3072.0) / static_cast<double>(fActual);
    if (!usable(ratio)) return out;

    const double maxRatio =
        static_cast<double>(kPhaseLimit - 1u) / static_cast<double>(nominalPhaseInc);
    const double minRatio = 1.0 / static_cast<double>(nominalPhaseInc);
    if (ratio > maxRatio) ratio = maxRatio;
    if (ratio < minRatio) ratio = minRatio;

    out.pitchOffset = n * 256;
    out.clockRatio = ratio;
    return out;
}

}  // namespace sq8l
