"""End-to-end test of the BUILT VST2 plugin (build-plugin/bin/SQ8L.vst) through the VST2 ABI,
against the emulated original:

1. renders golden scenarios through the plugin (effSetProgram + effProcessEvents +
   processReplacing) and requires bit-exact output;
2. chunk compatibility: chunks produced by the ORIGINAL (effGetChunk after selecting
   various programs and editing) are loaded into the port with effSetChunk; the port's
   effGetChunk must return identical bytes, and rendering must match the original
   rendering of the same chunk.
"""
import argparse
import ctypes as C
import glob
import os
import struct
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

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


_host_observer = None


def host_cb(effect, opcode, index, value, ptr, opt):
    if _host_observer is not None:
        _host_observer(effect, opcode, index, value, ptr, opt)
    if opcode == 1:      # version
        return 2400
    if opcode == 16:     # sample rate
        return 44100
    if opcode == 17:
        return 512
    return 0


_cb = HOSTCB(host_cb)


class Port:
    def __init__(self, plugin=None, processing=True, host_callback=None):
        self.lib = C.CDLL(plugin or PLUGIN)
        main = self.lib.VSTPluginMain
        main.restype = C.POINTER(AEffect)
        main.argtypes = [HOSTCB]
        self.host_callback = host_callback or _cb
        self.eff = main(self.host_callback)
        self.e = self.eff.contents
        assert self.e.magic == 0x56737450
        self.d(0)                       # effOpen
        self.d(10, opt=44100.0)          # sample rate
        self.d(11, value=512)            # block size
        self.processing = processing
        if processing:
            self.d(12, value=1)          # resume
            self.d(71)                   # start process

    def d(self, op, index=0, value=0, ptr=None, opt=0.0):
        return self.e.dispatcher(self.eff, op, index, value, ptr, opt)

    def close(self):
        if self.processing:
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
    # Oracle dependencies remain exclusive to the original differential mode.
    from harness import SQ8LHost, effSetProgram
    from oracle_render import GOLDEN
    from render_scenarios import all_scenarios, chord_and_melody
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


