"""Per-voice resampler clock inside Doc, and the Master/Synth tuning wiring (port addition).

Usage: test_tuning_engine.py
"""
import ctypes
import math
import struct
import sys

from harness import LIB_PATH

DOC_SIZE = 10400
DOC_RATE = 38455.85546875
NOMINAL_44100 = 936318848
PHASE_LIMIT = 1 << 30
# Doc image offsets of the nominal values (tests/capi_doc.cpp globalFields).
OFF_PHASE_INC = 0x2068
OFF_SMOOTH_POLE = 0x2080
OFF_AM_SMOOTH_POLE = 0x2084
OFF_SMOOTH_GAIN = 0x2088
OFF_AM_SMOOTH_GAIN = 0x208C


def lib():
    L = ctypes.CDLL(LIB_PATH)
    vp, f32, d, i32, u32 = (ctypes.c_void_p, ctypes.c_float, ctypes.c_double,
                            ctypes.c_int32, ctypes.c_uint32)
    L.sq8l_doc_new.argtypes = [u32]
    L.sq8l_doc_new.restype = vp
    L.sq8l_doc_free.argtypes = [vp]
    L.sq8l_doc_set_sample_rate.argtypes = [vp, f32, i32]
    L.sq8l_doc_save.argtypes = [vp, ctypes.c_char_p, u32]
    L.sq8l_doc_set_voice_clock.argtypes = [vp, u32, d]
    L.sq8l_doc_voice_clock.argtypes = [vp, u32, ctypes.POINTER(u32), ctypes.POINTER(f32),
                                       ctypes.POINTER(f32), ctypes.POINTER(f32),
                                       ctypes.POINTER(f32)]
    L.sq8l_synth_new.argtypes = [f32]
    L.sq8l_synth_new.restype = vp
    L.sq8l_synth_free.argtypes = [vp]
    L.sq8l_synth_events.argtypes = [vp, ctypes.c_void_p, i32]
    L.sq8l_synth_process.argtypes = [vp, ctypes.POINTER(f32), ctypes.POINTER(f32), i32, i32]
    L.sq8l_synth_set_tuning.argtypes = [vp, i32, i32, i32, ctypes.POINTER(d),
                                        ctypes.POINTER(u32)]
    L.sq8l_synth_voice_phase_inc.argtypes = [vp, i32]
    L.sq8l_synth_voice_phase_inc.restype = u32
    L.sq8l_synth_active_voices.argtypes = [vp]
    L.sq8l_synth_active_voices.restype = i32
    L.sq8l_synth_slot_of_key.argtypes = [vp, i32]
    L.sq8l_synth_slot_of_key.restype = i32
    L.sq8l_synth_set_mts.argtypes = [vp, i32, i32]
    L.sq8l_synth_get_mts.argtypes = [vp, ctypes.POINTER(i32), ctypes.POINTER(i32)]
    L.sq8l_synth_get_chunk.argtypes = [vp, ctypes.POINTER(ctypes.c_uint8), i32]
    L.sq8l_synth_get_chunk.restype = i32
    L.sq8l_synth_set_chunk.argtypes = [vp, ctypes.POINTER(ctypes.c_uint8), i32]
    L.sq8l_synth_set_chunk.restype = i32
    return L


class Raw(ctypes.Structure):
    _fields_ = [("deltaFrames", ctypes.c_int32), ("data", ctypes.c_uint8 * 3),
                ("noteOffVelocity", ctypes.c_uint8)]


def set_tuning(L, s, ratios=None, enabled=True, absolute=False, connected=True,
               filtered=(0, 0, 0, 0)):
    """`ratios` is the master's deviation from 12-ET per key (1.0 = no change)."""
    rbuf = (ctypes.c_double * 128)(*[1.0] * 128)
    if ratios is not None:
        for k, v in enumerate(ratios):
            rbuf[k] = v
    mask = (ctypes.c_uint32 * 4)(*filtered)
    L.sq8l_synth_set_tuning(s, int(enabled), int(absolute), int(connected), rbuf, mask)


