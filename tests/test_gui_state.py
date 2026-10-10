"""Read the internal state of the ORIGINAL editor's controls from emulator memory.

The original editor runs headless in the emulator (oracle/gui_driver.Editor). This module
locates the Delphi objects behind the form (TplugEditForm, via the VCL object-instance
thunk installed as the window procedure) and its published fields (offsets from
re/extracted/classes.json), and decodes each control's fields (offsets from the
disassembly of units LCD3, graphKnobB, GraphButton, AniDisplay, AniGIF; see
docs/modules/gui_controls.md).

Used by tests/test_gui_render.py (feeds the state to the C++ EditorView) and by
re/scripts/extract_gui_assets.py (decoded sprite frames / background).

Run directly to dump the start-up state:  python tests/test_gui_state.py
"""
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "oracle"))

from gui_driver import Editor  # noqa: E402

CLASSES = json.load(open(os.path.join(ROOT, "re", "extracted", "classes.json")))
_FORM_CLS = [c for c in CLASSES if c["name"] == "TplugEditForm"][0]
FORM_FIELDS = {f["name"]: f["offset"] for f in _FORM_CLS["published_fields"]}
VMT_NAMES = {c["vmt"]: c["name"] for c in CLASSES}

GIF_NAMES = ("KnobGif", "charGIF", "numCharGIF", "LedGIF", "ButtGIF", "smallButtGIF", "upDownGif", "scrButtGif")
KNOB_NAMES = tuple(f"lcdKnob{i}" for i in range(10))
BUTTON_NAMES = ("upButton", "downButton", "writeButton", "initButton", "sysExSendButton", "sysExReqButton",
                "buttOsc1", "buttOsc2", "buttOsc3", "buttDca1", "buttDca2", "buttDca3", "buttFilt", "buttDca4",
                "buttModes", "buttSync", "buttAm", "buttLfo1", "buttLfo2", "buttLfo3", "buttLfo4", "buttEnv1",
                "buttEnv2", "buttEnv3", "buttEnv4", "buttMat1", "buttMat2", "buttMat3", "buttWav", "buttMono",
                "BankButton", "pscrUpButton", "pscrDownButton")
LED_NAMES = ("ledSync", "ledAm", "ledMono")
LCD_NAMES = ("lcd", "numLcd")
IMAGE_NAMES = ("menuFileImage", "menuOptImage", "menuInfoImage", "menuPanicImage")

# TControl
C_LEFT, C_TOP, C_WIDTH, C_HEIGHT, C_TEXT, C_HINT = 0x30, 0x34, 0x38, 0x3C, 0x54, 0x70


def f32(b):
    return struct.unpack("<f", b)[0]


