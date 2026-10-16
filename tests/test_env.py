"""Differential test: Csq_env (unit mod_env_2, original emulated) vs sq8l::Env (C++).

1. Static tables (time, shape, velocity curves) vs the emulator memory after load.
2. Real calls of every entry point, captured while the emulated plugin plays all factory
   programs of banks C and D, random patches written into the edit buffer, sustain pedal
   and mono/legato/glide sequences. Each call is replayed on the C++ object loaded from the
   captured bytes: outputs and the whole object state (bytes 4..0x7f) must be identical.
3. Direct calls of the original routines (call_fpu) on random states and arguments,
   including edge cases (wrap-around, clamping, both rounding modes, other control rates).
4. Code coverage of the unit's basic blocks over 2. and 3.

Shared helpers (session, MIDI rendering, coverage) are reused by test_modfollower.py.
"""
import collections
import ctypes
import random
import struct
import sys
import time

from harness import LIB_PATH, master_ptr
from vsthost import SQ8LHost, effSetProgram

from unicorn import UC_HOOK_BLOCK
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_FPCW

HOST_FPCW, PROCESS_FPCW = 0x027F, 0x0E7F
RC_NAMES = ["nearest", "down", "up", "zero"]

ENV_SIZE = 0x80
ENV_RANGE = (0x45DBA0, 0x45E32C)
ENV_CTOR, ENV_SET_RATE, ENV_START, ENV_RELEASE, ENV_TICK, ENV_SHAPED = (
    0x45DBCC, 0x45DCB8, 0x45DD2C, 0x45DFB4, 0x45E210, 0x45E1D4)
SHAPE_TABLES = 0x4C1688
RAMP_CODE = [0x45E0BC, 0x45DFD4, 0x45E030, 0x45E08C]
NEXT_CODE = [0x45E0C0, 0x45E0F8, 0x45E130, 0x45E160, 0x45E17C]
CONTROL_RATE = struct.unpack("<f", struct.pack("<I", 0x42A72F66))[0]  # 83.592575

ENV_FIELDS = ([(f"level[{i}]", 0x04 + 4 * i, 4) for i in range(5)]
              + [(f"time[{i}]", 0x18 + 4 * i, 4) for i in range(4)]
              + [("levelScale", 0x28, 4), ("timeKeyScale", 0x2C, 4), ("secondRelease", 0x30, 1),
                 ("cycle", 0x31, 1), ("pad32", 0x32, 2), ("value", 0x34, 4), ("target", 0x38, 4),
                 ("step", 0x3C, 4), ("ticksLeft", 0x40, 4), ("output", 0x44, 4), ("smoothing", 0x48, 4),
                 ("smoothed", 0x4C, 4), ("smoothA", 0x50, 4), ("smoothB", 0x54, 4), ("shape", 0x58, 4),
                 ("shapeTable", 0x5C, 4), ("active", 0x60, 1), ("sustaining", 0x61, 1),
                 ("released", 0x62, 1), ("releasePending", 0x63, 1), ("finished", 0x64, 1),
                 ("pad65", 0x65, 3), ("ramp", 0x68, 4), ("rampSelf", 0x6C, 4), ("next", 0x70, 4),
                 ("nextSelf", 0x74, 4), ("rate", 0x78, 4), ("rateScale", 0x7C, 4)])

# Patch layout (edit buffer): patch = *(*(master + 0xff8) + 0xa94)
PATCH_ENV, PATCH_MONO, PATCH_GLIDE, PATCH_REST_VC, PATCH_REST_ENV, PATCH_REST_OSC, PATCH_CYC = (
    0x6A, 0x172, 0x173, 0x174, 0x175, 0x176, 0x177)
BANK_C = list(range(256, 384))   # SQ8L sounds
BANK_D = list(range(384, 424))   # original SQ-80 sounds
VEL_EXP = 0x494DCC               # velocity curve for LV >= 64


