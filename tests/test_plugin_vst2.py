"""End-to-end test of the BUILT VST2 plugin (build-plugin/bin/SQ8L.vst) through the VST2 ABI,
against the emulated original:

1. renders golden scenarios through the plugin (effSetProgram + effProcessEvents +
   processReplacing) and requires bit-exact output;
2. chunk compatibility: chunks produced by the ORIGINAL (effGetChunk after selecting
   various programs and editing) are loaded into the port with effSetChunk; the port's
   effGetChunk must return identical bytes, and rendering must match the original
   rendering of the same chunk.
"""
import ctypes as C
import glob
import os
import struct
import sys

import numpy as np

from harness import ROOT, SQ8LHost, effSetProgram
from oracle_render import GOLDEN
from render_scenarios import all_scenarios, chord_and_melody

PLUGIN = os.path.join(ROOT, "build-plugin", "bin", "SQ8L.vst", "Contents", "MacOS", "SQ8L")

HOSTCB = C.CFUNCTYPE(C.c_ssize_t, C.c_void_p, C.c_int32, C.c_int32, C.c_ssize_t, C.c_void_p, C.c_float)
DISPATCH = C.CFUNCTYPE(C.c_ssize_t, C.c_void_p, C.c_int32, C.c_int32, C.c_ssize_t, C.c_void_p, C.c_float)
PROCESS = C.CFUNCTYPE(None, C.c_void_p, C.POINTER(C.POINTER(C.c_float)), C.POINTER(C.POINTER(C.c_float)), C.c_int32)


class AEffect(C.Structure):
    _fields_ = [("magic", C.c_int32), ("dispatcher", DISPATCH), ("process", C.c_void_p),
                ("setParameter", C.c_void_p), ("getParameter", C.c_void_p),
                ("numPrograms", C.c_int32), ("numParams", C.c_int32), ("numInputs", C.c_int32),
                ("numOutputs", C.c_int32), ("flags", C.c_int32), ("resvd1", C.c_ssize_t), ("resvd2", C.c_ssize_t),
                ("initialDelay", C.c_int32), ("realQualities", C.c_int32), ("offQualities", C.c_int32),
                ("ioRatio", C.c_float), ("object", C.c_void_p), ("user", C.c_void_p),
                ("uniqueID", C.c_int32), ("version", C.c_int32), ("processReplacing", PROCESS),
                ("processDoubleReplacing", C.c_void_p), ("future", C.c_char * 56)]


class MidiEv(C.Structure):
    _fields_ = [("type", C.c_int32), ("byteSize", C.c_int32), ("deltaFrames", C.c_int32), ("flags", C.c_int32),
                ("noteLength", C.c_int32), ("noteOffset", C.c_int32), ("midiData", C.c_uint8 * 4),
                ("detune", C.c_int8), ("noteOffVelocity", C.c_uint8), ("r1", C.c_int8), ("r2", C.c_int8)]


def host_cb(effect, opcode, index, value, ptr, opt):
    if opcode == 1:      # version
        return 2400
    if opcode == 16:     # sample rate
        return 44100
    if opcode == 17:
        return 512
    return 0


_cb = HOSTCB(host_cb)


class Port:
    def __init__(self):
        self.lib = C.CDLL(PLUGIN)
        main = self.lib.VSTPluginMain
        main.restype = C.POINTER(AEffect)
        main.argtypes = [HOSTCB]
        self.eff = main(_cb)
        self.e = self.eff.contents
        assert self.e.magic == 0x56737450
        self.d(0)                       # effOpen
        self.d(10, opt=44100.0)          # sample rate
        self.d(11, value=512)            # block size
        self.d(12, value=1)              # resume
        self.d(71)                       # start process

    def d(self, op, index=0, value=0, ptr=None, opt=0.0):
        return self.e.dispatcher(self.eff, op, index, value, ptr, opt)

    def close(self):
        self.d(12, value=0)
        self.d(1)

    def midi(self, events):
        n = len(events)
        evs = (MidiEv * n)()
        ptrs = (C.c_void_p * n)()
        for i, (delta, data) in enumerate(events):
            evs[i].type, evs[i].byteSize, evs[i].deltaFrames = 1, 32, delta
            evs[i].midiData[:] = list(bytes(data) + bytes(4 - len(data)))
            ptrs[i] = C.addressof(evs[i])
        buf = C.create_string_buffer(8 + C.sizeof(C.c_void_p) * (n + 2))
        struct.pack_into("<i", buf, 0, n)
        C.memmove(C.addressof(buf) + 16 if C.sizeof(C.c_void_p) == 8 else C.addressof(buf) + 8, ptrs, C.sizeof(ptrs))
        self.d(25, ptr=C.cast(buf, C.c_void_p))

    def render(self, blocks):
        L, R = [], []
        for n, events in blocks:
            if events:
                self.midi(events)
            l, r = (C.c_float * n)(), (C.c_float * n)()
            outs = (C.POINTER(C.c_float) * 2)(C.cast(l, C.POINTER(C.c_float)), C.cast(r, C.POINTER(C.c_float)))
            ins = (C.POINTER(C.c_float) * 2)()
            self.e.processReplacing(self.eff, ins, outs, n)
            L.append(np.frombuffer(l, np.float32).copy())
            R.append(np.frombuffer(r, np.float32).copy())
        return np.concatenate(L), np.concatenate(R)

    def get_chunk(self):
        p = C.c_void_p()
        n = self.d(23, ptr=C.byref(p))
        return C.string_at(p, n)

    def set_chunk(self, data):
        b = C.create_string_buffer(bytes(data), len(data))
        return self.d(24, value=len(data), ptr=C.cast(b, C.c_void_p))


