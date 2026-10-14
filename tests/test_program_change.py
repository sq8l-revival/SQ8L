"""Port addition (issue #40): responding to MIDI program changes.

The original drops 0xC0 in processEvents and has no handler at all, so there is nothing
to compare against: these are contract checks on the port's own behaviour. The original's
record stream is covered by test_midiparser.py, which feeds 0xC0 with the switch at its
default and must stay identical.

Usage: test_program_change.py
"""
import ctypes
import struct
import sys

from harness import LIB_PATH

CHUNK_SIZE = 0x23B
CHUNK_FLAGS = 0x1D          # Synth.cpp kChunkFlags
FLAG_PROGRAM_CHANGE = 4     # Synth.cpp kChunkProgramChange
BANK_SIZE = 128

# MidiRecord: type, channel, data1, deltaFrames, delta, ctrl, value
RECORD = struct.Struct("<HBBiiHh")
REC_PROGRAM_CHANGE = 0x10   # MidiParser.h
LOG_PROGRAM_CHANGE = 0x10   # capi_midi.cpp Recorder
LOG_CONTROL = 8


class Raw(ctypes.Structure):
    _fields_ = [("deltaFrames", ctypes.c_int32), ("data", ctypes.c_uint8 * 3),
                ("noteOffVelocity", ctypes.c_uint8)]


def lib():
    L = ctypes.CDLL(LIB_PATH)
    vp, f32, i32, u8 = ctypes.c_void_p, ctypes.c_float, ctypes.c_int32, ctypes.c_uint8
    sig = {
        "sq8l_midi_new": ([], vp),
        "sq8l_midi_free": ([vp], None),
        "sq8l_midi_set_program_change": ([vp, i32], None),
        "sq8l_midi_process_events": ([vp, ctypes.POINTER(Raw), i32], None),
        "sq8l_midi_records": ([vp, ctypes.c_char_p, i32], i32),
        "sq8l_midi_run_block": ([vp, i32, ctypes.POINTER(i32), i32], i32),
        "sq8l_synth_new": ([f32], vp),
        "sq8l_synth_free": ([vp], None),
        "sq8l_synth_set_program": ([vp, i32], None),
        "sq8l_synth_program": ([vp], i32),
        "sq8l_synth_set_program_change": ([vp, i32], None),
        "sq8l_synth_program_change": ([vp], i32),
        "sq8l_synth_events": ([vp, ctypes.POINTER(Raw), i32], None),
        "sq8l_synth_process": ([vp, ctypes.POINTER(f32), ctypes.POINTER(f32), i32, i32], None),
        "sq8l_synth_get_chunk": ([vp, ctypes.POINTER(u8), i32], i32),
        "sq8l_synth_set_chunk": ([vp, ctypes.POINTER(u8), i32], i32),
    }
    for name, (args, res) in sig.items():
        fn = getattr(L, name)
        fn.argtypes = args
        fn.restype = res
    return L


def events(*msgs):
    """(deltaFrames, status, data1, data2) tuples -> a Raw array."""
    arr = (Raw * len(msgs))()
    for i, (delta, status, d1, d2) in enumerate(msgs):
        arr[i].deltaFrames = delta
        arr[i].data[0], arr[i].data[1], arr[i].data[2] = status, d1, d2
        arr[i].noteOffVelocity = 0
    return arr


def records(L, box):
    buf = ctypes.create_string_buffer(RECORD.size * 64)
    n = L.sq8l_midi_records(box, buf, 64)
    return [RECORD.unpack_from(buf, RECORD.size * i) for i in range(min(n, 64))]


def dispatch_log(L, box, frames=64):
    buf = (ctypes.c_int32 * 600)()
    n = L.sq8l_midi_run_block(box, frames, buf, 600)
    return [tuple(buf[i:i + 6]) for i in range(0, min(n, 600), 6)]


def send(L, s, *msgs, frames=64):
    """Queue the messages and render the block that dispatches them (the parser only
    dispatches from inside Master::process, one record per sample). Returns the block."""
    arr = events(*msgs)
    L.sq8l_synth_events(s, arr, len(msgs))
    left, right = (ctypes.c_float * frames)(), (ctypes.c_float * frames)()
    L.sq8l_synth_process(s, left, right, frames, 1)
    return bytes(memoryview(left).cast("B")) + bytes(memoryview(right).cast("B"))


def get_chunk(L, s):
    buf = (ctypes.c_uint8 * CHUNK_SIZE)()
    n = L.sq8l_synth_get_chunk(s, buf, CHUNK_SIZE)
    return bytes(buf[:n])


def set_chunk(L, s, data):
    buf = (ctypes.c_uint8 * len(data))(*data)
    return L.sq8l_synth_set_chunk(s, buf, len(data))