# ------------------------------------------------------------------ C API
def envlib():
    L = ctypes.CDLL(LIB_PATH)
    vp, f32, i32, u32 = ctypes.c_void_p, ctypes.c_float, ctypes.c_int32, ctypes.c_uint32
    cp = ctypes.c_char_p
    sig = {
        "sq8l_env_time_table": ([], ctypes.POINTER(ctypes.c_uint16)),
        "sq8l_env_shape_tables": ([], ctypes.POINTER(ctypes.c_int8)),
        "sq8l_velocity_curve": ([ctypes.c_int], ctypes.POINTER(ctypes.c_uint8)),
        "sq8l_env_new": ([], vp),
        "sq8l_env_free": ([vp], None),
        "sq8l_env_load": ([vp, cp, u32], ctypes.c_int),
        "sq8l_env_save": ([vp, cp, u32], None),
        "sq8l_env_init": ([vp, f32, ctypes.c_int], None),
        "sq8l_env_set_rate": ([vp, f32, ctypes.c_int], None),
        "sq8l_env_start": ([vp, cp, i32, i32, i32, i32, vp, ctypes.c_int], None),
        "sq8l_env_release": ([vp, i32, ctypes.c_int], None),
        "sq8l_env_tick": ([vp, i32, ctypes.c_int], i32),
        "sq8l_env_shaped": ([vp], i32),
        "sq8l_foll_new": ([], vp),
        "sq8l_foll_free": ([vp], None),
        "sq8l_foll_load": ([vp, cp], None),
        "sq8l_foll_save": ([vp, cp], None),
        "sq8l_foll_init": ([vp, f32, ctypes.c_int], None),
        "sq8l_foll_set_rate": ([vp, f32, ctypes.c_int], None),
        "sq8l_foll_set_speed": ([vp, f32, ctypes.c_int], None),
        "sq8l_foll_set_target": ([vp, i32], None),
        "sq8l_foll_reset": ([vp, i32], None),
        "sq8l_foll_tick": ([vp], i32),
        "sq8l_glide_speed": ([i32, i32, ctypes.c_int], f32),
    }
    for name, (args, res) in sig.items():
        fn = getattr(L, name)
        fn.argtypes = args
        fn.restype = res
    return L


# ------------------------------------------------------------------ small helpers
def s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def f32_from_bits(b):
    return struct.unpack("<f", struct.pack("<I", b & 0xFFFFFFFF))[0]


def fpcw_rc(emu):
    return (emu.uc.reg_read(UC_X86_REG_FPCW) >> 10) & 3


def stack_arg(emu, i):
    """i-th dword above the return address at function entry."""
    return emu.u32(emu.regs()["esp"] + 4 * (i + 1))


def field_diffs(a, b, layout):
    return [n for n, o, s in layout if a[o:o + s] != b[o:o + s]]


class Results:
    """Per-routine call / mismatch counters with a few printed examples."""

    def __init__(self, title):
        self.title = title
        self.calls = collections.Counter()
        self.bad = collections.Counter()
        self.examples = collections.defaultdict(list)

    def record(self, name, ok, detail):
        self.calls[name] += 1
        if not ok:
            self.bad[name] += 1
            if len(self.examples[name]) < 5:
                self.examples[name].append(detail)

    def report(self):
        print(self.title)
        for name in self.calls:
            n, b = self.calls[name], self.bad[name]
            print(f"  {name:28s} {n - b:8d}/{n:<8d} bit-exact")
            for d in self.examples[name]:
                print("      ", d)
        return sum(self.bad.values()) == 0


# ------------------------------------------------------------------ coverage
FILLERS = {"nop ", "mov eax, eax", "lea eax, [eax]", "add byte ptr [eax], al"}


class Coverage:
    """Records the basic blocks executed inside [lo, hi)."""

    def __init__(self, emu, lo, hi):
        self.lo, self.hi = lo, hi
        self.blocks = set()
        emu.uc.hook_add(UC_HOOK_BLOCK, self._hook, begin=lo, end=hi - 1)

    def _hook(self, uc, addr, size, _):
        self.blocks.add((addr, size))

    def snapshot(self):
        return set(self.blocks)

    def uncovered(self, emu, skip=(), blocks=None):
        """Instructions never executed, as (start, end, first instruction) runs."""
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        executed = set()
        for a, sz in (self.blocks if blocks is None else blocks):
            for ins in md.disasm(emu.read(a, sz), a):
                executed.add(ins.address)
        runs = []
        for start, end in self._code_ranges(skip):
            cur = None
            for ins in md.disasm(emu.read(start, end - start), start):
                if ins.address in executed:
                    cur = None
                    continue
                text = f"{ins.mnemonic} {ins.op_str}"
                if cur is None:
                    cur = [ins.address, ins.address + ins.size, text, text in FILLERS]
                    runs.append(cur)
                else:
                    cur[1] = ins.address + ins.size
                    cur[3] &= text in FILLERS
        # drop alignment padding between routines
        return [(a, b, first) for a, b, first, filler in runs if not (filler and b - a < 4)]

    def _code_ranges(self, skip):
        ranges = [(self.lo, self.hi)]
        for s, e in sorted(skip):
            nxt = []
            for a, b in ranges:
                if e <= a or s >= b:
                    nxt.append((a, b))
                else:
                    if a < s:
                        nxt.append((a, s))
                    if e < b:
                        nxt.append((e, b))
            ranges = nxt
        return ranges