def automation_render(plugin, rows):
    """Actual callbacks; audio, rather than a later save, observes application."""
    import hashlib
    assert C.sizeof(MidiEv) == 32 and MidiEv.midiData.offset == 24
    by_symbol = {row["symbol"]: row for row in rows}

    def edit(port, symbol, native):
        row = by_symbol[symbol]
        value = (native - row["min"]) / (row["max"] - row["min"])
        setter = C.CFUNCTYPE(None, C.c_void_p, C.c_int32, C.c_float)(port.e.setParameter)
        setter(port.eff, row["index"], value)

    def equal(a, b):
        return all(np.array_equal(x.view(np.uint32), y.view(np.uint32)) for x, y in zip(a, b))

    def observe(label, audio):
        assert all(np.isfinite(channel).all() for channel in audio), label
        peak = max(float(np.max(np.abs(channel))) for channel in audio)
        assert peak > 1e-5, (label, "inaudible control", peak)
        digest = hashlib.sha256(b"".join(channel.tobytes() for channel in audio)).hexdigest()
        print(f"  {label}: peak={peak:.9g}, stereo SHA256={digest}")

    # A fresh instance is explicitly loaded through effSetProgram. No editor,
    # installed library or oracle is involved: 269 is the built-in bank C lead.
    # All lanes start from the same legacy chunk and identical DSP history.
    for offset in (0, 127):
        ports = [Port(plugin) for _ in range(4)]
        try:
            ports[0].d(2, value=269)
            base = ports[0].get_chunk()
            assert len(base) == 571
            for port in ports:
                port.set_chunk(base)
            # Independent manual-equivalent legacy wire edit: 31-byte chunk
            # header + 34-byte Program header + oscillator1 signed SEMI byte.
            # Unlike setter/getChunk, this reference never exercises accept/apply.
            for port, semi in ((ports[1], 12), (ports[3], -12)):
                manual = bytearray(base)
                manual[31 + 34 + 1] = semi & 255
                port.set_chunk(manual)
            for semi in (-12, 0, 12):
                edit(ports[0], "oscillator1_semi", semi)
            # Nothing that synchronizes state is called on the pending lane
            # between its setters and processReplacing, including no getChunk.
            blocks = [(512, [(offset, b"\x90\x3c\x64")])] + [(512, [])] * 7
            audio = [port.render(blocks) for port in ports]
            for i, output in enumerate(audio):
                observe(f"note offset={offset} lane={i}", output)
            assert equal(audio[0], audio[1]), (offset, "pending versus manual")
            assert not equal(audio[0], audio[2]), (offset, "unchanged control sensitivity")
            assert not equal(audio[0], audio[3]), (offset, "intermediate control sensitivity")
            first = [tuple(channel[:512] for channel in output) for output in audio]
            assert not equal(first[0], first[2]) and not equal(first[0], first[3]), (offset, "first-call sensitivity")
            assert all(np.count_nonzero(channel[:offset]) == 0 for channel in audio[0])
            print(f"PASS: render-entry before MIDI offset {offset}; latest of 3 setters equals manual +12, differs from unchanged and intermediate -12")
        finally:
            for port in ports:
                port.close()

    # Same already-running trajectory in three lanes. Explicit stopped snapshot
    # application is the reference boundary and does not reset any voices; the
    # third lane omits this edit, proving sensitivity for this particular block.
    ports = [Port(plugin) for _ in range(3)]
    try:
        ports[0].d(2, value=269)
        base = ports[0].get_chunk()
        for port in ports:
            port.set_chunk(base)
        warmup = [(512, [(0, b"\x90\x3c\x64")])] + [(512, [])] * 3
        warm = [port.render(warmup) for port in ports]
        assert equal(warm[0], warm[1]) and equal(warm[0], warm[2])
        observe("identical sustained warmup", warm[0])
        for phase, value, events in (("sustain", 63, []),
                                     ("release", -63, [(127, b"\x80\x3c\x00")])):
            for port in ports[:2]:
                for native in (0, -value, value):
                    edit(port, "final_pan", native)
            ports[1].get_chunk()  # deliberately apply BEFORE run in reference only
            blocks = [(512, events)] + [(512, [])] * 3
            audio = [port.render(blocks) for port in ports]
            for i, output in enumerate(audio):
                observe(f"{phase} lane={i}", output)
            assert equal(audio[0], audio[1]), (phase, "pending versus explicit pre-application")
            assert not equal(audio[0], audio[2]), (phase, "unedited control sensitivity")
            first = [tuple(channel[:512] for channel in output) for output in audio]
            assert not equal(first[0], first[2]), (phase, "first-call sensitivity")
            if phase == "release":
                assert max(float(np.max(np.abs(channel[512:]))) for channel in audio[0]) > 1e-5
            print(f"PASS: between-call {phase} pan coalescing; pending run equals pre-applied reference, differs from unedited control")
    finally:
        for port in ports:
            port.close()


def automation_only(plugin):
    """Real ABI, stopped-state plus render checks, no editor/oracle dependencies."""
    # Independent checked-in registry audit, not private research or plugin metadata.
    import json
    fixture = os.path.join(ROOT, "tests", "fixtures", "sound_parameter_registry.json")
    with open(fixture) as stream:
        rows = json.load(stream)["parameters"]
    port = Port(plugin, processing=False)
    try:
        assert port.e.numParams == 256
        assert port.e.numPrograms == 512
        setter = C.CFUNCTYPE(None, C.c_void_p, C.c_int32, C.c_float)(port.e.setParameter)
        getter = C.CFUNCTYPE(C.c_float, C.c_void_p, C.c_int32)(port.e.getParameter)
        original = port.get_chunk()
        assert len(original) == 571 and original[5:14] == b"SQ8L.EDIT"
        # Legacy loading marks a sound modified. Establish that original behavior
        # before byte-exact comparisons instead of masking a differing header byte.
        port.set_chunk(original)
        original = port.get_chunk()
        # Every permanent identity must be named and automatable while stopped.
        for index in range(256):
            name = C.create_string_buffer(256)
            port.d(8, index=index, ptr=name)
            assert name.value, index
            assert port.d(26, index=index) == 1, index
        # Exhaust every native legal value with render/editor absent. This also
        # catches accidental double normalization, missing domains and stale getters.
        choices = 0
        for row in rows:
            index, lo, hi = row["index"], row["min"], row["max"]
            for native in range(lo, hi + 1):
                normalized = (native - lo) / (hi - lo)
                setter(port.eff, index, normalized)
                assert abs(getter(port.eff, index) - normalized) < 2e-6, (index, native)
                choices += 1
        port.set_chunk(original)
        # These native domains are fixed public contracts from the independent fixture.
        # Native stepping is observed via normalized getter, including signed center.
        by_symbol = {r["symbol"]: r for r in rows}
        for symbol, native_values in (
            ("dca1_level", (0, 1, 31, 63)),
            ("oscillator1_semi", (-12, -1, 0, 1, 12)),
            ("final_pan", (-63, 0, 63)),
        ):
            row = by_symbol[symbol]
            index = int(row["index"])
            lo, hi = int(row["min"]), int(row["max"])
            for value in native_values:
                normalized = (value - lo) / (hi - lo)
                setter(port.eff, index, normalized)
                assert abs(getter(port.eff, index) - normalized) < 2e-6, (symbol, value)
                saved = port.get_chunk()  # stopped save synchronizes accepted sound
                port.set_chunk(original)
                port.set_chunk(saved)
                assert abs(getter(port.eff, index) - normalized) < 2e-6, (symbol, value, "restore")
        # Explicit inactive/free values must extend state, survive restore, and retain the base.
        pair = next(r for r in rows if r["symbol"].endswith("_free"))
        index = int(pair["index"])
        setter(port.eff, index, 0.25)
        saved = port.get_chunk()
        accepted = getter(port.eff, index)
        assert len(saved) > 571
        port.set_chunk(original)
        port.set_chunk(saved)
        assert abs(getter(port.eff, index) - accepted) < 2e-6
        port.set_chunk(original)
        restored = port.get_chunk()
        assert restored == original, [(i, a, b) for i, (a, b) in enumerate(zip(restored, original)) if a != b]
        print(f"PASS: 256 named automatable parameters; {choices} native choices; stopped stepping/get/save/base+extended restore")
    finally:
        port.close()
    automation_invalid_numbers(plugin, rows)
    automation_render(plugin, rows)
    automation_refresh(plugin, rows)
    automation_idle(plugin)
    return 0


