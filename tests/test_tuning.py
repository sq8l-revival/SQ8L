"""Retune math for MTS-ESP support (port addition, no counterpart in the original).

The central invariant: the realised pitch is exact regardless of how the offset is split
between whole semitones (the pitch table) and the residual (the voice's resampler clock).

Usage: test_tuning.py
"""
import ctypes
import math
import sys

from harness import LIB_PATH

NOMINAL_44100 = 936318848
PHASE_LIMIT = 1 << 30
# Wave 0, key 60, semitone 0, fine 0: Doc::computePitch gives this base pitch.
P_BASE_K60 = 18432
# The smooth exponential kDocPitchTable approximates; see kIdealRef in src/engine/Tuning.cpp.
IDEAL_REF = float.fromhex('0x1.bf78bc3559206p+4')
# PIANO's multisample record changes at MIDI 55/56; SYNTH1's and SAW's do not.
WAVE_SAW, WAVE_PIANO, WAVE_SYNTH1 = 0, 9, 16


def ideal_register(pitch):
    return IDEAL_REF * 2.0 ** (pitch / 3072.0)


def lib():
    L = ctypes.CDLL(LIB_PATH)
    vp, d, i32, u32 = ctypes.c_void_p, ctypes.c_double, ctypes.c_int32, ctypes.c_uint32
    L.sq8l_tuning_compute_retune.argtypes = [i32, i32, i32, ctypes.POINTER(d), i32, i32, u32,
                                             ctypes.POINTER(i32), ctypes.POINTER(d)]
    L.sq8l_tuning_compute_retune.restype = None
    L.sq8l_doc_pitch_to_freq.argtypes = [i32]
    L.sq8l_doc_pitch_to_freq.restype = u32
    L.sq8l_doc_new.argtypes = [u32]
    L.sq8l_doc_new.restype = vp
    L.sq8l_doc_free.argtypes = [vp]
    L.sq8l_doc_base_pitch.argtypes = [vp, i32, i32]
    L.sq8l_doc_base_pitch.restype = i32
    return L


def retune(L, ratios=None, key=60, p_base=P_BASE_K60, enabled=True, absolute=False,
           connected=True, nominal=NOMINAL_44100):
    """-> (pitchOffset in 1/256 semitone, clockRatio).

    `ratios` is the master's deviation from 12-ET per key (MTS_RetuningAsRatio, 1.0 = none).
    """
    rbuf = (ctypes.c_double * 128)(*[1.0] * 128)
    if ratios is not None:
        for k, v in enumerate(ratios):
            rbuf[k] = v
    off, ratio = ctypes.c_int32(), ctypes.c_double()
    L.sq8l_tuning_compute_retune(int(enabled), int(absolute), int(connected), rbuf, key,
                                 p_base, nominal, ctypes.byref(off), ctypes.byref(ratio))
    return off.value, ratio.value


def realised(L, p_base, off, ratio):
    """The sounding frequency ratio the engine actually produces for this split."""
    f_anchor = L.sq8l_doc_pitch_to_freq(p_base - 16)
    f_actual = L.sq8l_doc_pitch_to_freq(p_base - 16 + off)
    return (f_actual / f_anchor) * ratio


def cents(a, b):
    return 1200.0 * math.log2(a / b)