# ------------------------------------------------------------------ MIDI scenarios
def note(t, dur, key, vel, ch=0):
    return [(t, (0x90 | ch, key, vel)), (t + dur, (0x80 | ch, key, 0))]


def cc(t, num, val, ch=0):
    return [(t, (0xB0 | ch, num, val))]


def render_events(host, events, total_seconds):
    """events: list of (time_s, midi bytes). Renders and discards the audio."""
    sr, blk = host.sr, host.block
    evs = sorted(((int(t * sr), bytes(d)) for t, d in events), key=lambda x: x[0])
    total = int(total_seconds * sr)
    pos, i = 0, 0
    while pos < total:
        n = min(blk, total - pos)
        chunk = []
        while i < len(evs) and evs[i][0] < pos + n:
            chunk.append((max(evs[i][0] - pos, 0), evs[i][1]))
            i += 1
        if chunk:
            host.send_midi(chunk[:256])
        host.process(n)
        pos += n


FACTORY_PATTERN = (note(0.00, 0.60, 36, 127) + note(0.02, 0.50, 60, 64) + note(0.04, 0.45, 72, 1)
                   + note(0.06, 0.40, 96, 100) + note(0.70, 0.30, 60, 90) + note(0.85, 0.10, 60, 40)
                   + note(1.05, 0.10, 48, 30) + note(1.20, 0.10, 48, 110) + note(1.40, 0.05, 108, 127)
                   + note(1.45, 0.05, 21, 64))
FACTORY_SECONDS = 2.0

PEDAL_PATTERN = (cc(0.0, 64, 127) + note(0.05, 0.2, 60, 100) + note(0.10, 0.2, 64, 70)
                 + note(0.40, 0.1, 67, 50) + cc(0.9, 64, 0) + note(1.0, 0.2, 72, 90)
                 + cc(1.1, 64, 100) + note(1.15, 0.05, 74, 80) + cc(1.6, 64, 0))
PEDAL_PROGRAMS = [394, 273, 283, 300, 305, 352, 368, 412]

LEGATO_PATTERN = (note(0.0, 0.5, 48, 100) + note(0.4, 0.5, 60, 80) + note(0.8, 0.5, 55, 60)
                  + note(1.2, 0.3, 72, 120) + note(1.8, 0.3, 40, 90) + note(2.2, 0.2, 40, 30)
                  + note(2.3, 0.4, 41, 127))
LEGATO_SECONDS = 3.0


def patch_ptr(h):
    e = h.emu
    return e.u32(e.u32(master_ptr(h) + 0xFF8) + 0xA94)


def velocity_divides_by_zero(e, lv):
    """Velocities for which LV >= 64 gives levelScale == 0 (the shaper would fault)."""
    if lv < 64:
        return set()
    curve = e.read(VEL_EXP, 128)
    return {v for v in range(128) if ((lv * curve[v]) >> 5) + (63 - lv) * 4 == 0}


def random_env_record(rng, short=True):
    lv = rng.choice([rng.randint(0, 63), rng.randint(64, 127)])
    tmax = 40 if short and rng.random() < 0.8 else 63
    rec = [rng.randint(-63, 63) & 0xFF for _ in range(4)] + [rng.randint(-63, 63) & 0xFF]
    rec += [lv, rng.randint(0, 63)]
    rec += [rng.randint(0, tmax) for _ in range(3)]
    rec += [rng.randint(0, tmax) | (0x40 if rng.random() < 0.3 else 0)]
    rec += [rng.choice([0, rng.randint(0, 63)])]
    flags = (rng.random() < 0.25) | (rng.randint(0, 7) << 1) | (0x10 if rng.random() < 0.3 else 0)
    rec += [flags, rng.choice([0, rng.randint(0, 63)])]
    return bytes(rec)