def clock_of_key(L, s, key):
    """The resampler increment of the voice playing `key`.

    The port layout maps the playable voices over 128 slots through Master::slotMap_, so a
    note does not land in slot 0 and the slot has to be looked up.
    """
    slot = L.sq8l_synth_slot_of_key(s, key)
    assert slot >= 0, f"no active voice for key {key}"
    return L.sq8l_synth_voice_phase_inc(s, slot)


def note_on(L, s, key, vel=100):
    ev = (Raw * 1)()
    ev[0].deltaFrames = 0
    ev[0].data[0], ev[0].data[1], ev[0].data[2] = 0x90, key, vel
    L.sq8l_synth_events(s, ctypes.byref(ev), 1)


def run_block(L, s, frames=1024):
    left = (ctypes.c_float * frames)()
    right = (ctypes.c_float * frames)()
    L.sq8l_synth_process(s, left, right, frames, 1)
    return left, right


def nominal(L, doc):
    buf = ctypes.create_string_buffer(DOC_SIZE)
    L.sq8l_doc_save(doc, buf, 0)
    b = buf.raw
    u32_at = lambda o: struct.unpack_from("<I", b, o)[0]  # noqa: E731
    f32_at = lambda o: struct.unpack_from("<f", b, o)[0]  # noqa: E731
    return dict(phase_inc=u32_at(OFF_PHASE_INC), pole=f32_at(OFF_SMOOTH_POLE),
                gain=f32_at(OFF_SMOOTH_GAIN), am_pole=f32_at(OFF_AM_SMOOTH_POLE),
                am_gain=f32_at(OFF_AM_SMOOTH_GAIN))


def voice_clock(L, doc, v):
    inc = ctypes.c_uint32()
    pole, gain, am_pole, am_gain = (ctypes.c_float() for _ in range(4))
    L.sq8l_doc_voice_clock(doc, v, ctypes.byref(inc), ctypes.byref(pole), ctypes.byref(gain),
                           ctypes.byref(am_pole), ctypes.byref(am_gain))
    return dict(phase_inc=inc.value, pole=pole.value, gain=gain.value,
                am_pole=am_pole.value, am_gain=am_gain.value)