def main():
    L = lib()
    fails = []

    def check(name, cond, detail=""):
        if cond:
            print(f"  ok   {name}")
        else:
            print(f"  FAIL {name} {detail}")
            fails.append(name)

    flat = [1.0] * 128      # a master in plain 12-ET
    doubled = [2.0] * 128   # an octave up

    print("inactive and degenerate inputs")
    for name, kw in [("disabled", dict(enabled=False)),
                     ("no master", dict(connected=False)),
                     ("disabled and no master", dict(enabled=False, connected=False))]:
        got = retune(L, ratios=doubled, **kw)
        check(f"{name} -> no retune", got == (0, 1.0), f"got {got}")

    # Review Focus 1: a master reporting nonsense must not retune at all.
    for name, value in [("zero", 0.0), ("negative", -2.0),
                        ("nan", float("nan")), ("inf", float("inf"))]:
        r = [1.0] * 128
        r[60] = value
        for absolute in (False, True):
            got = retune(L, ratios=r, absolute=absolute)
            check(f"ratio {name} -> no retune (absolute={absolute})", got == (0, 1.0), f"got {got}")

    # Review Focus 2: Voice::key is int8_t and -1 means "no key".
    for key in (-1, -128, 128, 255):
        got = retune(L, ratios=doubled, key=key)
        check(f"key {key} -> no retune", got == (0, 1.0), f"got {got}")

    print("relative mode: a master in plain 12-ET is a perfect no-op")
    for key in (24, 48, 60, 72, 96, 120):
        got = retune(L, ratios=flat, key=key)
        check(f"ratio 1.0 at key {key} -> ratio exactly 1.0", got == (0, 1.0), f"got {got}")

    print("relative mode: the realised pitch is exact at every split")
    for c in (-1200, -700, -300, -50, -40, -1, 1, 40, 50, 300, 700, 1200):
        r = [1.0] * 128
        r[60] = 2.0 ** (c / 1200.0)
        off, ratio = retune(L, ratios=r)
        err = abs(cents(realised(L, P_BASE_K60, off, ratio), r[60]))
        check(f"{c:+5d} cents realised within 1e-4 cents", err < 1e-4,
              f"error {err:.3e} cents, split off={off} ratio={ratio!r}")

    print("the clock is what delivers the accuracy, not the pitch table")
    # Guard against the exactness checks above passing vacuously: with the residual dropped
    # (clockRatio forced to 1.0) the pitch table alone is off by whole cents.
    worst = 0.0
    for c in (-40, -17, -7, 7, 17, 40):
        r = [1.0] * 128
        r[60] = 2.0 ** (c / 1200.0)
        off, _ = retune(L, ratios=r)
        worst = max(worst, abs(cents(realised(L, P_BASE_K60, off, 1.0), r[60])))
    check("the table alone is off by more than half a cent", worst > 0.5,
          f"worst table-only error {worst:.3f} cents")

    print("the split keeps the residual inside half a semitone")
    for c in (-1200, -700, 700, 1200):
        r = [1.0] * 128
        r[60] = 2.0 ** (c / 1200.0)
        off, ratio = retune(L, ratios=r)
        check(f"{c:+5d} cents residual within half a semitone",
              2.0 ** (-0.5 / 12) <= ratio <= 2.0 ** (0.5 / 12), f"ratio {ratio!r} off {off}")

    print("absolute mode lands on the ideal register, removing the table's rounding")
    for key, p_base in ((60, P_BASE_K60), (48, P_BASE_K60 - 12 * 256),
                        (84, P_BASE_K60 + 24 * 256)):
        off, ratio = retune(L, ratios=flat, key=key, p_base=p_base, absolute=True)
        anchor = p_base - 16
        got = L.sq8l_doc_pitch_to_freq(anchor + off) * ratio
        err = abs(cents(got, ideal_register(anchor)))
        check(f"key {key}: the realised register is the ideal one within 1e-4 cents",
              err < 1e-4, f"error {err:.3e} cents")
        native = cents(L.sq8l_doc_pitch_to_freq(anchor), ideal_register(anchor))
        check(f"key {key}: the correction is the table's own error ({native:+.3f} cents)",
              abs(cents(ratio, 1.0) + native) < 1e-4)

    print("absolute mode across a multisample split (the PIANO octave-jump regression)")
    # PIANO's wavesample record changes at MIDI 55/56: the resolution goes 3 -> 1 and the table
    # holds a quarter as many cycles. Deriving the sounding pitch physically made absolute mode
    # read keys <= 55 as two octaves flat and "correct" them upward, which also dragged the
    # patch's other oscillators along, since one clock serves the whole voice. Comparing
    # registers against the ideal exponential instead is immune: those factors are exact powers
    # of two and cancel.
    # Every wave, every key, not a sample: a multisample split can sit anywhere, so this is
    # swept exhaustively rather than spot-checked.
    doc = L.sq8l_doc_new(8)
    worst_c = (0.0, None)
    worst_err = (0.0, None)
    bad_offset = None
    declined = 0
    for wave in range(75):
        for key in range(24, 109):
            p_base = L.sq8l_doc_base_pitch(doc, key, wave)
            anchor = p_base - 16
            off, ratio = retune(L, ratios=flat, key=key, p_base=p_base, absolute=True)
            if off != 0 and bad_offset is None:
                bad_offset = (wave, key, off)
            if not 0 <= anchor < 0x8000:
                # Past the top of the table the engine's own lookup folds down an octave, which
                # is the original's behaviour there; the retune declines rather than "fixing" it.
                declined += 1
                if (off, ratio) != (0, 1.0):
                    bad_offset = bad_offset or (wave, key, off)
                continue
            c = abs(cents(ratio, 1.0))
            if c > worst_c[0]:
                worst_c = (c, (wave, key))
            # What is heard: the register the engine ends up using, times the clock. It must be
            # the ideal exponential, which is smooth by construction -- so however large the
            # correction has to be, the corrected pitch has no steps in it.
            err = abs(cents(L.sq8l_doc_pitch_to_freq(anchor + off) * ratio, ideal_register(anchor)))
            if err > worst_err[0]:
                worst_err = (err, (wave, key))
    L.sq8l_doc_free(doc)
    check("no wave shifts a whole semitone anywhere on the keyboard", bad_offset is None,
          f"wave {bad_offset[0]} key {bad_offset[1]} got pitchOffset {bad_offset[2]}"
          if bad_offset else "")
    check(f"the corrected pitch is the ideal exponential everywhere, so it has no steps "
          f"(worst {worst_err[0]:.2e} cents at wave/key {worst_err[1]})", worst_err[0] < 1e-4)
    # The correction itself is as large as the table's error at that key, which is biggest in
    # the extreme bass of the percussion one-shots, where the register is only a few counts.
    check(f"the correction it needs stays under 25 cents over all 75 waves "
          f"(worst {worst_c[0]:.2f} at wave/key {worst_c[1]})", worst_c[0] < 25.0)
    check(f"and it declines past the top of the pitch table ({declined} wave/key pairs)",
          declined > 0)

    print("clamping")
    # Review Focus 3: near the top of the table the octave fold must never engage, so n is
    # reduced and the pitch saturates instead of dropping an octave.
    high_base = 32000
    r = [1.0] * 128
    r[60] = 2.0  # +1200 cents, which cannot be folded in at this base
    off, ratio = retune(L, ratios=r, p_base=high_base)
    check("near the table top the lookup stays in range",
          0 <= high_base - 16 + off < 0x8000, f"off {off} -> {high_base - 16 + off}")
    # Review Focus 4: phaseInc must stay strictly below 2^30.
    inc = round(NOMINAL_44100 * ratio)
    check("phaseInc stays below 2^30", 1 <= inc < PHASE_LIMIT, f"inc {inc}")
    # Review Focus 5: a huge downward retune must not round phaseInc to zero.
    r = [1.0] * 128
    r[60] = 1e-9
    off, ratio = retune(L, ratios=r)
    inc = round(NOMINAL_44100 * ratio)
    check("extreme downward retune keeps phaseInc >= 1", inc >= 1, f"inc {inc} ratio {ratio!r}")

    print()
    if fails:
        print(f"FAILED: {len(fails)} check(s): {', '.join(fails)}")
        return 1
    print("all tuning math checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
