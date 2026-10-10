"""Behaviour of the ported controls vs the ORIGINAL objects running in the emulator.

1. assets: every sprite frame and the background equal the original's decoded bitmaps;
2. EditorView defaults equal the original form after start-up (geometry, sprites, AniIdx,
   hover frames, LCD set-up and charsets, LED set-up, hints, z-order);
3. TGraphKnobB methods called directly on an original knob (setValue/Min/Max/ValueStep,
   MaxPixDist, MaxFinePixDist, beginDrag/dragMove/endDrag/cancelDrag) vs the C++ Knob,
   random sequences, every field compared bit-exactly after every call;
4. TLCD3.writeText (random, incl. clipping) and TAniDisplay setters, field by field;
5. rendering of random knob values / LED values / LCD contents (incl. incremental LCD
   updates on a persistent C++ view: the dirty-cell path);
6. mouse sequences replayed on both (Windows messages into the oracle, the same messages into
   EditorView): knob drags (vertical, horizontal distance, Shift), double clicks, button
   hover/press/leave/release, LCD clicks/drags, menu areas. After every message the control
   state (knob fields, button pressed/hover, mouse capture) and the event stream (MouseDown/Up,
   Click, DblClick, knob OnChange, LCD cell events) must be identical.

Usage: SQ8L_TESTAPI=build/libsq8l_testapi.dylib python tests/test_gui_controls.py
"""
import os
import random
import struct
import sys
import time

import numpy as np
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESP

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_gui_state import (BUTTON_NAMES, GIF_NAMES, KNOB_FIELDS, KNOB_NAMES, LED_NAMES, CppGui,  # noqa: E402
                            OriginalGui, lib, text_masks)

WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDBLCLK = 0x200, 0x201, 0x202, 0x203
WM_RBUTTONDOWN, WM_RBUTTONUP = 0x204, 0x205
MK_LBUTTON, MK_RBUTTON, MK_SHIFT = 1, 2, 4

FAILS = []
COUNTS = {}


def check(cond, what):
    COUNTS[what.split(":")[0]] = COUNTS.get(what.split(":")[0], 0) + 1
    if not cond:
        FAILS.append(what)
        if len(FAILS) < 30:
            print("   MISMATCH", what)
    return cond


