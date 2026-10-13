"""Read the internal state of the ORIGINAL editor's controls from emulator memory.

The original editor runs headless in the emulator (oracle/gui_driver.Editor). This module
locates the Delphi objects behind the form (TplugEditForm, via the VCL object-instance
thunk installed as the window procedure) and its published fields (offsets from
re/extracted/classes.json), and decodes each control's fields (offsets from the
disassembly of units LCD3, graphKnobB, GraphButton, AniDisplay, AniGIF; see
docs/modules/gui_controls.md).

Used by re/scripts/extract_gui_assets.py (decoded sprite frames / background), and a tool
for looking at what the original does.

Run directly to dump the start-up state:  python oracle/gui_state.py
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