def automation_invalid_numbers(plugin, rows):
    """Reject host NaN/infinities before switch coercion, including unset pairs."""
    global _host_observer
    port = Port(plugin, processing=False)
    setter = C.CFUNCTYPE(None, C.c_void_p, C.c_int32, C.c_float)(port.e.setParameter)
    getter = C.CFUNCTYPE(C.c_float, C.c_void_p, C.c_int32)(port.e.getParameter)
    callbacks = []
    try:
        original = port.get_chunk()
        port.set_chunk(original)
        original = port.get_chunk()
        for rendering in (False, True):
            for row in rows:
                index = row["index"]
                for invalid in (float("nan"), float("inf"), -float("inf")):
                    port.set_chunk(original)  # fresh, unset Free/Sync intent
                    if row["min"] == 0 and row["max"] == 1:
                        # NaN/-inf previously became low; +inf became high.
                        setter(port.eff, index, 0 if invalid == float("inf") else 1)
                    baseline = port.get_chunk()
                    before = [getter(port.eff, i) for i in range(256)]
                    port.d(53)  # drain replacement notification before observing
                    callbacks.clear()
                    _host_observer = lambda effect, opcode, *args: callbacks.append(opcode)
                    setter(port.eff, index, invalid)
                    assert [getter(port.eff, i) for i in range(256)] == before, (index, invalid)
                    if rendering:
                        port.render([(128, [])])
                    else:
                        port.render([(0, [])])
                    assert [getter(port.eff, i) for i in range(256)] == before
                    assert port.get_chunk() == baseline, (index, invalid, rendering, "state/intent")
                    port.d(53)
                    assert not any(op in (0, 42, 43, 44) for op in callbacks), callbacks
                    _host_observer = None
        for row in rows:
            if row["min"] == 0 and row["max"] == 1:
                for value, expected in ((-1, 0), (0.5, 0), (0.5001, 1), (2, 1)):
                    setter(port.eff, row["index"], value)
                    assert getter(port.eff, row["index"]) == expected
        level = next(row for row in rows if row["symbol"] == "dca1_level")
        for value, expected in ((-1, 0), (0.5, 32/63), (2, 1)):
            setter(port.eff, level["index"], value)
            assert abs(getter(port.eff, level["index"]) - expected) < 2e-6
        print("PASS: VST2 all256 NaN/+inf/-inf ignored; stopped/render, opposite booleans, unset pairs, getters/state, no display/gestures")
    finally:
        _host_observer = None
        port.close()