def random_patch_events(h, rng):
    """Write random envelope records / mode flags into the edit buffer; return MIDI events."""
    e = h.emu
    p = patch_ptr(h)
    forbidden = set()
    for i in range(4):
        rec = random_env_record(rng)
        e.write(p + PATCH_ENV + 14 * i, rec)
        if (rec[12] >> 1) & 7:
            forbidden |= velocity_divides_by_zero(e, rec[5])
    e.write(p + PATCH_MONO, bytes([rng.random() < 0.3, rng.choice([0, rng.randint(0, 40)]),
                                   rng.random() < 0.5, rng.random() < 0.5, rng.random() < 0.5,
                                   rng.random() < 0.25]))
    allowed = [v for v in range(1, 128) if v not in forbidden]
    events = []
    t = 0.0
    keys = [rng.randint(0, 127) for _ in range(3)]
    for _ in range(rng.randint(5, 10)):
        key = rng.choice(keys) if rng.random() < 0.4 else rng.randint(0, 127)
        events += note(t, rng.choice([0.03, 0.1, 0.3, 0.6, 1.2]), key, rng.choice(allowed))
        t += rng.choice([0.0, 0.02, 0.1, 0.25, 0.4])
    if rng.random() < 0.3:
        events += cc(rng.uniform(0, 1), 64, 127) + cc(rng.uniform(1, 2.5), 64, 0)
    return events


LEGATO_PROGRAMS = [260, 264, 265, 266, 268, 272, 280, 284, 285, 291, 297, 301, 367, 371, 397]  # mono / glide


def _log(msg):
    print(msg, flush=True)


def play_session(h, rng, factory=BANK_C + BANK_D, n_random=60, log=_log):
    """Play the full scenario. Returns a dict of section -> seconds of audio."""
    t0 = time.time()
    played = collections.Counter()
    for prog in factory:
        h.dispatch(effSetProgram, value=prog)
        render_events(h, FACTORY_PATTERN, FACTORY_SECONDS)
        played["factory"] += FACTORY_SECONDS
    log(f"  factory programs done ({time.time() - t0:.0f}s)")
    for prog in PEDAL_PROGRAMS:
        h.dispatch(effSetProgram, value=prog)
        render_events(h, PEDAL_PATTERN, 2.5)
        played["pedal"] += 2.5
    for prog in LEGATO_PROGRAMS:
        h.dispatch(effSetProgram, value=prog)
        render_events(h, LEGATO_PATTERN, LEGATO_SECONDS)
        played["legato/glide"] += LEGATO_SECONDS
    log(f"  pedal / legato done ({time.time() - t0:.0f}s)")
    for _ in range(n_random):
        h.dispatch(effSetProgram, value=rng.choice(BANK_C + BANK_D))
        events = random_patch_events(h, rng)
        render_events(h, events, 3.0)
        played["random patches"] += 3.0
    # Let everything ring out to voice end.
    render_events(h, [], 6.0)
    played["tail"] += 6.0
    log(f"  random patches done ({time.time() - t0:.0f}s)")
    return played


def new_host():
    return SQ8LHost(sample_rate=44100.0)