class OriginalGui:
    """Wraps oracle Editor with object-level access to the original controls."""

    def __init__(self, program=None, editor=None):
        self.ed = editor or Editor(program=program)
        self.e = self.ed.emu
        self.g = self.ed.g
        self.form = self.e.u32(self.g.windows[self.ed.form].wndproc + 9)
        self.objs = {name: self.e.u32(self.form + off) for name, off in FORM_FIELDS.items()}
        self.gif_by_ptr = {self.objs[n]: n for n in GIF_NAMES}
        # windowed controls: object -> hwnd (VCL thunk: E8 rel32, Method.Code, Method.Data)
        self.hwnd_of = {}
        for h, w in self.g.windows.items():
            if w.parent == self.ed.form or h == self.ed.form:
                try:
                    self.hwnd_of[self.e.u32(w.wndproc + 9)] = h
                except Exception:
                    pass
        self.name_of = {v: k for k, v in self.objs.items()}

    # ------------------------------------------------------------- raw access
    def s32(self, o, off):
        return self.e.s32(o + off)

    def u32(self, o, off):
        return self.e.u32(o + off)

    def fl(self, o, off):
        return f32(self.e.read(o + off, 4))

    def flbits(self, o, off):
        return self.e.u32(o + off)

    def u8(self, o, off):
        return self.e.read(o + off, 1)[0]

    def dstr(self, p):
        if not p:
            return ""
        n = self.e.s32(p - 4)
        return self.e.read(p, n).decode("latin1")

    def bounds(self, o):
        return [self.s32(o, x) for x in (C_LEFT, C_TOP, C_WIDTH, C_HEIGHT)]

    def set_capture(self, name):
        self.L.sq8l_gui_set_capture(self.v, name.encode())

    def hint(self, name):
        return self.dstr(self.u32(self.objs[name], C_HINT))

    def visible(self, name):
        h = self.hwnd_of.get(self.objs[name])
        return bool(h and self.g.windows[h].visible)

    def window(self, name):
        return self.g.windows[self.hwnd_of[self.objs[name]]]

    # ------------------------------------------------------------- bitmaps
    def bitmap_pixels(self, tbitmap):
        """RGB pixels (numpy HxWx3) of a TBitmap as decoded by the original (TBitmap.FImage.FHandle)."""
        img = self.e.u32(tbitmap + 0x20)
        hbm = self.e.u32(img + 8)
        return self.g.objs[hbm]["surface"].load()

    def gif_frames(self, name):
        o = self.objs[name]
        arr, n = self.u32(o, 0x38), self.s32(o, 0x3C)
        return [self.bitmap_pixels(self.e.u32(arr + 4 * i)) for i in range(n)]

    def gif_info(self, name):
        o = self.objs[name]
        return dict(loaded=self.u8(o, 0x24), w=self.s32(o, 0x30), h=self.s32(o, 0x34), count=self.s32(o, 0x3C))

    def background_pixels(self):
        """The form background: Form.Brush.Bitmap (FormShow draws ImgBack's JPEG into it)."""
        brush = self.u32(self.form, 0x130)
        res = self.u32(brush, 0x10)        # TBrush.FResource -> TResource (Data at +0x10)
        bmp = self.u32(res, 0x18)          # TBrushData: Color, Bitmap, Style
        return self.bitmap_pixels(bmp)

    # ------------------------------------------------------------- controls
    def gif_name(self, p):
        return self.gif_by_ptr.get(p, "" if p == 0 else "?%x" % p)

    def lcd_state(self, name):
        o = self.objs[name]
        s = dict(bounds=self.bounds(o), visible=self.visible(name),
                 charW=self.s32(o, 0x1F8), charH=self.s32(o, 0x1FC), gapX=self.s32(o, 0x200), gapY=self.s32(o, 0x204),
                 frameW=self.s32(o, 0x208), frameH=self.s32(o, 0x20C), widthPix=self.s32(o, 0x210),
                 heightPix=self.s32(o, 0x214), cols=self.s32(o, 0x218), rows=self.s32(o, 0x21C),
                 gif=self.gif_name(self.u32(o, 0x220)), nattr=self.s32(o, 0x224),
                 charmap=list(struct.unpack("<256i", self.e.read(o + 0x228, 1024))),
                 colors=[self.u32(o, x) for x in (0x634, 0x638, 0x63C, 0x640, 0x644)],
                 redraw=self.u8(o, 0x630), dirty=self.u8(o, 0x631), lastMouse=[self.s32(o, 0x670), self.s32(o, 0x674)])
        rows = self.u32(o, 0x628)
        cells = []
        if rows:
            for r in range(self.e.s32(rows - 4)):
                rp = self.e.u32(rows + 4 * r)
                n = self.e.s32(rp - 4) if rp else 0
                raw = self.e.read(rp, 6 * n) if n else b""
                cells.append([(raw[6 * i + 1], struct.unpack("<i", raw[6 * i + 2:6 * i + 6])[0], raw[6 * i])
                              for i in range(n)])
        s["cells"] = cells   # (char, attr, dirty)
        return s

    def knob_state(self, name):
        o = self.objs[name]
        fb = lambda off: self.flbits(o, off)  # noqa: E731
        return dict(bounds=self.bounds(o), visible=self.visible(name), gif=self.gif_name(self.u32(o, 0x2DC)),
                    value=fb(0x22C), min=fb(0x230), max=fb(0x234), range=fb(0x238), step=fb(0x23C),
                    intMode=self.u8(o, 0x240), pixFactor=fb(0x244), snapToZero=self.u8(o, 0x248), snapZone=fb(0x24C),
                    maxPixDist=self.s32(o, 0x218), maxFinePixDist=self.s32(o, 0x21C), minFineFac=fb(0x1FC),
                    radius=self.s32(o, 0x250), framePos=[self.s32(o, 0x254), self.s32(o, 0x258)],
                    center=[self.s32(o, 0x264), self.s32(o, 0x268)],
                    angleMin=fb(0x26C), angleMax=fb(0x270), angleRange=fb(0x274), angle=fb(0x278), angleK=fb(0x27C),
                    loaded=self.u8(o, 0x1F8), force=self.u8(o, 0x1F9), dragging=self.u8(o, 0x200),
                    captured=self.u8(o, 0x201), acc=fb(0x204), downPos=[self.s32(o, 0x208), self.s32(o, 0x20C)],
                    lastPos=[self.s32(o, 0x210), self.s32(o, 0x214)], savedMouse=[self.s32(o, 0x220), self.s32(o, 0x224)],
                    restoreMouse=self.u8(o, 0x228), labelClick=self.u8(o, 0x280), showCaption=self.u8(o, 0x2B5),
                    showValue=self.u8(o, 0x2B6), textValid=self.u8(o, 0x2B4), hint=self.hint(name))

    def button_state(self, name):
        o = self.objs[name]
        return dict(bounds=self.bounds(o), visible=self.visible(name), gif=self.gif_name(self.u32(o, 0x200)),
                    aniIdx=self.s32(o, 0x204), imagePos=[self.s32(o, 0x208), self.s32(o, 0x20C)],
                    twoFrames=self.u8(o, 0x210), caption=self.dstr(self.u32(o, 0x214)), captionPlace=self.u8(o, 0x218),
                    fontColor=self.u32(o, 0x230), pressDisp=self.s32(o, 0x234), pressed=self.s32(o, 0x238),
                    hover=self.u8(o, 0x23C), frameWidth=self.s32(o, 0x240), frameColor=self.u32(o, 0x244),
                    hint=self.hint(name))

    def led_state(self, name):
        o = self.objs[name]
        return dict(bounds=self.bounds(o), visible=self.visible(name), gif=self.gif_name(self.u32(o, 0x1F8)),
                    startFrame=self.s32(o, 0x1FC), numFrames=self.s32(o, 0x200), min=self.flbits(o, 0x204),
                    max=self.flbits(o, 0x208), value=self.flbits(o, 0x20C), lock=self.s32(o, 0x210))

    def label_state(self, name):
        o = self.objs[name]
        p = self.u32(o, C_TEXT)
        return dict(bounds=self.bounds(o), text=self.e.cstr(p) if p else "", hint=self.hint(name))

    def panel_state(self, name):
        o = self.objs[name]
        return dict(bounds=self.bounds(o), visible=self.visible(name))

    def edit_state(self):
        name = "progNameEdit"
        return dict(bounds=self.bounds(self.objs[name]), visible=self.visible(name), text=self.window(name).text,
                    hint=self.hint(name))

    def child_order(self):
        """Windowed children of the form in z-order (oracle compose order), by form field name."""
        out = []
        for h in self.g.windows[self.ed.form].children:
            o = self.e.u32(self.g.windows[h].wndproc + 9)
            out.append(self.name_of.get(o, "?"))
        return out

    def state(self):
        return dict(
            lcd={n: self.lcd_state(n) for n in LCD_NAMES},
            knobs={n: self.knob_state(n) for n in KNOB_NAMES},
            buttons={n: self.button_state(n) for n in BUTTON_NAMES},
            leds={n: self.led_state(n) for n in LED_NAMES},
            labels={n: self.label_state(n) for n in ("StatusLabel1", "StatusLabel2")},
            panels={n: self.panel_state(n) for n in ("StatusPanel2", "Panel1")},
            edit=self.edit_state(),
            images={n: dict(bounds=self.bounds(self.objs[n]), hint=self.hint(n)) for n in IMAGE_NAMES},
            order=self.child_order(),
        )

    def frame(self):
        return self.ed.frame()

    def capture_name(self):
        """VCL object name of the window holding the mouse capture ('' if none)."""
        h = self.g.capture
        if not h or h not in self.g.windows:
            return ""
        if h == self.ed.form:
            return "plugEditForm"
        return self.name_of.get(self.e.u32(self.g.windows[h].wndproc + 9), "?")