def automation_refresh(plugin, rows, enabled=True):
    """Actual stopped/no-editor display invalidation and callback order."""
    import threading
    global _host_observer
    port = Port(plugin, processing=False)
    getter = C.CFUNCTYPE(C.c_float, C.c_void_p, C.c_int32)(port.e.getParameter)
    setter = C.CFUNCTYPE(None, C.c_void_p, C.c_int32, C.c_float)(port.e.setParameter)
    callbacks, errors, readers = [], [], []
    owner = threading.get_ident()

    def observe(effect, opcode, index, value, ptr, opt):
        callbacks.append(opcode)
        if opcode in (0, 43, 44):
            errors.append("refresh emitted recorded automation/gesture")
        if opcode != 42:
            return
        if threading.get_ident() != owner:
            errors.append("display callback changed thread")
        # A distinct thread must be able to acquire engine ownership before the
        # callback returns. A recursive same-thread getter would miss this defect.
        done = threading.Event()
        def read():
            values = [getter(port.eff, i) for i in range(256)]
            if not all(0 <= v <= 1 for v in values):
                errors.append("invalid display snapshot")
            done.set()
        reader = threading.Thread(target=read)
        readers.append(reader)
        reader.start()
        if not done.wait(2):
            errors.append("host callback under engine ownership")

    try:
        # Initialize the cursor before observing replacement invalidation.
        port.d(53)
        _host_observer = observe
        original = port.get_chunk()
        # Real stopped program route, with no UI or processing/idle progress.
        port.d(2, value=269)
        assert callbacks == ([42] if enabled else []), callbacks
        loaded = port.get_chunk()
        assert original != loaded
        # Both legacy and extended raw chunk replacements invalidate once.
        for chunk in (original, loaded, original):
            callbacks.clear()
            port.set_chunk(chunk)
            assert callbacks == ([42] if enabled else []), callbacks
            port.d(53)
            assert callbacks == ([42] if enabled else []), ("duplicate idle refresh", callbacks)
        pair = next(r for r in rows if r["symbol"].endswith("_free"))
        index = pair["index"]
        # An equal host assignment still has explicit saved intent.
        equal = getter(port.eff, index)
        callbacks.clear()
        setter(port.eff, index, equal)
        assert callbacks == [], ("setter recorded/invalidated display", callbacks)
        explicit = port.get_chunk()
        assert len(explicit) == 595
        assert explicit[571:579] == b"SQ8LAUTO"
        assert struct.unpack_from("<I", explicit, 587)[0] == index
        for _ in range(2):
            port.set_chunk(explicit)
            assert port.get_chunk() == explicit
        # Callback order: a new chunk supersedes pending host edits; later host
        # edits supersede that chunk, including stopped save/readback.
        ordinary = next(r for r in rows if r["symbol"] == "oscillator1_semi")
        i = ordinary["index"]
        for pending, final in ((0.0, 1.0), (1.0, 0.0)):
            setter(port.eff, i, pending)
            port.set_chunk(original)
            baseline = getter(port.eff, i)
            setter(port.eff, i, final)
            assert abs(getter(port.eff, i) - final) < 2e-6
            saved = port.get_chunk()
            port.set_chunk(original)
            assert abs(getter(port.eff, i) - baseline) < 2e-6
            port.set_chunk(saved)
            assert abs(getter(port.eff, i) - final) < 2e-6
        # Incidental program selection after a chunk remains suppressed.
        port.set_chunk(explicit)
        callbacks.clear()
        port.d(2, value=13)
        assert port.get_chunk() == explicit
        assert callbacks == [], callbacks
        assert not any(op in callbacks for op in (0, 43, 44))
        assert not errors, errors
        print(f"PASS: VST2 refresh opt-in={enabled}; stopped/no-editor program+raw chunks; callback getter; equal intent; repeated restore; callback precedence; incidental program suppression; no recorded gestures")
    finally:
        _host_observer = None
        for reader in readers:
            reader.join(timeout=3)
        port.close()