# ------------------------------------------------------------------ envelope replay
class EnvChecker:
    def __init__(self, L, emu):
        self.L, self.e = L, emu
        self.obj = L.sq8l_env_new()
        self.other = L.sq8l_env_new()
        self.buf = ctypes.create_string_buffer(ENV_SIZE)
        self.res = Results("envelope calls")
        self.stats = collections.Counter()

    def _load(self, h, b, addr):
        err = self.L.sq8l_env_load(h, b, addr)
        if err:
            raise RuntimeError(f"unmappable envelope state at {addr:#x} (err {err})")

    def _state(self, base, addr):
        self.buf.raw = base
        self.L.sq8l_env_save(self.obj, self.buf, addr)
        return self.buf.raw

    def _cmp(self, name, after_ours, after_orig, out_ours=None, out_orig=None, info=""):
        diffs = field_diffs(after_ours, after_orig, ENV_FIELDS)
        ok = not diffs and out_ours == out_orig
        detail = ""
        if not ok:
            detail = f"{info} out ours={out_ours} orig={out_orig} diffs=" + ", ".join(
                f"{n}: ours={after_ours[o:o + s].hex()} orig={after_orig[o:o + s].hex()}"
                for n, o, s in ENV_FIELDS if n in diffs)
        self.res.record(name, ok, detail)

    # one method per routine: (before bytes, inputs..., after bytes [, output])
    def ctor(self, addr, rate, rc, after):
        self.L.sq8l_env_init(self.obj, rate, rc)
        self._cmp("ctor 45dbcc", self._state(bytes(ENV_SIZE), addr), after, info=f"rate={rate!r}")

    def set_rate(self, addr, before, rate, rc, after, name="setRate 45dcb8"):
        self._load(self.obj, before, addr)
        self.L.sq8l_env_set_rate(self.obj, rate, rc)
        self._cmp(name, self._state(before, addr), after, info=f"rate={rate!r} rc={rc}")

    def start(self, addr, before, rec, key, vel, restart, cycle, prev, prev_bytes, rc, after, name="start 45dd2c"):
        self._load(self.obj, before, addr)
        if prev == 0:
            ph, kind = None, "none"
        elif prev == addr:
            ph, kind = self.obj, "self"
        else:
            self._load(self.other, prev_bytes, prev)
            ph, kind = self.other, "other"
        self.stats[f"start prev={kind} restart={int(restart != 0)} rounding={RC_NAMES[rc]}"] += 1
        if kind == "self":
            self.stats["start prev=self (same voice restruck)"] += 1
        flags = rec[12]
        for feature, present in (("SHAPE", (flags >> 1) & 7), ("MODE-T1V=SMT", flags & 0x10),
                                 ("SMTH>0", rec[13]), ("CYC", cycle), ("R (T4 bit 6)", rec[10] & 0x40),
                                 ("LV>=64 (X)", rec[5] >= 64), ("L0!=0", rec[0]), ("TK!=0", rec[11]),
                                 ("T1V!=0", rec[6])):
            if present:
                self.stats[f"start feature {feature}" + (f"={present}" if feature == "SHAPE" else "")] += 1
        self.L.sq8l_env_start(self.obj, rec, key, vel, restart, cycle, ph, rc)
        self._cmp(name, self._state(before, addr), after,
                  info=f"rec={rec.hex()} key={key} vel={vel} restart={restart} cyc={cycle} prev={kind}")

    def release(self, addr, before, pedal, rc, after, name="release 45dfb4"):
        self._load(self.obj, before, addr)
        self.stats[f"release pedal={int(pedal != 0)} rounding={RC_NAMES[rc]}"] += 1
        self.L.sq8l_env_release(self.obj, pedal, rc)
        self._cmp(name, self._state(before, addr), after, info=f"pedal={pedal}")

    def tick(self, addr, before, pedal, rc, after, out, name="tick 45e210"):
        self._load(self.obj, before, addr)
        self.stats[f"tick pedal={int(pedal != 0)} rounding={RC_NAMES[rc]}"] += 1
        ours = self.L.sq8l_env_tick(self.obj, pedal, rc)
        self._cmp(name, self._state(before, addr), after, ours, out, info=f"pedal={pedal} before={before.hex()}")

    def shaped(self, addr, before, after, out, name="shaped 45e1d4"):
        self._load(self.obj, before, addr)
        ours = self.L.sq8l_env_shaped(self.obj)
        self._cmp(name, self._state(before, addr), after, ours, out)