def fb(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def bf(b):
    return struct.unpack("<f", struct.pack("<I", b & 0xFFFFFFFF))[0]


# ------------------------------------------------------------------ original knob fields
KNOB_DW = dict(value=0x22C, min=0x230, max=0x234, range=0x238, step=0x23C, pixFactor=0x244, snapZone=0x24C,
               minFineFac=0x1FC, acc=0x204, angle=0x278, angleK=0x27C, maxPixDist=0x218, maxFinePixDist=0x21C,
               radius=0x250, fx=0x254, fy=0x258, downX=0x208, downY=0x20C, lastX=0x210, lastY=0x214)
KNOB_B = dict(snapToZero=0x248, intMode=0x240, dragging=0x200, dragActive=0x201, loaded=0x1F8,
              force=0x1F9, textValid=0x2B4)
assert set(KNOB_DW) | set(KNOB_B) == set(KNOB_FIELDS)


def oknob(og, o):
    d = {k: og.e.u32(o + off) for k, off in KNOB_DW.items()}
    d.update({k: og.u8(o, off) for k, off in KNOB_B.items()})
    return d


def oknob_write(og, o, d):
    for k, off in KNOB_DW.items():
        og.e.w32(o + off, d[k])
    for k, off in KNOB_B.items():
        og.e.write(o + off, bytes([d[k]]))


def ocall(og, addr, *args):
    return og.e.call(addr, *args, conv="register")[0]


def fmt_knob(d):
    return {k: (bf(v) if k in ("value", "min", "max", "range", "step", "pixFactor", "snapZone", "minFineFac", "acc",
                               "angle", "angleK") else v) for k, v in d.items()}


# ------------------------------------------------------------------ event tracing (original)
class Tracer:
    """Hooks TControl.MouseDown/MouseUp/Click/DblClick, TGraphKnobB OnChange and the LCD cell
    event handlers of the original, producing the same log lines as tests/capi_gui.cpp."""

    def __init__(self, og):
        self.og, self.log, self.handles = og, [], []
        e = og.e
        names = dict(og.name_of)
        names[og.form] = "plugEditForm"

        def name(p):
            return names.get(p, "?%x" % p)

        def reg(r):
            return e.uc.reg_read(r)

        def arg(i):
            return e.u32(reg(UC_X86_REG_ESP) + 4 * i)

        def s32(v):
            return v - (1 << 32) if v & 0x80000000 else v

        hooks = {
            0x442960: lambda: "down %s %d %d %d %d" % (name(reg(UC_X86_REG_EAX)), reg(UC_X86_REG_EDX) & 0xFF,
                                                     reg(UC_X86_REG_ECX) & 0xFF, s32(arg(2)), s32(arg(1))),
            0x442C44: lambda: "up %s %d %d %d %d" % (name(reg(UC_X86_REG_EAX)), reg(UC_X86_REG_EDX) & 0xFF,
                                                   reg(UC_X86_REG_ECX) & 0xFF, s32(arg(2)), s32(arg(1))),
            0x4428D4: lambda: "click %s" % name(reg(UC_X86_REG_EAX)),
            0x442940: lambda: "dblclick %s" % name(reg(UC_X86_REG_EAX)),
            0x47B1F8: lambda: "change %s %d" % (name(reg(UC_X86_REG_EAX)), e.u32(reg(UC_X86_REG_EAX) + 0x22C)),
        }
        lcd = og.objs["lcd"]
        cell = {
            0x648: lambda: "celldown lcd %d %d %d %d" % (reg(UC_X86_REG_ECX) & 0xFF, arg(3) & 0xFF, s32(arg(2)),
                                                       s32(arg(1))),
            0x650: lambda: "cellup lcd %d %d %d %d" % (reg(UC_X86_REG_ECX) & 0xFF, arg(3) & 0xFF, s32(arg(2)),
                                                     s32(arg(1))),
            0x658: lambda: "cellmove lcd %d %d %d" % (reg(UC_X86_REG_ECX) & 0xFF, s32(arg(2)), s32(arg(1))),
            0x660: lambda: "dblpos lcd %d %d" % (s32(reg(UC_X86_REG_ECX)), s32(arg(1))),
            0x668: lambda: "celldbl lcd %d %d" % (s32(reg(UC_X86_REG_ECX)), s32(arg(1))),
        }
        for off, f in cell.items():
            code = e.u32(lcd + off)
            if code:
                hooks[code] = f
        for addr, f in hooks.items():
            self.handles.append(e.trace(addr, (lambda f: lambda emu: self.log.append(f()))(f),
                                        lambda emu, ctx: None))
            e.uc.ctl_remove_cache(addr, addr + 1)   # code hooks only apply to blocks translated later

    def take(self):
        out, self.log = self.log, []
        return out

    def close(self):
        for h in self.handles:
            self.og.e.untrace(h)


# ================================================================== 1. assets
def test_assets(og):
    print("[1] assets: sprite frames and background vs the original's decoded bitmaps")
    import ctypes
    L = lib()
    nframes = 0
    for name in GIF_NAMES:
        frames = og.gif_frames(name)
        info = (ctypes.c_int32 * 3)()
        for i, fr in enumerate(frames):
            h, w = fr.shape[:2]
            buf = (ctypes.c_uint8 * (w * h * 3))()
            ok = L.sq8l_gui_sprite_frame(name.encode(), i, buf, info)
            px = np.frombuffer(bytes(buf), np.uint8).reshape(h, w, 3)
            check(ok and list(info) == [w, h, len(frames)] and (px == fr).all(), f"asset: {name}[{i}]")
            nframes += 1
    bg = og.background_pixels()
    buf = (ctypes.c_uint8 * (626 * 430 * 3))()
    L.sq8l_gui_background(buf)
    check((np.frombuffer(bytes(buf), np.uint8).reshape(430, 626, 3) == bg).all(), "asset: background")
    print(f"    {nframes} frames + background compared")


# ================================================================== 2. defaults
def test_defaults(og):
    print("[2] EditorView defaults vs the original form after start-up (and its first paint)")
    og.frame()
    st = og.state()
    cg = CppGui()
    check(cg.zorder() == st["order"], "default: z-order")
    for n, s in st["buttons"].items():
        b = cg.button(n)
        check(cg.bounds(n) == s["bounds"], f"default: {n} bounds {cg.bounds(n)} {s['bounds']}")
        for k in ("aniIdx", "twoFrames", "frameWidth", "frameColor", "pressDisp", "pressed", "hover"):
            check(b[k] == s[k], f"default: {n}.{k} {b[k]} {s[k]}")
        check(cg.hint(n) == s["hint"], f"default: {n} hint")
    for n, s in st["knobs"].items():
        k = cg.knob(n)
        check(cg.bounds(n) == s["bounds"], f"default: {n} bounds {cg.bounds(n)} {s['bounds']}")
        for f, of in (("radius", "radius"), ("minFineFac", "minFineFac"), ("maxFinePixDist", "maxFinePixDist"),
                      ("snapToZero", "snapToZero")):
            check(k[f] == s[of], f"default: {n}.{f} {k[f]} {s[of]}")
        check([k["fx"], k["fy"]] == s["framePos"], f"default: {n} frame pos {[k['fx'], k['fy']]} {s['framePos']}")
    for n, s in st["leds"].items():
        a = cg.ani(n)
        check(cg.bounds(n) == s["bounds"], f"default: {n} bounds")
        check([a["startFrame"], a["numFrames"], a["min"], a["max"], a["value"]] ==
              [s["startFrame"], s["numFrames"], s["min"], s["max"], s["value"]], f"default: {n} config")
    for n, s in st["lcd"].items():
        i = cg.lcd_info(n)
        check(cg.bounds(n) == s["bounds"], f"default: {n} bounds {cg.bounds(n)} {s['bounds']}")
        for f in ("charW", "charH", "gapX", "gapY", "frameW", "frameH", "widthPix", "heightPix", "cols", "rows",
                  "nattr"):
            check(i[f] == s[f], f"default: {n}.{f} {i[f]} {s[f]}")
        check(i["colors"] == s["colors"], f"default: {n} colors {i['colors']} {s['colors']}")
        check(cg.lcd_charmap(n) == s["charmap"], f"default: {n} charmap")
    for n in ("StatusPanel2", "Panel1", "progNameEdit"):
        b = st["panels"][n]["bounds"] if n in st["panels"] else st["edit"]["bounds"]
        check(cg.bounds(n) == b, f"default: {n} bounds")
    for n, s in st["images"].items():
        check(cg.bounds(n) == s["bounds"] and cg.hint(n) == s["hint"], f"default: {n}")
    check(cg.hint("numLcd") == og.hint("numLcd"), "default: numLcd hint")
    check(cg.hint("StatusLabel2") == og.hint("StatusLabel2"), "default: StatusLabel2 hint")


# ================================================================== 3. knob methods
FLOAT_POOL = [0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 1.5, 2.5, -2.5, 63.0, 64.0, 127.0, -127.0, 128.0, 145.0, 200.0, 1e6,
              -1e6, 1e-7, 0.49999997, 0.50000006, 3.4999998, 126.5, 127.49999, 0.1, 0.2, 0.25, 2.0, 3.0, 99.99]


def rfloat(rng):
    r = rng.random()
    if r < 0.35:
        return rng.choice(FLOAT_POOL)
    if r < 0.6:
        return float(rng.randint(-150, 150))
    if r < 0.75:
        return rng.randint(-300, 300) / 2.0
    return bf(fb(rng.uniform(-160, 160)))


def test_knob_methods(og, ops=4000, seed=1):
    print("[3] TGraphKnobB methods, random sequences, original vs C++ (all fields after every call)")
    rng = random.Random(seed)
    o = og.objs["lcdKnob2"]
    for off in (0x2E0, 0x2E8, 0x2F0):          # detach OnChange / mouse-position events
        og.e.write(o + off, bytes(8))
    cg = CppGui()
    cg.set_knob("lcdKnob2", oknob(og, o))
    p = cg.c("lcdKnob2")
    L = cg.L
    names = ["setValue", "setMin", "setMax", "setStep", "setMaxPixDist", "setMaxFine", "beginDrag", "dragMove",
             "endDrag", "cancelDrag"]
    counts = dict.fromkeys(names, 0)
    x, y = 20, 20
    for n in range(ops):
        r = rng.random()
        if r < 0.20:
            op = "setValue"
        elif r < 0.27:
            op = "setMin"
        elif r < 0.34:
            op = "setMax"
        elif r < 0.38:
            op = "setStep"
        elif r < 0.42:
            op = "setMaxPixDist"
        elif r < 0.45:
            op = "setMaxFine"
        elif r < 0.52:
            op = "beginDrag"
        elif r < 0.94:
            op = "dragMove"
        elif r < 0.97:
            op = "endDrag"
        else:
            op = "cancelDrag"
        shift = rng.choice((0, 0, 0, 1, 8, 9))
        if op == "setValue":
            v = rfloat(rng)
            ocall(og, 0x47AA8C, o, 0, 0, fb(v))
            L.sq8l_gui_knob_call(p, 0, fb(v), 0, 0)
        elif op in ("setMin", "setMax"):
            v = rfloat(rng)
            ocall(og, 0x47ABD0 if op == "setMin" else 0x47ACD0, o, 0, 0, fb(v))
            L.sq8l_gui_knob_call(p, 1 if op == "setMin" else 2, fb(v), 0, 0)
        elif op == "setStep":
            v = rng.choice((0.0, 1.0, 1.0, 1.0, 0.5, 0.25, 2.0, 3.0, 0.1, -1.0, 2.5))
            ocall(og, 0x47ADD0, o, 0, 0, fb(v))
            L.sq8l_gui_knob_call(p, 3, fb(v), 0, 0)
        elif op == "setMaxPixDist":
            v = rng.choice((0, 1, 40, 100, 180, 200, 256, -77, 20000, rng.randint(-500, 500)))
            ocall(og, 0x47AA44, o, v & 0xFFFFFFFF)
            L.sq8l_gui_knob_call(p, 4, v & 0xFFFFFFFF, 0, 0)
        elif op == "setMaxFine":
            v = rng.choice((0, 1, 50, 100, 200, -5, rng.randint(-50, 300)))
            ocall(og, 0x47AA30, o, v & 0xFFFFFFFF)
            L.sq8l_gui_knob_call(p, 5, v & 0xFFFFFFFF, 0, 0)
        elif op == "beginDrag":
            x, y = rng.randint(0, 39), rng.randint(0, 39)
            ocall(og, 0x47A1D0, o, shift, x, y)
            L.sq8l_gui_knob_call(p, 6, shift, x, y)
        elif op == "dragMove":
            x += rng.choice((0, 0, 1, -1, 3, -3, 10, -10, 40, -40, 150, -150))
            y += rng.choice((0, 1, -1, 1, -1, 2, -2, 5, -5, 12, -12, 30, -30, 90, -90))
            ocall(og, 0x47A27C, o, shift, x & 0xFFFFFFFF, y & 0xFFFFFFFF)
            L.sq8l_gui_knob_call(p, 7, shift, x, y)
        elif op == "endDrag":
            ocall(og, 0x47A248, o)
            L.sq8l_gui_knob_call(p, 8, 0, 0, 0)
        else:
            ocall(og, 0x47A268, o)
            L.sq8l_gui_knob_call(p, 9, 0, 0, 0)
        counts[op] += 1
        a, b = oknob(og, o), cg.knob("lcdKnob2")
        if not check(a == b, f"knob-method: #{n} {op}"):
            print("      orig", fmt_knob(a))
            print("      c++ ", fmt_knob(b))
            cg.set_knob("lcdKnob2", a)   # resync and continue
        if not check(L.sq8l_gui_knob_frame(p) == knob_frame_original(a), f"knob-frame: #{n}"):
            pass
    print("    calls:", counts)


def knob_frame_original(d):
    """Frame index Paint would draw (independent Python model of 0x47a554, x87 53-bit)."""
    vmin, vmax, v = bf(d["min"]), bf(d["max"]), bf(d["value"])
    r = np.float32(np.float64(vmax) - np.float64(vmin))
    if r == 0:
        idx = 0
    else:
        t = np.float32((np.float64(v) - np.float64(vmin)) / np.float64(r) * 128.0)
        idx = int(np.rint(np.float64(t)))
    return min(max(idx, 0), 127)


# ================================================================== 4. LCD + AniDisplay methods
def delphi_string(og, s):
    b = s.encode("latin1")
    p = og.e.heap_alloc(len(b) + 16)
    og.e.write(p, struct.pack("<ii", -1, len(b)) + b + b"\0")
    return p + 8


def test_lcd_write(og, calls=1500, seed=2):
    print("[4a] TLCD3.writeText (0x457fb4) random calls, cells + flags compared")
    rng = random.Random(seed)
    o = og.objs["lcd"]
    cg = CppGui()
    og.frame()                       # paint: clears the original's dirty flags
    st = og.lcd_state("lcd")
    for r, row in enumerate(st["cells"]):
        for c, (ch, attr, dirty) in enumerate(row):
            cg.L.sq8l_gui_lcd_set_cell(cg.c("lcd"), c, r, ch, attr, dirty)
    cg.render()                      # same for the C++ view
    alphabet = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789=*<>-.:abcxyz!~\x7f\x01"
    for n in range(calls):
        col = rng.choice((0, 1, 5, 20, 40, 43, 44, 50, -1, -3, -10, rng.randint(-50, 60)))
        row = rng.choice((0, 1, 0, 1, -1, 2, 3))
        text = "".join(rng.choice(alphabet) for _ in range(rng.choice((0, 1, 3, 8, 15, 44, 50))))
        attr = rng.choice((0, 0, 1, 2, 3, 4, 7))
        ocall(og, 0x457FB4, o, col & 0xFFFFFFFF, row & 0xFFFFFFFF, delphi_string(og, text), attr)
        cg.lcd_write("lcd", col, row, text, attr)
        if rng.random() < 0.1:   # paint both (clears dirty flags)
            og.frame()
            cg.render()
        s = og.lcd_state("lcd")
        mine = cg.lcd_cells("lcd")
        check(mine == [[tuple(c) for c in r] for r in s["cells"]], f"lcd-write: #{n} ({col},{row},{text!r},{attr})")
        check(cg.lcd_info("lcd")["flags"] == (s["redraw"] | s["dirty"] << 1), f"lcd-flags: #{n}")


def test_ani_methods(og, calls=600, seed=3):
    print("[4b] TAniDisplay setValue/setMinVal/setMaxVal random calls")
    rng = random.Random(seed)
    o = og.objs["ledAm"]
    cg = CppGui()
    s = og.led_state("ledAm")
    cg.L.sq8l_gui_ani_set(cg.c("ledAm"), b"LedGIF", s["startFrame"], s["numFrames"], s["min"], s["max"], s["value"])
    for n in range(calls):
        fn = rng.choice((0, 0, 0, 1, 2))
        v = rng.choice((0.0, 1.0, 0.5, -0.5, 2.0, 0.49999997, 0.25, 0.75, rng.uniform(-2, 3)))
        ocall(og, (0x47B81C, 0x47B7B4, 0x47B7E8)[fn], o, 0, 0, fb(v))
        cg.L.sq8l_gui_ani_call(cg.c("ledAm"), fn, fb(v))
        a = og.led_state("ledAm")
        b = cg.ani("ledAm")
        check([a["min"], a["max"], a["value"]] == [b["min"], b["max"], b["value"]], f"ani-method: #{n} fn{fn} {v}")
    ocall(og, 0x47B7B4, o, 0, 0, fb(0.0))
    ocall(og, 0x47B7E8, o, 0, 0, fb(1.0))
    ocall(og, 0x47B81C, o, 0, 0, fb(0.0))


# ================================================================== 5. rendering of random values
def compare_frame(og, cg, what):
    img = og.frame()
    st = og.state()
    if cg is None:
        cg = CppGui()
        cg.load_state(st)
    out = cg.render()
    mask = np.zeros(img.shape[:2], bool)
    for (_, x0, y0, x1, y1) in text_masks(st):
        mask[y0:y1, x0:x1] = True
    d = (img != out).any(-1) & ~mask
    check(not d.any(), f"render: {what} ({int(d.sum())} px)")


def test_random_render(og, rounds=25, seed=4):
    print("[5a] random knob values (10 knobs per frame) and LED values, full frame compared")
    rng = random.Random(seed)
    for n in range(rounds):
        for name in KNOB_NAMES:
            o = og.objs[name]
            vmin, vmax = bf(og.e.u32(o + 0x230)), bf(og.e.u32(o + 0x234))
            if vmax <= vmin or rng.random() < 0.3:
                vmin, vmax = float(rng.randint(-127, 0)), float(rng.randint(1, 160))
                og.e.w32(o + 0x230, fb(vmin))
                og.e.w32(o + 0x234, fb(vmax))
            v = rng.choice((vmin, vmax, (vmin + vmax) / 2, rng.uniform(vmin, vmax),
                            vmin + (vmax - vmin) * (rng.randint(0, 255) + 0.5) / 256))
            og.e.w32(o + 0x22C, fb(v))
        for name in LED_NAMES:
            o = og.objs[name]
            og.e.w32(o + 0x20C, fb(rng.choice((0.0, 1.0, 0.25, 0.5, 0.75, 0.49999997, 0.50000006))))
        compare_frame(og, None, f"random values #{n}")
    for name in LED_NAMES:
        og.e.w32(og.objs[name] + 0x20C, fb(0.0))


def test_lcd_incremental(og, rounds=30, seed=5):
    print("[5b] LCD incremental repaint: same writes on the original and on a persistent C++ view")
    rng = random.Random(seed)
    o = og.objs["lcd"]
    og.frame()
    cg = CppGui()
    cg.load_state(og.state())
    cg.render()
    alphabet = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789=*<>-.:+/"
    for n in range(rounds):
        for _ in range(rng.randint(1, 6)):
            col, row = rng.randint(-5, 43), rng.randint(0, 1)
            text = "".join(rng.choice(alphabet) for _ in range(rng.randint(1, 12)))
            attr = rng.choice((0, 1, 2, 3, 5))
            ocall(og, 0x457FB4, o, col & 0xFFFFFFFF, row, delphi_string(og, text), attr)
            cg.lcd_write("lcd", col, row, text, attr)
        img = og.frame()
        out = cg.render()
        lx, ly, lw, lh = og.lcd_state("lcd")["bounds"]
        check((img[ly:ly + lh, lx:lx + lw] == out[ly:ly + lh, lx:lx + lw]).all(), f"lcd-incremental: #{n}")


# ================================================================== 6. mouse sequences
class Both:
    """Send the same Win32 mouse messages to the original (oracle driver) and to EditorView."""

    def __init__(self, og):
        self.og = og
        og.frame()
        self.cg = CppGui()
        self.cg.load_state(og.state())
        self.cg.set_capture(og.capture_name())
        self.tr = Tracer(og)
        self.cg.events()

    def send(self, msg, x, y, keys=0, compare=(), ignore=()):
        self.og.ed.mouse(msg, x, y, keys)
        self.cg.mouse(msg, x, y, keys)
        ev_o = self.tr.take()
        # The original's form puts the cursor back after a knob turn (mouseJump); the port
        # hides it for the duration instead, so the oracle's SetCursorPos has no counterpart.
        ev_c = [ev for ev in self.cg.events() if not ev.startswith("setcursor")]
        ev_o = [ev for ev in ev_o if not ev.startswith(ignore)] if ignore else ev_o
        check(ev_o == ev_c, f"events: {msg:#x} ({x},{y}) keys={keys}\n        orig {ev_o}\n        c++  {ev_c}")
        cap_o, cap_c = self.og.capture_name(), self.cg.capture()
        check(cap_o == cap_c, f"capture: {msg:#x} ({x},{y}) orig={cap_o!r} c++={cap_c!r}")
        for name in compare:
            if name.startswith("lcdKnob"):
                a, b = oknob(self.og, self.og.objs[name]), self.cg.knob(name)
                keys_ = ("value", "acc", "dragging", "dragActive", "downX", "downY", "lastX", "lastY")
                check(all(a[k] == b[k] for k in keys_),
                      f"knob-mouse: {name} {msg:#x} ({x},{y}) orig={ {k: fmt_knob(a)[k] for k in keys_} } "
                      f"c++={ {k: fmt_knob(b)[k] for k in keys_} }")
            else:
                a, b = self.og.button_state(name), self.cg.button(name)
                check((a["pressed"], a["hover"]) == (b["pressed"], b["hover"]),
                      f"button-mouse: {name} {msg:#x} ({x},{y}) orig={(a['pressed'], a['hover'])} "
                      f"c++={(b['pressed'], b['hover'])}")

    def close(self):
        self.tr.close()


def knob_drag_sequences(og, rng):
    print("[6a] knob drags through mouse messages (form handlers live: OnChange, mouseJump)")
    for page in ("buttFilt", "buttLfo1", "buttEnv2", "buttWav"):
        og.ed.click(page)
        og.ed.idle(100)
        for name in ("lcdKnob0", "lcdKnob3", "lcdKnob6", "lcdKnob9"):
            b = Both(og)
            cx, cy = og.ed.center(name)
            keys = rng.choice((0, 0, MK_SHIFT))
            b.send(WM_MOUSEMOVE, cx, cy, keys, compare=(name,))
            b.send(WM_LBUTTONDOWN, cx, cy, MK_LBUTTON | keys, compare=(name,))
            x, y = cx, cy
            for _ in range(rng.randint(5, 25)):
                x += rng.choice((0, 0, 2, -2, 15, -15, 60, -60))
                y += rng.choice((-1, 1, -3, 3, -8, 8, -20, 20, -45))
                if rng.random() < 0.15:
                    keys ^= MK_SHIFT
                b.send(WM_MOUSEMOVE, x, y, MK_LBUTTON | keys, compare=(name,))
            b.send(WM_LBUTTONUP, x, y, keys, compare=(name,))
            b.send(WM_MOUSEMOVE, x + 1, y + 1, 0, compare=(name,))
            # double click on the knob: DblClick cancels the drag, MouseDown(ssDouble) starts one
            b.send(WM_MOUSEMOVE, cx, cy, 0, compare=(name,))
            b.send(WM_LBUTTONDOWN, cx, cy, MK_LBUTTON, compare=(name,))
            b.send(WM_LBUTTONUP, cx, cy, 0, compare=(name,))
            b.send(WM_LBUTTONDBLCLK, cx, cy, MK_LBUTTON, compare=(name,))
            b.send(WM_MOUSEMOVE, cx, cy - 7, MK_LBUTTON, compare=(name,))
            b.send(WM_LBUTTONUP, cx, cy - 7, 0, compare=(name,))
            # right button on a knob
            b.send(WM_RBUTTONDOWN, cx, cy, MK_RBUTTON, compare=(name,))
            b.send(WM_RBUTTONUP, cx, cy, 0, compare=(name,))
            b.close()


def button_sequences(og, rng):
    print("[6b] buttons: hover in/out, press/release, press-leave-release, cross-button moves")
    safe = ("buttFilt", "buttLfo3", "buttEnv1", "buttMat2", "buttDca4", "pscrDownButton", "pscrUpButton",
            "buttMono", "buttSync", "buttAm", "buttWav", "buttOsc2")
    for _ in range(14):
        a = rng.choice(safe)
        b2 = rng.choice(BUTTON_NAMES)
        both = Both(og)
        ax, ay = og.ed.center(a)
        bx, by = og.ed.center(b2)
        cmp = (a, b2)
        both.send(WM_MOUSEMOVE, ax, ay, 0, compare=cmp)                  # hover a
        both.send(WM_MOUSEMOVE, ax + 3, ay + 2, 0, compare=cmp)
        both.send(WM_MOUSEMOVE, bx, by, 0, compare=cmp)                  # straight to b (a still captures)
        both.send(WM_MOUSEMOVE, bx + 1, by, 0, compare=cmp)              # now b
        both.send(WM_LBUTTONDOWN, bx + 1, by, MK_LBUTTON, compare=cmp)   # press b
        both.send(WM_MOUSEMOVE, bx + 2, by + 1, MK_LBUTTON, compare=cmp)
        both.send(WM_MOUSEMOVE, 5, 300, MK_LBUTTON, compare=cmp)         # leave while pressed
        both.send(WM_MOUSEMOVE, bx, by, MK_LBUTTON, compare=cmp)         # come back
        both.send(WM_LBUTTONUP, bx, by, 0, compare=cmp)                  # no click
        both.send(WM_MOUSEMOVE, ax, ay, 0, compare=cmp)
        both.send(WM_MOUSEMOVE, ax, ay, 0, compare=cmp)
        both.send(WM_LBUTTONDOWN, ax, ay, MK_LBUTTON, compare=cmp)       # full click on a
        both.send(WM_LBUTTONUP, ax, ay, 0, compare=cmp)
        both.send(WM_MOUSEMOVE, 300, 30, 0, compare=cmp)                 # leave after the click
        both.close()
        og.ed.idle(60)


def lcd_sequences(og, rng):
    print("[6c] LCD: clicks, drags, double clicks (cell events; knob drags started from the LCD are "
          "form logic and ignored)")
    s = og.lcd_state("lcd")
    x0, y0, w, h = s["bounds"]
    og.ed.click("buttEnv1")
    og.ed.idle(100)
    for _ in range(10):
        b = Both(og)
        x, y = x0 + rng.randint(0, w - 1), y0 + rng.randint(0, h - 1)
        b.send(WM_MOUSEMOVE, x, y)
        b.send(WM_LBUTTONDOWN, x, y, MK_LBUTTON)
        for _ in range(rng.randint(0, 6)):
            x += rng.randint(-30, 30)
            y += rng.randint(-15, 15)
            b.send(WM_MOUSEMOVE, x, y, MK_LBUTTON, ignore=("change",))
        b.send(WM_LBUTTONUP, x, y, ignore=("change",))
        x, y = x0 + rng.randint(0, w - 1), y0 + rng.randint(0, h - 1)
        b.send(WM_MOUSEMOVE, x, y)
        b.send(WM_LBUTTONDOWN, x, y, MK_LBUTTON)
        b.send(WM_LBUTTONUP, x, y)
        b.send(WM_LBUTTONDBLCLK, x, y, MK_LBUTTON)
        b.send(WM_LBUTTONUP, x, y)
        b.close()
        og.ed.idle(60)


def menu_sequences(og):
    print("[6d] menu areas (graphic controls on the form) and the form itself")
    b = Both(og)
    for name in ("menuInfoImage", "menuOptImage"):
        x, y = og.ed.center(name)
        b.send(WM_MOUSEMOVE, x, y)
        b.send(WM_MOUSEMOVE, x + 2, y)
        b.send(WM_LBUTTONDOWN, x, y, MK_LBUTTON)
        b.send(WM_MOUSEMOVE, x + 4, y + 1, MK_LBUTTON)
        b.send(WM_MOUSEMOVE, 400, 200, MK_LBUTTON)      # dragged off: no click
        b.send(WM_LBUTTONUP, 400, 200)
    b.send(WM_MOUSEMOVE, 330, 100)                       # form background
    b.send(WM_LBUTTONDOWN, 330, 100, MK_LBUTTON)
    b.send(WM_LBUTTONUP, 330, 100)
    b.close()


def main():
    t0 = time.time()
    og = OriginalGui()
    og.ed.idle(1500)
    test_assets(og)
    test_defaults(og)
    test_knob_methods(og)
    test_lcd_write(og)
    test_ani_methods(og)
    og2 = OriginalGui()
    og2.ed.idle(1500)
    test_random_render(og2)
    og2.ed.click("buttLfo2")
    og2.ed.idle(200)
    test_lcd_incremental(og2)
    og3 = OriginalGui()
    og3.ed.idle(1500)
    rng = random.Random(6)
    knob_drag_sequences(og3, rng)
    button_sequences(og3, rng)
    lcd_sequences(og3, rng)
    menu_sequences(og3)
    print()
    print("checks:", {k: v for k, v in sorted(COUNTS.items())})
    print(f"time {time.time() - t0:.0f}s")
    if FAILS:
        print(f"FAIL: {len(FAILS)} mismatches")
        sys.exit(1)
    print("PASS")


if __name__ == "__main__":
    main()