# ===================================================================== C++ side
LIB_PATH = os.environ.get("SQ8L_TESTAPI", os.path.join(ROOT, "build", "libsq8l_testapi.dylib"))
W, H = 626, 430
KNOB_FIELDS = ("value", "min", "max", "range", "step", "pixFactor", "snapZone", "minFineFac", "acc", "angle",
               "angleK", "maxPixDist", "maxFinePixDist", "radius", "fx", "fy", "downX", "downY", "lastX", "lastY",
               "snapToZero", "intMode", "dragging", "dragActive", "loaded", "force", "textValid")


def _lib():
    import ctypes
    L = ctypes.CDLL(LIB_PATH)
    vp, i32, u32, cp = ctypes.c_void_p, ctypes.c_int32, ctypes.c_uint32, ctypes.c_char_p
    P = ctypes.POINTER
    sig = {
        "sq8l_gui_new": ([], vp), "sq8l_gui_free": ([vp], None),
        "sq8l_gui_render": ([vp, P(ctypes.c_uint8)], None), "sq8l_gui_find": ([vp, cp], vp),
        "sq8l_gui_zorder": ([vp, cp, i32], i32),
        "sq8l_gui_events": ([vp, cp, i32], i32), "sq8l_gui_mouse": ([vp, i32, i32, i32, i32], None),
        "sq8l_gui_capture": ([vp, cp, i32], i32), "sq8l_gui_set_capture": ([vp, cp], None),
        "sq8l_gui_set_bounds": ([vp, i32, i32, i32, i32], None), "sq8l_gui_get_bounds": ([vp, P(i32)], None),
        "sq8l_gui_set_visible": ([vp, i32], None), "sq8l_gui_hint": ([vp, cp, i32], i32),
        "sq8l_gui_paint_control": ([vp, P(ctypes.c_uint8), i32], i32),
        "sq8l_gui_lcd_setup": ([vp, cp, i32, i32, i32, i32, i32, i32, P(u32)], None),
        "sq8l_gui_lcd_set_charmap": ([vp, cp, i32, i32], None),
        "sq8l_gui_lcd_set_charmap_raw": ([vp, P(i32), i32], None),
        "sq8l_gui_lcd_set_cell": ([vp, i32, i32, i32, i32, i32], None),
        "sq8l_gui_lcd_get_cells": ([vp, P(i32)], None),
        "sq8l_gui_lcd_write": ([vp, i32, i32, cp, i32, i32], None),
        "sq8l_gui_lcd_info": ([vp, P(i32)], None), "sq8l_gui_lcd_charmap": ([vp, P(i32)], None),
        "sq8l_gui_lcd_cell_at": ([vp, i32, i32, P(i32)], None),
        "sq8l_gui_knob_get": ([vp, P(u32)], None), "sq8l_gui_knob_set": ([vp, P(u32)], None),
        "sq8l_gui_knob_call": ([vp, i32, u32, i32, i32], None), "sq8l_gui_knob_frame": ([vp], i32),
        "sq8l_gui_button_set": ([vp, cp, i32, i32, i32, i32, i32, u32, i32], None),
        "sq8l_gui_button_get": ([vp, P(i32)], None),
        "sq8l_gui_ani_set": ([vp, cp, i32, i32, u32, u32, u32], None), "sq8l_gui_ani_get": ([vp, P(u32)], None),
        "sq8l_gui_ani_call": ([vp, i32, u32], None),
        "sq8l_gui_label_set": ([vp, cp], None), "sq8l_gui_edit_set": ([vp, cp], None),
        "sq8l_gui_sprite_frame": ([cp, i32, P(ctypes.c_uint8), P(i32)], i32),
        "sq8l_gui_background": ([P(ctypes.c_uint8)], None),
    }
    for name, (args, res) in sig.items():
        fn = getattr(L, name)
        fn.argtypes = args
        fn.restype = res
    return L