def install_env_traces(e, chk):
    def rd(a):
        return e.read(a, ENV_SIZE)

    def ctor_in(emu):
        return f32_from_bits(stack_arg(emu, 0)), fpcw_rc(emu)

    def ctor_out(emu, ctx):
        obj = emu.regs()["eax"]
        chk.ctor(obj, ctx[0], ctx[1], rd(obj))

    def rate_in(emu):
        obj = emu.regs()["eax"]
        return obj, rd(obj), f32_from_bits(stack_arg(emu, 0)), fpcw_rc(emu)

    def rate_out(emu, ctx):
        obj, before, rate, rc = ctx
        chk.set_rate(obj, before, rate, rc, rd(obj))

    def start_in(emu):
        r = emu.regs()
        obj, prev = r["eax"], stack_arg(emu, 0)
        prev_bytes = rd(prev) if prev and prev != obj else None
        return (obj, rd(obj), e.read(r["edx"], 14), s32(r["ecx"]), s32(stack_arg(emu, 3)),
                stack_arg(emu, 2) & 0xFF, stack_arg(emu, 1) & 0xFF, prev, prev_bytes, fpcw_rc(emu))

    def start_out(emu, ctx):
        obj, before, rec, key, vel, restart, cycle, prev, prev_bytes, rc = ctx
        chk.start(obj, before, rec, key, vel, restart, cycle, prev, prev_bytes, rc, rd(obj))

    def rel_in(emu):
        r = emu.regs()
        return r["eax"], rd(r["eax"]), r["edx"] & 0xFF, fpcw_rc(emu)

    def rel_out(emu, ctx):
        obj, before, pedal, rc = ctx
        chk.release(obj, before, pedal, rc, rd(obj))

    def tick_in(emu):
        r = emu.regs()
        return r["eax"], rd(r["eax"]), r["edx"] & 0xFF, fpcw_rc(emu)

    def tick_out(emu, ctx):
        obj, before, pedal, rc = ctx
        chk.tick(obj, before, pedal, rc, rd(obj), s32(emu.regs()["eax"]))

    def shp_in(emu):
        obj = emu.regs()["eax"]
        return obj, rd(obj)

    def shp_out(emu, ctx):
        obj, before = ctx
        chk.shaped(obj, before, rd(obj), s32(emu.regs()["eax"]))

    return [e.trace(ENV_CTOR, ctor_in, ctor_out),
            e.trace(ENV_SET_RATE, rate_in, rate_out),
            e.trace(ENV_START, start_in, start_out),
            e.trace(ENV_RELEASE, rel_in, rel_out),
            e.trace(ENV_TICK, tick_in, tick_out),
            e.trace(ENV_SHAPED, shp_in, shp_out)]


# ------------------------------------------------------------------ direct calls on random states
def random_env_state(rng, addr, shape_ok=True):
    def pick(common, extreme):
        return extreme() if rng.random() < 0.05 else common()

    def i32():
        return rng.randint(-(1 << 31), (1 << 31) - 1)

    b = bytearray(ENV_SIZE)
    struct.pack_into("<I", b, 0, 0x45DB78)
    for i in range(5):
        struct.pack_into("<i", b, 0x04 + 4 * i, pick(lambda: rng.randint(-258, 254), i32))
    for i in range(4):
        struct.pack_into("<i", b, 0x18 + 4 * i, pick(lambda: rng.randint(0, 63), lambda: rng.randint(-100, 200)))
    scale = pick(lambda: rng.randint(-256, 260), i32)
    shape = rng.randint(-1, 7)
    table = 0 if shape < 0 or rng.random() < 0.2 else SHAPE_TABLES + 256 * rng.randint(0, 7)
    if table and scale in (0, -1):
        scale = 252
    struct.pack_into("<ii", b, 0x28, scale, pick(lambda: rng.randint(-64, 64), i32))
    b[0x30] = pick(lambda: rng.randint(0, 1), lambda: rng.randint(0, 255))
    b[0x31] = pick(lambda: rng.randint(0, 1), lambda: rng.randint(0, 255))
    value = pick(lambda: rng.randint(-130 * 256, 130 * 256), i32)
    struct.pack_into("<iiii", b, 0x34, value, pick(lambda: rng.randint(-130, 130), i32),
                     pick(lambda: rng.randint(0, 0x8000), i32), rng.choice([-1, 0, 1, 2, rng.randint(1, 3000)]))
    struct.pack_into("<i", b, 0x44, pick(lambda: rng.randint(-300, 300), lambda: rng.randint(-(1 << 22), 1 << 22)))
    a = rng.randint(0, 256)
    struct.pack_into("<iiii", b, 0x48, rng.choice([0, -3, rng.randint(1, 255)]),
                     pick(lambda: rng.randint(-130 * 256, 130 * 256), i32), a, 256 - a)
    struct.pack_into("<iI", b, 0x58, shape, table)
    for o in range(0x60, 0x65):
        b[o] = pick(lambda: rng.randint(0, 1), lambda: rng.randint(0, 255))
    ramp = 0 if rng.random() < 0.01 else rng.choice(RAMP_CODE)  # 0 = nil method pointer
    nxt = 0 if rng.random() < 0.01 else rng.choice(NEXT_CODE)
    struct.pack_into("<IIII", b, 0x68, ramp, addr if ramp else 0, nxt, addr if nxt else 0)
    rate = rng.choice([CONTROL_RATE, CONTROL_RATE, rng.uniform(1, 400)])
    rscale = rng.choice([1.0, 1.0, rng.uniform(0.01, 4.0), rng.uniform(0.5, 1.5)])
    struct.pack_into("<ff", b, 0x78, rate, rscale)
    return bytes(b)