def main():
    L = lib()
    fails = []

    def check(name, cond, detail=""):
        if cond:
            print(f"  ok   {name}")
        else:
            print(f"  FAIL {name} {detail}")
            fails.append(name)

    doc = L.sq8l_doc_new(8)
    L.sq8l_doc_set_sample_rate(doc, 44100.0, 0)
    nom = nominal(L, doc)

    print("nominal clock")
    check("Doc phaseInc at 44100 is the documented value", nom["phase_inc"] == NOMINAL_44100,
          f'got {nom["phase_inc"]}')
    for v in range(8):
        got = voice_clock(L, doc, v)
        check(f"voice {v} starts at the nominal clock, bit for bit", got == nom,
              f"got {got} want {nom}")

    print("ratio 1.0 copies the nominal values rather than recomputing them")
    for v in range(8):
        L.sq8l_doc_set_voice_clock(doc, v, 1.0)
        got = voice_clock(L, doc, v)
        check(f"voice {v} after setVoiceClock(1.0)", got == nom, f"got {got} want {nom}")

    print("setSampleRate puts every voice back on the nominal clock")
    L.sq8l_doc_set_voice_clock(doc, 3, 2.0 ** (0.4 / 12))
    check("voice 3 moved off nominal first",
          voice_clock(L, doc, 3)["phase_inc"] != nom["phase_inc"])
    L.sq8l_doc_set_sample_rate(doc, 48000.0, 0)
    nom48 = nominal(L, doc)
    for v in range(8):
        got = voice_clock(L, doc, v)
        check(f"voice {v} follows the new sample rate", got == nom48, f"got {got} want {nom48}")

    print("scaled clock")
    L.sq8l_doc_set_sample_rate(doc, 44100.0, 0)
    nom = nominal(L, doc)
    for cents in (-100, -50, -10, 10, 50, 100):
        ratio = 2.0 ** (cents / 1200.0)
        L.sq8l_doc_set_voice_clock(doc, 0, ratio)
        got = voice_clock(L, doc, 0)
        want_inc = round(nom["phase_inc"] * ratio)
        check(f"{cents:+4d} cents scales phaseInc", got["phase_inc"] == want_inc,
              f'got {got["phase_inc"]} want {want_inc}')
        check(f"{cents:+4d} cents keeps phaseInc in range",
              1 <= got["phase_inc"] < PHASE_LIMIT, f'got {got["phase_inc"]}')

    print("the DCA/AM time constants do not stretch with the clock")
    # pole = 0.01^(1/(T*rate+1))  =>  rate = (ln(0.01)/ln(pole) - 1)/T. The implied rate must
    # track docRate*ratio, which is exactly what it means for T itself to stay put.
    def implied_rate(pole, t_seconds):
        return (math.log(0.01) / math.log(pole) - 1.0) / t_seconds

    for cents in (-100, -50, 50, 100):
        ratio = 2.0 ** (cents / 1200.0)
        L.sq8l_doc_set_voice_clock(doc, 0, ratio)
        got = voice_clock(L, doc, 0)
        for label, pole, t_seconds in (("DCA 2 ms", got["pole"], 0.002),
                                       ("AM 0.2 ms", got["am_pole"], 0.0002)):
            rate = implied_rate(pole, t_seconds)
            err = abs(rate / (DOC_RATE * ratio) - 1.0)
            check(f"{label} at {cents:+4d} cents implies the scaled DOC rate", err < 2e-3,
                  f"implied {rate:.1f} want {DOC_RATE * ratio:.1f} (err {err:.2e})")
        check(f"gain is 1 - pole at {cents:+4d} cents",
              abs(got["gain"] - (1.0 - got["pole"])) < 1e-7 and
              abs(got["am_gain"] - (1.0 - got["am_pole"])) < 1e-7)

    print("the poles are cached per 1/64 semitone, the pitch is not")
    # One bucket is 1/64 semitone = 1.5625 cents, so the q boundary sits at 0.78 cents:
    # 0.2 and 0.7 share bucket 0, where 0.5 and 0.9 would straddle it.
    a = 2.0 ** (0.2 / 1200.0)
    b = 2.0 ** (0.7 / 1200.0)
    L.sq8l_doc_set_voice_clock(doc, 1, a)
    first = voice_clock(L, doc, 1)
    L.sq8l_doc_set_voice_clock(doc, 1, b)
    second = voice_clock(L, doc, 1)
    check("poles unchanged inside one bucket",
          (first["pole"], first["am_pole"]) == (second["pole"], second["am_pole"]),
          f"{first} vs {second}")
    check("phaseInc still tracks the exact ratio",
          second["phase_inc"] == round(nom["phase_inc"] * b),
          f'got {second["phase_inc"]} want {round(nom["phase_inc"] * b)}')
    L.sq8l_doc_set_voice_clock(doc, 1, 2.0 ** (60.0 / 1200.0))
    third = voice_clock(L, doc, 1)
    check("poles do change across buckets", third["pole"] != second["pole"],
          f'both {third["pole"]}')

    print("degenerate ratios fall back to nominal")
    for bad in (0.0, -1.0, float("nan")):
        L.sq8l_doc_set_voice_clock(doc, 2, bad)
        got = voice_clock(L, doc, 2)
        check(f"ratio {bad!r} -> nominal", got == nom, f"got {got}")

    L.sq8l_doc_free(doc)

    print("Master/Synth wiring")
    flat = [1.0] * 128                      # a master in plain 12-ET
    sharp40 = [1.0] * 128
    sharp40[60] = 2.0 ** (40.0 / 1200.0)    # key 60 up 40 cents

    for name, kw in [("no tuning set", None), ("disabled", dict(enabled=False)),
                     ("relative 12-ET", dict(enabled=True))]:
        s = L.sq8l_synth_new(44100.0)
        if kw is not None:
            set_tuning(L, s, ratios=flat, **kw)
        note_on(L, s, 60)
        run_block(L, s)
        got = clock_of_key(L, s, 60)
        check(f"{name}: the voice is on the nominal clock", got == NOMINAL_44100, f"got {got}")
        L.sq8l_synth_free(s)

    s = L.sq8l_synth_new(44100.0)
    set_tuning(L, s, ratios=flat, enabled=True)
    note_on(L, s, 60)
    run_block(L, s)
    set_tuning(L, s, ratios=sharp40, enabled=True)
    run_block(L, s)
    moved = clock_of_key(L, s, 60)
    want = round(NOMINAL_44100 * 2.0 ** (40.0 / 1200.0))
    check("a held note follows a changed scale", moved != NOMINAL_44100, f"got {moved}")
    check("the held note's clock matches +40 cents", abs(moved - want) <= 1,
          f"got {moved} want {want}")

    # A master going away mid-note must not leave the voice on a scaled clock.
    set_tuning(L, s, ratios=sharp40, enabled=True, connected=False)
    run_block(L, s)
    back = clock_of_key(L, s, 60)
    check("a master disconnecting mid-note returns the voice to nominal",
          back == NOMINAL_44100, f"got {back}")
    L.sq8l_synth_free(s)

    print("the switches belong to the instance and travel in the chunk")
    # The whole point: a project must recall them, and two instances must be able to differ.
    def get_mts(s):
        a, b = ctypes.c_int32(), ctypes.c_int32()
        L.sq8l_synth_get_mts(s, ctypes.byref(a), ctypes.byref(b))
        return a.value, b.value

    def chunk_of(s):
        buf = (ctypes.c_uint8 * 65536)()
        n = L.sq8l_synth_get_chunk(s, buf, 65536)
        return bytes(buf[:n])

    s = L.sq8l_synth_new(44100.0)
    check("both switches are off in a fresh instance", get_mts(s) == (0, 0), f"got {get_mts(s)}")
    plain = chunk_of(s)
    for want in ((1, 0), (0, 1), (1, 1), (0, 0)):
        L.sq8l_synth_set_mts(s, want[0], want[1])
        data = chunk_of(s)
        t = L.sq8l_synth_new(44100.0)
        L.sq8l_synth_set_chunk(t, (ctypes.c_uint8 * len(data))(*data), len(data))
        check(f"{want} survives a chunk round trip", get_mts(t) == want, f"got {get_mts(t)}")
        L.sq8l_synth_free(t)
    L.sq8l_synth_set_mts(s, 0, 0)
    check("with both off the chunk is byte-identical to a fresh instance's",
          chunk_of(s) == plain)
    # A chunk that predates the feature must leave an instance with both off.
    L.sq8l_synth_set_mts(s, 1, 1)
    L.sq8l_synth_set_chunk(s, (ctypes.c_uint8 * len(plain))(*plain), len(plain))
    check("an older chunk puts the instance back to both off", get_mts(s) == (0, 0),
          f"got {get_mts(s)}")
    L.sq8l_synth_free(s)

    print("note filtering")
    s = L.sq8l_synth_new(44100.0)
    set_tuning(L, s, ratios=flat, enabled=True, filtered=(0, 1 << (61 - 32), 0, 0))
    note_on(L, s, 61)
    run_block(L, s)
    check("a filtered key sounds no voice", L.sq8l_synth_active_voices(s) == 0,
          f"got {L.sq8l_synth_active_voices(s)}")
    note_on(L, s, 60)
    run_block(L, s)
    check("an unfiltered key still sounds", L.sq8l_synth_active_voices(s) >= 1,
          f"got {L.sq8l_synth_active_voices(s)}")
    L.sq8l_synth_free(s)

    print("enabled with no master present is byte-identical to disabled")
    outs = []
    for enabled, connected in ((False, False), (True, False)):
        s = L.sq8l_synth_new(44100.0)
        set_tuning(L, s, ratios=sharp40, enabled=enabled, connected=connected)
        note_on(L, s, 60)
        left, right = run_block(L, s, 4096)
        outs.append((bytes(memoryview(left).cast("B")), bytes(memoryview(right).cast("B"))))
        L.sq8l_synth_free(s)
    check("enabled without a master renders byte-identically", outs[0] == outs[1])

    print()
    if fails:
        print(f"FAILED: {len(fails)} check(s): {', '.join(fails)}")
        return 1
    print("all per-voice clock checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