_L = None


def lib():
    global _L
    if _L is None:
        _L = _lib()
    return _L


class CppGui:
    """The C++ EditorView (tests/capi_gui.cpp)."""

    def __init__(self):
        import ctypes
        self.ct = ctypes
        self.L = lib()
        self.v = self.L.sq8l_gui_new()

    def __del__(self):
        if getattr(self, "v", None):
            self.L.sq8l_gui_free(self.v)
            self.v = None

    def _arr(self, ctype, n):
        return (ctype * n)()

    def c(self, name):
        p = self.L.sq8l_gui_find(self.v, name.encode())
        assert p, name
        return p

    def render(self):
        import numpy as np
        buf = self._arr(self.ct.c_uint8, W * H * 3)
        self.L.sq8l_gui_render(self.v, buf)
        return np.frombuffer(bytes(buf), np.uint8).reshape(H, W, 3)

    def _str(self, fn, *args):
        buf = self.ct.create_string_buffer(1 << 16)
        fn(*args, buf, len(buf))
        return buf.value.decode("latin1")

    def zorder(self):
        return self._str(self.L.sq8l_gui_zorder, self.v).split("\n")[:-1]

    def events(self):
        return self._str(self.L.sq8l_gui_events, self.v).split("\n")[:-1]

    def capture(self):
        return self._str(self.L.sq8l_gui_capture, self.v)

    def set_capture(self, name):
        self.L.sq8l_gui_set_capture(self.v, name.encode())

    def hint(self, name):
        return self._str(self.L.sq8l_gui_hint, self.c(name))

    def mouse(self, msg, x, y, keys=0):
        self.L.sq8l_gui_mouse(self.v, msg, x, y, keys)

    def bounds(self, name):
        o = self._arr(self.ct.c_int32, 4)
        self.L.sq8l_gui_get_bounds(self.c(name), o)
        return list(o)

    def set_bounds(self, name, b):
        self.L.sq8l_gui_set_bounds(self.c(name), *b)

    # ---- per control
    def knob(self, name):
        o = self._arr(self.ct.c_uint32, len(KNOB_FIELDS))
        self.L.sq8l_gui_knob_get(self.c(name), o)
        return dict(zip(KNOB_FIELDS, list(o)))

    def set_knob(self, name, d):
        a = self._arr(self.ct.c_uint32, len(KNOB_FIELDS))
        for i, k in enumerate(KNOB_FIELDS):
            a[i] = d[k] & 0xFFFFFFFF
        self.L.sq8l_gui_knob_set(self.c(name), a)

    def button(self, name):
        o = self._arr(self.ct.c_int32, 8)
        self.L.sq8l_gui_button_get(self.c(name), o)
        return dict(zip(("aniIdx", "twoFrames", "pressed", "hover", "frameWidth", "frameColor", "pressDisp",
                         "frameIndex"), list(o)))

    def ani(self, name):
        o = self._arr(self.ct.c_uint32, 6)
        self.L.sq8l_gui_ani_get(self.c(name), o)
        return dict(zip(("startFrame", "numFrames", "min", "max", "value", "frameIndex"), list(o)))

    def lcd_info(self, name):
        o = self._arr(self.ct.c_int32, 21)
        self.L.sq8l_gui_lcd_info(self.c(name), o)
        keys = ("charW", "charH", "gapX", "gapY", "frameW", "frameH", "widthPix", "heightPix", "cols", "rows",
                "nattr", "width", "height", "flags", "lastX", "lastY")
        d = dict(zip(keys, list(o)[:16]))
        d["colors"] = [v & 0xFFFFFFFF for v in list(o)[16:21]]
        return d

    def lcd_cells(self, name):
        info = self.lcd_info(name)
        n = info["rows"] * info["cols"]
        o = self._arr(self.ct.c_int32, 3 * n)
        self.L.sq8l_gui_lcd_get_cells(self.c(name), o)
        flat = list(o)
        return [[tuple(flat[3 * (r * info["cols"] + c):3 * (r * info["cols"] + c) + 3]) for c in range(info["cols"])]
                for r in range(info["rows"])]

    def lcd_charmap(self, name):
        o = self._arr(self.ct.c_int32, 256)
        self.L.sq8l_gui_lcd_charmap(self.c(name), o)
        return list(o)

    def lcd_write(self, name, col, row, text, attr):
        b = text.encode("latin1")
        self.L.sq8l_gui_lcd_write(self.c(name), col, row, b, len(b), attr)

    # ---- load a full state read from the original (OriginalGui.state())
    def load_state(self, st):
        ct = self.ct
        for name, s in st["lcd"].items():
            p = self.c(name)
            cols = (ct.c_uint32 * 3)(*s["colors"][:3])
            self.L.sq8l_gui_lcd_setup(p, s["gif"].encode(), s["cols"], s["rows"], s["gapX"], s["gapY"],
                                      s["frameW"], s["frameH"], cols)
            self.L.sq8l_gui_lcd_set_charmap_raw(p, (ct.c_int32 * 256)(*s["charmap"]), s["nattr"])
            for r, row in enumerate(s["cells"]):
                for col, (ch, attr, dirty) in enumerate(row):
                    self.L.sq8l_gui_lcd_set_cell(p, col, r, ch, attr, 1)
            self.L.sq8l_gui_set_bounds(p, *s["bounds"])
            self.L.sq8l_gui_set_visible(p, int(s["visible"]))
        for name, s in st["knobs"].items():
            p = self.c(name)
            self.L.sq8l_gui_set_bounds(p, *s["bounds"])
            self.L.sq8l_gui_set_visible(p, int(s["visible"]))
            d = dict(s)
            d["fx"], d["fy"] = s["framePos"]
            d["downX"], d["downY"] = s["downPos"]
            d["lastX"], d["lastY"] = s["lastPos"]
            d["dragging"], d["dragActive"] = s["dragging"], s["captured"]
            self.set_knob(name, d)
            assert s["gif"] == "KnobGif"
        for name, s in st["buttons"].items():
            p = self.c(name)
            self.L.sq8l_gui_set_bounds(p, *s["bounds"])
            self.L.sq8l_gui_set_visible(p, int(s["visible"]))
            self.L.sq8l_gui_button_set(p, s["gif"].encode(), s["aniIdx"], s["twoFrames"], s["pressed"], s["hover"],
                                       s["frameWidth"], s["frameColor"], s["pressDisp"])
            assert not s["caption"]
        for name, s in st["leds"].items():
            p = self.c(name)
            self.L.sq8l_gui_set_bounds(p, *s["bounds"])
            self.L.sq8l_gui_set_visible(p, int(s["visible"]))
            self.L.sq8l_gui_ani_set(p, s["gif"].encode(), s["startFrame"], s["numFrames"], s["min"], s["max"],
                                    s["value"])
        for name, s in st["panels"].items():
            p = self.c(name)
            self.L.sq8l_gui_set_bounds(p, *s["bounds"])
            self.L.sq8l_gui_set_visible(p, int(s["visible"]))
        for name, s in st["labels"].items():
            p = self.c(name)
            self.L.sq8l_gui_label_set(p, s["text"].encode("latin1"))
            self.L.sq8l_gui_set_bounds(p, *s["bounds"])
        p = self.c("progNameEdit")
        self.L.sq8l_gui_set_bounds(p, *st["edit"]["bounds"])
        self.L.sq8l_gui_set_visible(p, int(st["edit"]["visible"]))
        self.L.sq8l_gui_edit_set(p, st["edit"]["text"].encode("latin1"))