def fuzz_env(e, L, chk, rng, n):
    obj = e.scratch(ENV_SIZE)
    prev = e.scratch(ENV_SIZE)
    rec_addr = e.scratch(16)
    for i in range(n):
        cw = rng.choice([HOST_FPCW, PROCESS_FPCW])
        rc = (cw >> 10) & 3
        state = random_env_state(rng, obj)
        e.write(obj, state)
        kind = rng.random()
        if kind < 0.45:
            pedal = rng.choice([0, 0, 1, 0x80])
            out, _, _ = e.call_fpu(ENV_TICK, eax=obj, edx=pedal, fpcw=cw)
            chk.tick(obj, state, pedal, rc, e.read(obj, ENV_SIZE), s32(out), name="fuzz tick")
        elif kind < 0.7:
            rec = bytes(rng.randint(0, 255) for _ in range(14)) if rng.random() < 0.5 else random_env_record(rng)
            key = rng.choice([rng.randint(0, 127), rng.randint(-200, 300)])
            vel = rng.choice([rng.randint(0, 127), rng.randint(-200, 300)])
            restart, cycle = rng.choice([0, 1, rng.randint(0, 255)]), rng.choice([0, 1, rng.randint(0, 255)])
            p = rng.choice([0, obj, prev])
            pstate = random_env_state(rng, prev)
            e.write(prev, pstate)
            e.write(rec_addr, rec)
            e.call_fpu(ENV_START, eax=obj, edx=rec_addr, ecx=key, push=(vel, restart, cycle, p), fpcw=cw)
            chk.start(obj, state, rec, key, vel, restart, cycle, p, pstate, rc, e.read(obj, ENV_SIZE),
                      name="fuzz start")
        elif kind < 0.85:
            pedal = rng.choice([0, 1])
            e.call_fpu(ENV_RELEASE, eax=obj, edx=pedal, fpcw=cw)
            chk.release(obj, state, pedal, rc, e.read(obj, ENV_SIZE), name="fuzz release")
        elif kind < 0.97:
            out, _, _ = e.call_fpu(ENV_SHAPED, eax=obj, fpcw=cw)
            chk.shaped(obj, state, e.read(obj, ENV_SIZE), s32(out), name="fuzz shaped")
        else:
            rate = rng.choice([CONTROL_RATE, rng.uniform(0.001, 200000.0), rng.uniform(80, 90),
                               float(rng.randint(1, 100000))])
            rate = struct.unpack("<f", struct.pack("<f", rate))[0]
            e.call_fpu(ENV_SET_RATE, eax=obj, push=(struct.unpack("<I", struct.pack("<f", rate))[0],), fpcw=cw)
            chk.set_rate(obj, state, rate, rc, e.read(obj, ENV_SIZE), name="fuzz setRate")


def sweep_env_rates(e, chk, rng, n):
    """setRate over many control rates in both rounding modes (exact 80-bit division)."""
    obj = e.scratch(ENV_SIZE)
    rates = [CONTROL_RATE, 1.0, 0.5, 44100.0, 48000.0, 96000.0, 83.5925, 83.59258, 167.18515]
    rates += [rng.uniform(1, 1000) for _ in range(n)] + [rng.uniform(1e-3, 1e6) for _ in range(n)]
    for rate in rates:
        bits = struct.unpack("<I", struct.pack("<f", rate))[0]
        rate = f32_from_bits(bits)
        for cw in (HOST_FPCW, PROCESS_FPCW):
            state = random_env_state(rng, obj)
            e.write(obj, state)
            e.call_fpu(ENV_SET_RATE, eax=obj, push=(bits,), fpcw=cw)
            chk.set_rate(obj, state, rate, (cw >> 10) & 3, e.read(obj, ENV_SIZE), name="sweep setRate")


