"""Differential test: SQ-80/ESQ-1 SysEx conversion (unit_453adc / unit_450db8) vs C++."""
import ctypes
import random
import struct
import sys

import pefile

import test_program as tp
from harness import ROOT, started_host

PROG = 0x21C
SRC_FROM_SQ80 = 0x453B14     # al -> ax
SRC_TO_SQ80 = 0x453BA0       # ax -> al
NAME_FROM = 0x4510D4         # (chars, n, var AnsiString)
NAME_TO = 0x45113C           # (AnsiString, n) -> GetMem'd buffer
FROM_NYBBLES = 0x454054      # (src, prog)
TO_NYBBLES = 0x4545DC        # (prog, dst, esq1)
FROM_SQ80 = 0x454AEC         # (raw, prog, f192, f196, f13b, level)
SQ80_FACTORY = 0x493324      # 40 * 102 bytes


def ansistring(e, s):
    b = s.encode("latin1")
    a = e.scratch(struct.pack("<iI", -1, len(b)) + b + b"\0")
    return a + 8 if b else 0


def read_ansistring(e, p):
    return e.read(p, e.u32(p - 4)) if p else b""


def main():
    api = tp.api()
    tp.API = api
    rng = random.Random(99)
    h = started_host()
    e = h.emu
    ok = True
    call = tp.emu_call

    # ---- mod source mapping (exhaustive)
    bad = 0
    for s in range(256):
        ax, _ = call(e, SRC_FROM_SQ80, s)
        bad += (ax & 0xFFFF) != (api.sq8l_sysex_source_from(s) & 0xFFFF)
    for s in range(-32768, 32768):
        al, _ = call(e, SRC_TO_SQ80, s & 0xFFFF)
        bad += (al & 0xFF) != api.sq8l_sysex_source_to(s)
    print("mod source mapping: 256 + 65536 values", "OK" if not bad else f"{bad} MISMATCHES")
    ok &= bad == 0

    # ---- names
    mark = e.scratch_top
    bad = 0
    out = ctypes.create_string_buffer(64)
    for k in range(3000):
        n = rng.randrange(0, 8)
        chars = bytes(rng.choice([rng.randrange(256), rng.randrange(0x20, 0x60), 0x21, 0x23, 0x25, 0x28,
                                  0x29, 0x3a, 0x3b, 0x5b, 0x5c, 0x5d]) for _ in range(n))
        src = e.scratch(chars + b"\0")
        var = e.scratch(4)
        call(e, NAME_FROM, src, n, var)
        want = read_ansistring(e, e.u32(var))
        ln = api.sq8l_sysex_name_from(chars, n, out, 64)
        bad += out.raw[:ln] != want
        e.reset_scratch(mark)
    alphabet = "ABCXYZabcxyz0123456789.. -+*/!#%()[]\\^_`{}~\x7f\x80\xe9:;'\""
    for k in range(3000):
        s = "".join(rng.choice(alphabet) for _ in range(rng.randrange(0, 16)))
        n = rng.choice([6, 6, 6, 1, 3, 8, 12])
        a = ansistring(e, s)
        p, _ = call(e, NAME_TO, a, n)
        want = e.read(p, n) if p else bytes(n)
        got = ctypes.create_string_buffer(n)
        api.sq8l_sysex_name_to(s.encode("latin1"), len(s), n, got)
        bad += got.raw != want
        e.reset_scratch(mark)
    print("name conversion: 6000 strings", "OK" if not bad else f"{bad} MISMATCHES")
    ok &= bad == 0

    # ---- programs: library -> nybbles (all 512 programs + random records), both esq1 modes
    lib = e.u32(tp.LIB_GLOBAL)
    progs = [e.read(lib + 0x20 + i * PROG, PROG) for i in range(512)]
    rnd = []
    for k in range(600):
        r = bytearray(rng.randrange(256) for _ in range(PROG))
        r[2] = rng.randrange(0, 40)  # names longer than 15 read into the next field
        rnd.append(bytes(r))
    pa, da = e.scratch(PROG), e.scratch(0xCC)
    bad = 0
    nyb_samples = []
    for i, pr in enumerate(progs + rnd):
        for esq1 in (0, 1):
            e.write(pa, pr)
            e.write(da, bytes(rng.randrange(256) for _ in range(0xCC)))
            call(e, TO_NYBBLES, pa, da, esq1)
            want = e.read(da, 0xCC)
            got = ctypes.create_string_buffer(0xCC)
            api.sq8l_sysex_to_nybbles(pr, got, esq1)
            bad += got.raw != want
            if esq1 == 0:
                nyb_samples.append(want)
    print(f"program -> SysEx: {2 * (512 + len(rnd))} conversions", "OK" if not bad else f"{bad} MISMATCHES")
    ok &= bad == 0

    # ---- nybbles -> program: exported programs, random nybbles, random bytes
    for k in range(400):
        nyb_samples.append(bytes(rng.randrange(16) for _ in range(0xCC)))
        nyb_samples.append(bytes(rng.randrange(256) for _ in range(0xCC)))
    sa, pa2 = e.scratch(0xCC), e.scratch(PROG)
    bad = 0
    for nyb in nyb_samples:
        e.write(sa, nyb)
        before = bytes(rng.randrange(256) for _ in range(PROG))
        e.write(pa2, before)
        call(e, FROM_NYBBLES, sa, pa2)
        got = tp.buf(before)
        api.sq8l_sysex_from_nybbles(nyb, got)
        bad += got.raw != e.read(pa2, PROG)
    print(f"SysEx -> program: {len(nyb_samples)} conversions", "OK" if not bad else f"{bad} MISMATCHES")
    ok &= bad == 0

    # SysEx -> program -> SysEx of every library program: stable except for the DCA level/enable
    # of oscillators 1-2 (nybbles 0x80/0x81, 0x94/0x95), which the import forces when AM is on.
    bad = 0
    am_forced = 0
    for pr in progs:
        n1 = ctypes.create_string_buffer(0xCC)
        api.sq8l_sysex_to_nybbles(pr, n1, 0)
        back = tp.buf(PROG)
        api.sq8l_sysex_from_nybbles(n1.raw, back)
        n2 = ctypes.create_string_buffer(0xCC)
        api.sq8l_sysex_to_nybbles(back.raw, n2, 0)
        diff = {j for j in range(0xCC) if n1.raw[j] != n2.raw[j]}
        am_forced += bool(diff)
        bad += not diff <= {0x80, 0x81, 0x94, 0x95}
    print(f"SysEx round trip (512 programs, {am_forced} with AM-forced osc levels):",
          "OK" if not bad else f"{bad} MISMATCHES")
    ok &= bad == 0

    # ---- raw SQ-80 bytes -> program (bank D factory data + random)
    pe = pefile.PE(ROOT + "/original/SQ8L.dll")
    raw = pe.get_data(SQ80_FACTORY - pe.OPTIONAL_HEADER.ImageBase, 40 * 0x66)
    cases = [(raw[i * 0x66:(i + 1) * 0x66], 1, 0, 4, -12) for i in range(40)]
    for k in range(600):
        cases.append((bytes(rng.randrange(256) for _ in range(0x66)), rng.randrange(256), rng.randrange(256),
                      rng.randrange(256), rng.randrange(-128, 128)))
    ra, pa3 = e.scratch(0x66), e.scratch(PROG)
    bad = 0
    bank_d_ok = True
    for idx, (r, a, b, c, lvl) in enumerate(cases):
        e.write(ra, r)
        before = bytes(rng.randrange(256) for _ in range(PROG))
        e.write(pa3, before)
        call(e, FROM_SQ80, ra, pa3, a, b, c, lvl & 0xFFFFFFFF)
        want = e.read(pa3, PROG)
        got = tp.buf(before)
        api.sq8l_sysex_from_sq80(r, got, a, b, c, lvl)
        bad += got.raw != want
        if idx < 40:
            bank_d_ok &= want == progs[384 + idx]
    print(f"SQ-80 bytes -> program: {len(cases)} conversions", "OK" if not bad else f"{bad} MISMATCHES")
    print("bank D = conversion of the 40 SQ-80 factory programs:", "OK" if bank_d_ok else "MISMATCH")
    ok &= bad == 0 and bank_d_ok

    # ---- headers
    t = ctypes.c_int32()
    good = api.sq8l_sysex_parse_header(bytes([0xF0, 0x0F, 0x02, 0x00, 0x01]), ctypes.byref(t)) == 1 and t.value == 1
    good &= api.sq8l_sysex_parse_header(bytes([0xF0, 0x0F, 0x03, 0x00, 0x01]), ctypes.byref(t)) == 0
    print("headers:", "OK" if good else "MISMATCH")
    ok &= good

    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