def main():
    ok = True
    scen = all_scenarios()
    port = Port()
    meta = (struct.pack('>i', port.e.uniqueID), port.e.numPrograms, port.e.numOutputs, port.e.flags)
    port.close()
    print(f"plugin: uniqueID {meta[0]}, programs {meta[1]}, outputs {meta[2]}, flags {meta[3]:#x} "
          f"(original: b'SQ8L', 512, 2, 0x131)")
    ok &= meta[:3] == (b"SQ8L", 512, 2)
    files = sorted(glob.glob(os.path.join(GOLDEN, "*.npz")))[:21]
    n_ok = 0
    for f in files:
        prog, name = os.path.basename(f)[:-4].split("_", 1)
        g = np.load(f)
        inst = Port()                # fresh instance per scenario, like the golden renders
        inst.d(2, value=int(prog))   # effSetProgram
        L, R = inst.render(scen[name]())
        inst.close()
        same = np.array_equal(L.view(np.uint32), g["L"].view(np.uint32)) and np.array_equal(R.view(np.uint32), g["R"].view(np.uint32))
        n_ok += same
        if not same:
            print(f"  {prog} {name}: differs (max {np.abs(L - g['L']).max():.3g})")
    print(f"renders through the VST2 plugin: {n_ok}/{len(files)} bit-exact")
    ok &= n_ok == len(files)

    # chunk compatibility with the original
    h = SQ8LHost()
    h.load()
    h.start()
    e = h.emu
    def orig_get_chunk():
        pp = e.scratch(4)
        n = h.dispatch(23, ptr=pp)
        return e.read(e.u32(pp), n)

    chunks = []
    for p in (0, 45, 256 + 13, 384 + 10, 300, 511):
        h.dispatch(effSetProgram, value=p)
        chunks.append((p, orig_get_chunk()))
    port = Port()
    n_ok = 0
    for p, ch in chunks:
        # original round trip: setChunk then getChunk (setChunk marks the program modified)
        buf = e.scratch(len(ch))
        e.write(buf, ch)
        h.dispatch(24, value=len(ch), ptr=buf)
        expected = orig_get_chunk()
        port.set_chunk(ch)
        back = port.get_chunk()
        same = back == expected
        n_ok += same
        if not same:
            diff = [i for i in range(len(ch)) if back[i] != expected[i]][:8]
            print(f"  chunk of program {p}: differs at {diff}")
    print(f"original chunks round-tripped through the port: {n_ok}/{len(chunks)} identical ({len(chunks[0][1])} bytes)")
    ok &= n_ok == len(chunks)
    # render after loading an original chunk (what reopening an old project does)
    p, ch = chunks[2]
    h2 = SQ8LHost()
    h2.load()
    h2.start()
    buf = h2.emu.scratch(len(ch))
    h2.emu.write(buf, ch)
    h2.dispatch(24, value=len(ch), ptr=buf)
    blocks = chord_and_melody()
    oL, oR = [], []
    for n, evs in blocks:
        if evs:
            h2.send_midi(evs)
        l, r = h2.process(n)
        oL.append(np.array(l, np.float32))
        oR.append(np.array(r, np.float32))
    oL, oR = np.concatenate(oL), np.concatenate(oR)
    port2 = Port()
    port2.set_chunk(ch)
    L, R = port2.render(blocks)
    same = np.array_equal(L.view(np.uint32), oL.view(np.uint32)) and np.array_equal(R.view(np.uint32), oR.view(np.uint32))
    print(f"render after loading an original chunk (program {p}): {'bit-exact' if same else 'DIFFERS'}")
    ok &= same
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