def main():
    L = lib()
    fails = []

    def check(name, cond, detail=""):
        if cond:
            print(f"  ok   {name}")
        else:
            print(f"  FAIL {name} {detail}")
            fails.append(name)

    # ---------------------------------------------------------------- parser
    print("parser")
    box = L.sq8l_midi_new()
    L.sq8l_midi_process_events(box, events((5, 0xC0, 7, 0)), 1)
    check("off, a program change makes no record at all", records(L, box) == [])
    L.sq8l_midi_free(box)

    box = L.sq8l_midi_new()
    L.sq8l_midi_set_program_change(box, 1)
    L.sq8l_midi_process_events(box, events((5, 0xC3, 7, 0)), 1)
    recs = records(L, box)
    ok = len(recs) == 1 and recs[0][0] == REC_PROGRAM_CHANGE and recs[0][1] == 3 and recs[0][2] == 7
    check("on, one record carrying the channel and the program", ok, detail=str(recs))
    check("dispatched at the message's own sample",
          dispatch_log(L, box) == [(5, LOG_PROGRAM_CHANGE, 3, 7, 0, 0)])
    L.sq8l_midi_free(box)

    # The ordering the VST3 patch has to rebuild by hand: a host's bank+program selector
    # sends both on one timestamp, and the bank has to be in before the program change.
    box = L.sq8l_midi_new()
    L.sq8l_midi_set_program_change(box, 1)
    L.sq8l_midi_process_events(box, events((9, 0xB0, 0, 2), (9, 0xC0, 13, 0)), 2)
    log = dispatch_log(L, box)
    kinds = [entry[1] for entry in log]
    check("a bank select and a program change on one sample keep their order",
          kinds == [LOG_CONTROL, LOG_PROGRAM_CHANGE], detail=str(log))
    L.sq8l_midi_free(box)

    # ---------------------------------------------------------------- engine
    print("engine")
    s = L.sq8l_synth_new(44100.0)
    check("a new instance responds", L.sq8l_synth_program_change(s) == 1)

    send(L, s, (0, 0xC0, 7, 0))
    check("with no bank select, the program loads from the bank the edit buffer is on",
          L.sq8l_synth_program(s) == 7, detail=str(L.sq8l_synth_program(s)))

    send(L, s, (0, 0xB0, 0, 2), (0, 0xC0, 13, 0))
    check("CC0 names the bank", L.sq8l_synth_program(s) == 2 * BANK_SIZE + 13,
          detail=str(L.sq8l_synth_program(s)))

    send(L, s, (0, 0xC0, 5, 0))
    check("the bank stays armed for later program changes",
          L.sq8l_synth_program(s) == 2 * BANK_SIZE + 5, detail=str(L.sq8l_synth_program(s)))

    send(L, s, (0, 0xB0, 0, 7), (0, 0xC0, 3, 0))
    check("a bank the library does not have ignores the program change",
          L.sq8l_synth_program(s) == 2 * BANK_SIZE + 5, detail=str(L.sq8l_synth_program(s)))

    # Omni, like note on: the channel is ignored.
    send(L, s, (0, 0xB9, 0, 1), (0, 0xC9, 4, 0))
    check("program changes and bank select are omni", L.sq8l_synth_program(s) == BANK_SIZE + 4,
          detail=str(L.sq8l_synth_program(s)))

    # What the whole event-ordering question is for: a program change on the same sample as a
    # note on, the program change first, must make the note sound with the new program. The
    # formats that hand over raw MIDI inherit the host's order; the VST3 shell has to rebuild
    # it, which is why the patch collects notes last (see patches/dpf-vst3-program-change.patch).
    NOTE, PROG = 60, 45
    a = L.sq8l_synth_new(44100.0)
    together = send(L, a, (0, 0xC0, PROG, 0), (0, 0x90, NOTE, 100), frames=4096)
    L.sq8l_synth_free(a)

    b = L.sq8l_synth_new(44100.0)
    L.sq8l_synth_set_program(b, PROG)
    already = send(L, b, (0, 0x90, NOTE, 100), frames=4096)
    L.sq8l_synth_free(b)

    c = L.sq8l_synth_new(44100.0)
    old = send(L, c, (0, 0x90, NOTE, 100), frames=4096)
    L.sq8l_synth_free(c)

    check("a note on the program change's own sample sounds with the new program",
          together == already)
    check("and that is audibly not the program it replaced", together != old)

    L.sq8l_synth_set_program(s, 42)
    L.sq8l_synth_set_program_change(s, 0)
    send(L, s, (0, 0xC0, 99, 0))
    check("off, a program change changes nothing", L.sq8l_synth_program(s) == 42,
          detail=str(L.sq8l_synth_program(s)))
    L.sq8l_synth_free(s)

    # ---------------------------------------------------------------- chunk
    print("chunk")
    s = L.sq8l_synth_new(44100.0)
    on = get_chunk(L, s)
    check("a responding instance sets its bit in the chunk",
          len(on) == CHUNK_SIZE and on[CHUNK_FLAGS] & FLAG_PROGRAM_CHANGE != 0,
          detail=hex(on[CHUNK_FLAGS]) if on else "no chunk")

    L.sq8l_synth_set_program_change(s, 0)
    off = get_chunk(L, s)
    check("an instance set not to respond writes no bit",
          off[CHUNK_FLAGS] & FLAG_PROGRAM_CHANGE == 0, detail=hex(off[CHUNK_FLAGS]))

    # What reopening a project saved by the original or by an earlier port does.
    old = bytearray(on)
    old[CHUNK_FLAGS] = 0
    s2 = L.sq8l_synth_new(44100.0)
    set_chunk(L, s2, bytes(old))
    check("a chunk without the bit stops the instance responding",
          L.sq8l_synth_program_change(s2) == 0)
    send(L, s2, (0, 0xC0, 99, 0))
    check("and its program changes are then ignored", L.sq8l_synth_program(s2) != 99,
          detail=str(L.sq8l_synth_program(s2)))
    L.sq8l_synth_free(s2)

    s3 = L.sq8l_synth_new(44100.0)
    L.sq8l_synth_set_program_change(s3, 0)
    set_chunk(L, s3, on)
    check("a chunk with the bit makes it respond again", L.sq8l_synth_program_change(s3) == 1)
    L.sq8l_synth_free(s3)
    L.sq8l_synth_free(s)

    print()
    if fails:
        print(f"FAILED: {len(fails)} check(s): {', '.join(fails)}")
        return 1
    print("all program change checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