def automation_idle(plugin, enabled=True):
    """ABI host timer model: NeedIdle starts, zero effIdle stops, close removes.

    Matches JUCE 6.1.6's independent 50ms plugin timer, without opening an editor.
    Time advances explicitly on the control thread; no editor-idle is injected.
    """
    for request_result, cooperative in ((1, True), (0, True), (0, False)):
        scheduled = {}
        callbacks, errors, cache = [], [], []
        rendering = False
        requests = []
        host_attached = False
        getter_type = C.CFUNCTYPE(C.c_float, C.c_void_p, C.c_int32)

        def snapshot(effect):
            e = C.cast(effect, C.POINTER(AEffect)).contents
            get = getter_type(e.getParameter)
            return [get(effect, i) for i in range(256)]

        def host(effect, opcode, index, value, ptr, opt):
            callbacks.append(opcode)
            if opcode == 14:
                requests.append(effect)
                # Registration may synchronously reenter the published instance.
                if len(snapshot(effect)) != 256:
                    errors.append("registration reentry failed")
                if cooperative and host_attached:
                    scheduled[effect] = True
                return request_result
            if opcode in (0, 43, 44):
                errors.append("bulk refresh recorded automation/gesture")
            if opcode == 42:
                if rendering:
                    errors.append("realtime display callback")
                cache[:] = snapshot(effect)
            return host_cb(effect, opcode, index, value, ptr, opt)

        callback = HOSTCB(host)
        port = Port(plugin, host_callback=callback)
        address = C.cast(port.eff, C.c_void_p).value
        idle_calls = 0

        def tick():
            nonlocal idle_calls
            # Only service requests retained by this host, never a manual idle.
            for effect in list(scheduled):
                assert effect == address
                idle_calls += 1
                if port.d(53) == 0:
                    del scheduled[effect]

        def render(program, offset):
            nonlocal rendering
            rendering = True
            try:
                return port.render([(512, [(offset, bytes((0xc0, program)))])])
            finally:
                rendering = False

        try:
            assert len(requests) == (1 if enabled else 0), requests
            assert not scheduled  # first effOpen preceded host association
            host_attached = True
            assert port.d(0) == 1  # JUCE initialises again after attaching its host
            assert len(requests) == (2 if enabled else 0)
            tick()
            # Capture is driven solely by UpdateDisplay, never direct getter polls.
            port.d(2, value=269)
            baseline = snapshot(port.eff)
            if enabled:
                assert cache == baseline
            for program, offset in ((7, 0), (13, 127), (7, 127), (13, 0)):
                callbacks.clear()
                previous = list(cache)
                previous_engine = snapshot(port.eff)
                render(program, offset)
                current = snapshot(port.eff)
                assert port.d(3) == 256 + program
                assert 42 not in callbacks and 14 not in callbacks
                assert not any(op in callbacks for op in (0, 43, 44))
                if enabled:
                    assert cache == previous and current != previous_engine
                tick()
                if enabled and cooperative:
                    assert cache == current and scheduled
                    before = callbacks.count(42)
                    tick()
                    assert callbacks.count(42) == before  # no duplicate refresh
                elif enabled:
                    assert cache == previous
                    if program == 7:
                        assert cache != current  # declined scheduling leaves a stale display
            # Idle persists through stopped processing and repeated replacements.
            port.d(12, value=0)
            port.processing = False
            for program in (269, 263, 269):
                port.d(2, value=program)
                tick()
                if enabled:
                    assert cache == snapshot(port.eff)
                if enabled and cooperative:
                    assert scheduled
            assert not errors, errors
            assert idle_calls > 8 if enabled and cooperative else idle_calls == 0
        finally:
            # The host must remove its timer before effClose frees AEffect. VST2
            # has no NeedIdle cancellation opcode or plugin-owned timer to join.
            scheduled.pop(address, None)
            port.close()
        before = (len(callbacks), idle_calls)
        tick()
        assert not scheduled and before == (len(callbacks), idle_calls)
        print(f"PASS: VST2 idle opt-in={enabled}, NeedIdle result={request_result}, cooperative={cooperative}; repeated MIDI0/127+stopped, closed editor, host cache/getters, no RT display/gestures, host teardown")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--automation-only", action="store_true")
    parser.add_argument("--plugin", help="explicit built VST2 artifact")
    parser.add_argument("--automation-isolated", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.automation_only and not args.plugin:
        parser.error("--automation-only requires --plugin")
    if args.plugin:
        PLUGIN = os.path.abspath(args.plugin)
    if args.automation_only:
        if args.automation_isolated:
            directory = os.environ.get("SQ8L_DATA_DIRECTORY")
            assert directory and os.path.isdir(directory)
            sys.exit(automation_only(PLUGIN))
        import subprocess
        import tempfile
        # Parent owns cleanup after child process/DSO destructors have finished.
        # No library is loaded in this process and HOME remains unchanged.
        with tempfile.TemporaryDirectory(prefix="sq8l-automation-") as directory:
            directory = os.path.realpath(directory)
            environment = dict(os.environ, SQ8L_DATA_DIRECTORY=directory)
            result = subprocess.run([sys.executable, __file__, "--automation-only",
                                     "--automation-isolated", "--plugin", PLUGIN],
                                    env=environment, timeout=60).returncode
            assert all(os.path.commonpath((directory, os.path.realpath(os.path.join(directory, f)))) == directory
                       for f in os.listdir(directory))
        sys.exit(result)
    sys.exit(main())