def text_masks(st):
    """Form-coordinate rectangles drawn with Windows fonts: the status labels (clipped to their
    panels) and the program name edit box."""
    rects = []
    for label, panel in (("StatusLabel1", "Panel1"), ("StatusLabel2", "StatusPanel2")):
        px, py, pw, ph = st["panels"][panel]["bounds"]
        lx, ly, lw, lh = st["labels"][label]["bounds"]
        x0, y0 = max(px + lx, px), max(py + ly, py)
        x1, y1 = min(px + lx + lw, px + pw), min(py + ly + lh, py + ph)
        if x1 > x0 and y1 > y0:
            rects.append((label, x0, y0, x1, y1))
    x, y, w, h = st["edit"]["bounds"]
    rects.append(("progNameEdit", x, y, x + w, y + h))
    return rects


if __name__ == "__main__":
    og = OriginalGui()
    og.frame()
    st = og.state()
    for k in ("lcd",):
        for n, s in st[k].items():
            s = dict(s)
            s["charmap"] = "..."
            s["cells"] = ["".join(chr(c[0]) if c[0] >= 32 else "." for c in row) for row in s["cells"]]
            print(n, s)
    for k in ("knobs", "buttons", "leds", "labels", "panels", "images"):
        for n, s in st[k].items():
            print(k, n, s)
    print("edit", st["edit"])
    print("order", st["order"])
