"""Heavy soft voice stealing, added to the regression set (tests/regression): many
overlapping notes in quick succession, so that more stolen voices are fading at once than
the original has fade slots (8). Rendered by the ORIGINAL (emulated DLL, needs original/);
appends the cases and the hashes of the original's output to tests/regression.

Run once: python tests/export_regression_stealing.py
"""
import os
import struct

from export_regression import OUT, fnv1a64
from oracle_render import render_original
from render_scenarios import notes

PROGRAMS = [256 + 0, 256 + 13, 384 + 10, 384 + 3, 256 + 45]
SCENARIOS = {
    "burst10ms": [(i * 0.010, 0.6, 36 + i, 100) for i in range(40)],
    "burst30ms": [(i * 0.030, 0.8, 40 + (i * 7) % 48, 90) for i in range(40)],
    "chords": [(c * 0.15, 0.4, 48 + n * 3 + c, 100) for c in range(6) for n in range(10)],
}


def main():
    lines = []
    for p in PROGRAMS:
        for name, seq in SCENARIOS.items():
            blocks = notes(seq)
            L, R = render_original(p, blocks)
            out = bytearray(struct.pack("<ii", p, len(blocks)))
            for n, evs in blocks:
                out += struct.pack("<ii", n, len(evs))
                for d, b in evs:
                    out += struct.pack("<i", d) + bytes(b) + bytes(4 - len(b))
            case = f"{p}_{name}.bin"
            with open(os.path.join(OUT, "cases", case), "wb") as f:
                f.write(out)
            h = fnv1a64(L.astype("<f4").tobytes() + R.astype("<f4").tobytes())
            lines.append(f"{case} {len(L)} {h:016x}")
            print(lines[-1])
    with open(os.path.join(OUT, "expected.txt"), "a") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