def divide_fault_demo(e, rng):
    """The original faults (#DE) in the shaper when levelScale == 0 with SHAPE on."""
    obj = e.scratch(ENV_SIZE)
    b = bytearray(random_env_state(rng, obj))
    struct.pack_into("<iiI", b, 0x28, 0, 0, 0)  # levelScale = 0
    struct.pack_into("<iI", b, 0x58, 0, SHAPE_TABLES)
    struct.pack_into("<i", b, 0x44, 5)
    e.write(obj, b)
    esp, mark = e.uc.reg_read(UC_X86_REG_ESP), e.scratch_top
    try:
        e.call_fpu(ENV_SHAPED, eax=obj)
        faulted = False
    except RuntimeError:
        faulted = True
    e.uc.reg_write(UC_X86_REG_ESP, esp)
    e.reset_scratch(mark)
    return faulted


# ------------------------------------------------------------------ main
def check_tables(L, e):
    ok = True
    for name, ours, addr, n in [
        ("time table", bytes(ctypes.string_at(L.sq8l_env_time_table(), 128)), 0x4C2090, 128),
        ("shape tables", bytes(ctypes.string_at(L.sq8l_env_shape_tables(), 2048)), SHAPE_TABLES, 2048),
        ("velocity curve L", bytes(ctypes.string_at(L.sq8l_velocity_curve(0), 128)), 0x494D4C, 128),
        ("velocity curve X", bytes(ctypes.string_at(L.sq8l_velocity_curve(1), 128)), VEL_EXP, 128),
    ]:
        same = ours == e.read(addr, n)
        print(f"  {name:18s} {n:5d} bytes {'OK' if same else 'MISMATCH'}")
        ok &= same
    return ok


def main():
    """Full run ~10 min (the emulated plugin with up to 16 voices is the bottleneck);
    --quick: every 4th factory program, 15 random patches (~3 min)."""
    quick = "--quick" in sys.argv
    t0 = time.time()
    L = envlib()
    rng = random.Random(1234)
    h = new_host()
    e = h.emu
    chk = EnvChecker(L, e)
    hooks = install_env_traces(e, chk)
    cov = Coverage(e, *ENV_RANGE)
    h.load()
    h.start()

    print("tables")
    ok = check_tables(L, e)

    print("playing (captures replayed on the fly)", flush=True)
    factory = (BANK_C + BANK_D)[::4] if quick else BANK_C + BANK_D
    played = play_session(h, rng, factory=factory, n_random=15 if quick else 60)
    print("  audio played: " + ", ".join(f"{k} {v:.0f}s" for k, v in played.items()))
    ok &= chk.res.report()
    chk.stats["start prev=self (same voice restruck)"] += 0
    for k in sorted(chk.stats):
        print(f"    {k}: {chk.stats[k]}")

    played_blocks = cov.snapshot()
    for hk in hooks:
        e.untrace(hk)

    print("direct calls on random states", flush=True)
    fchk = EnvChecker(L, e)
    fuzz_env(e, L, fchk, rng, 10000 if quick else 30000)
    sweep_env_rates(e, fchk, rng, 100 if quick else 300)
    ok &= fchk.res.report()

    faulted = divide_fault_demo(e, rng)
    print(f"  shaper with levelScale == 0 faults in the original: {faulted}")

    skip = [(0x45DBA0, 0x45DBCC),   # class metadata (VMT) before the first routine
            (0x45DC14, 0x45DC3C),   # destructor (Csq_env_Destroy)
            (0x45DCD8, 0x45DCE4),   # 80-bit constant
            (0x45E2CC, 0x45E32C)]   # unit initialization / finalization
    for title, blocks in (("real play", played_blocks), ("real play + direct calls", None)):
        print(f"coverage, {title}: never executed (excluding destructor, constant, unit init)")
        runs = cov.uncovered(e, skip, blocks)
        for a, b, first in runs:
            print(f"  {a:#x}..{b:#x}  {first}")
        if not runs:
            print("  none")
    print(f"total {time.time() - t0:.0f}s")
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
