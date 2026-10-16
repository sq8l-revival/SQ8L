"""Additions of the port to the editor (not in the original), on the C++ logic alone.

The editor is no longer compared with the original, so these are the port's own checks of
what it adds:
  * OPTIONS "Down arrow loads next program": the original's hidden swapProgUpDn ini key;
  * OPTIONS "Ask before loading banks/libraries" ([port] confirmLoad): no prompt when off;
  * a left click on the program number opens the program list (the original: right only);
  * EMU "VOICES" (program byte 0x197): the playable voices of the program, 1 to 64, with the
    original's 8 for every program that does not have the parameter;
  * OPTIONS "Polyphony": the same per instance, 0 = set by the program;
  * OSC "DIR" (program osc byte 0x0b): the wave playback direction of each oscillator, with
    WAVE= shortened to WF= to make room for it, the same label on the OSC1-3 and LFO pages,
    and the LFO field widened to spell the oscillator wave names out (issue #44);
  * OPTIONS "Output gain": a cut of this instance's output, also kept in SQ8L.ini as the
    cut a newly created instance starts at (issue #52);
  * OPTIONS "Zoom" ([port] zoom): the editor's size, 100% to 300%;
  * the knobs a display page has no parameter for are marked inactive (drawn faded);
  * OPTIONS "Mouse" -> "Hide cursor when editing": the cursor is hidden and pinned for the
    length of a knob turn, so the turn has no bounds, and comes back where it started;
  * a right click on a knob opens its value menu (the original: only a double click);
  * a parameter with only two values gets a soft button in place of its knob, and a click
    on the button or on its field in the display flips it (issue #43);
  * the mouse wheel over a knob or its display field steps the parameter, one notch at a time
    whatever the device's delta scale, and OPTIONS "Mouse" -> "Natural scrolling" inverts it
    (issue #35);
  * the version of the build (cmake/Version.cmake) on the boot message, with its dots drawn as
    the decimal-point attribute of the cell before them rather than as characters.

Self-contained (no original files needed):
  SQ8L_TESTAPI=$PWD/build/libsq8l_testapi.dylib python3 tests/test_gui_extensions.py
"""
import ctypes
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIB_PATH = os.environ.get("SQ8L_TESTAPI", os.path.join(ROOT, "build", "libsq8l_testapi.dylib"))

WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_RBUTTONDOWN, WM_RBUTTONUP = 0x200, 0x201, 0x202, 0x204, 0x205
MK_LBUTTON, MK_RBUTTON, MK_CONTROL = 1, 2, 8
# A notch with Ctrl held moves this many steps (EditorController::kWheelCtrlStep).
WHEEL_CTRL_STEP = 10
# lcdKnob0-9 (DFM): 40 x 40 at these centres, 0-4 top row, 5-9 bottom row. The soft button of
# a two-state slot (issue #43) is centred in the same 40 x 40, so these are its centre too.
def knob_xy(i):
    return 118 + 96 * (i % 5) + 20, (194 if i < 5 else 304) + 20



# The centre of VFD cell (col, row): the display sits at (34, 247) with a frame of 4 and
# 11 x 18 characters at gaps of 1 / 3 (EditorView).
def cell_xy(col, row):
    return 38 + 12 * col + 5, 251 + 21 * row + 9

# Form positions (DFM; numLcd is sized at run time)
NUM_LCD = (40, 52)
UP, DOWN = (296, 48), (296, 67)
OPTIONS, FILE = (105, 12), (37, 12)
HIDE_CURSOR, NATURAL_SCROLL = "Hide cursor when editing", "Natural scrolling"
# FILE items: 0 load library, 1 save library, 2 init library, 3 line, 4 load bank, 5 save bank
FILE_LOAD_LIB, FILE_SAVE_LIB, FILE_LOAD_BANK, FILE_SAVE_BANK = 0, 1, 4, 5

failures = []


def opt_index(opts, caption):
    """The position of an OPTIONS item by caption.

    The port regroups OPTIONS and its layout is expected to keep changing, so nothing here
    asserts the order; the items the behaviour checks drive are located by name instead, and a
    missing one fails loudly.
    """
    for i, it in enumerate(opts):
        if text(it) == caption:
            return i
    raise AssertionError(f"OPTIONS has no {caption!r}: {[text(i) for i in opts]}")


def sub_index(item, caption):
    """The position of a submenu child by caption, for the same reason as opt_index: the
    menus are expected to keep changing, so nothing here is driven by a fixed position."""
    kids = item.get("sub", [])
    for i, it in enumerate(kids):
        if text(it) == caption:
            return i
    raise AssertionError(f"{text(item)!r} has no {caption!r}: {[text(i) for i in kids]}")


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures.append(what)


class Editor:
    def __init__(self, L):
        self.L = L
        self.v = L.sq8l_gl_new(1)
        L.sq8l_gl_set_host(self.v, 8, 0, b"/sandbox")
        L.sq8l_gl_show(self.v)
        self.idle()
        self.events()

    def idle(self, ms=100):
        self.L.sq8l_gl_idle(self.v, ms)

    def click(self, xy, right=False):
        x, y = xy
        self.L.sq8l_gl_mouse(self.v, WM_MOUSEMOVE, x, y, 0)
        if right:
            self.L.sq8l_gl_mouse(self.v, WM_RBUTTONDOWN, x, y, MK_RBUTTON)
            self.L.sq8l_gl_mouse(self.v, WM_RBUTTONUP, x, y, 0)
        else:
            self.L.sq8l_gl_mouse(self.v, WM_LBUTTONDOWN, x, y, MK_LBUTTON)
            self.L.sq8l_gl_mouse(self.v, WM_LBUTTONUP, x, y, 0)
        self.idle()

    def drag_knob(self, i, dy=20, release=True):
        """Turn knob i: the mouse messages the plugin UI would send."""
        x, y = knob_xy(i)
        self.L.sq8l_gl_mouse(self.v, WM_MOUSEMOVE, x, y, 0)
        self.L.sq8l_gl_mouse(self.v, WM_LBUTTONDOWN, x, y, MK_LBUTTON)
        self.L.sq8l_gl_mouse(self.v, WM_MOUSEMOVE, x, y - dy, MK_LBUTTON)
        if release:
            self.L.sq8l_gl_mouse(self.v, WM_LBUTTONUP, x, y - dy, 0)
        self.idle()

    def context_menu(self, xy):
        x, y = xy
        self.L.sq8l_gl_mouse(self.v, WM_MOUSEMOVE, x, y, 0)
        self.L.sq8l_gl_context_menu(self.v, x, y)
        self.idle()

    def cursor_events(self, evs):
        """Hide/show and position events in order: False/True, or (x, y) for a move."""
        return [e["visible"] if e["ev"] == "cursor" else (e["x"], e["y"])
                for e in evs if e["ev"] in ("cursor", "setcursor")]

    def cursor_hidden_at(self, evs, point):
        """True if the events are: hidden, pinned at `point` throughout, shown again."""
        ev = self.cursor_events(evs)
        return (len(ev) >= 3 and ev[0] is False and ev[-1] is True
                and all(p == point for p in ev[1:-1]))

    def press(self, xy):
        """A press that opens a menu, as the plugin UI sees it.

        The menu is modal and keeps the button up, so the editor only ever gets the button
        down; the plugin UI then cancels the mouse capture itself (SQ8LUI::onMouse), which
        scripted input has to stand in for.
        """
        x, y = xy
        self.L.sq8l_gl_mouse(self.v, WM_MOUSEMOVE, x, y, 0)
        self.L.sq8l_gl_mouse(self.v, WM_LBUTTONDOWN, x, y, MK_LBUTTON)
        self.L.sq8l_gl_cancel_mouse_mode(self.v)
        self.idle()

    def choose(self, path):
        self.L.sq8l_gl_menu_choice(self.v, (ctypes.c_int32 * max(len(path), 1))(*path), len(path))

    def answer_file(self, path):
        self.L.sq8l_gl_file_answer(self.v, path.encode())

    def cstr(self, fn, size=1 << 20):
        # (the event log is cleared by the call: one call with a large buffer)
        buf = ctypes.create_string_buffer(size)
        n = fn(self.v, buf, size)
        assert n < size, "buffer too small"
        return buf.raw[:n].decode("latin1")

    def events(self):
        return [json.loads(line) for line in self.cstr(self.L.sq8l_gl_events).splitlines() if line.startswith("{")]

    def popups(self, evs):
        return [e["tree"] for e in evs if e["ev"] == "popup"]

    def menu(self, xy):
        """Open a menu, dismiss it, return its items."""
        self.choose([])
        self.click(xy)
        trees = self.popups(self.events())
        return trees[-1] if trees else None

    def program(self):
        row = json.loads(self.cstr(self.L.sq8l_gl_state))["numLcd"][0]
        return "".join(chr(c[0]) for c in row).strip()

    def settings(self):
        out = (ctypes.c_int32 * 11)()
        self.L.sq8l_gl_settings(self.v, out)
        return list(out)

    def knob_active(self, i):
        return bool(self.L.sq8l_gl_knob_active(self.v, i))

    def toggle_shown(self, i):
        """Whether knob slot i shows its soft button instead of the knob (issue #43)."""
        return bool(self.L.sq8l_gl_toggle_shown(self.v, i))

    def wheel(self, xy, delta, keys=0):
        """One wheel event at xy, `delta` as the platform would report it (issue #35)."""
        x, y = xy
        self.L.sq8l_gl_mouse(self.v, WM_MOUSEMOVE, x, y, 0)
        self.L.sq8l_gl_wheel(self.v, x, y, delta, keys)
        self.idle()

    def drag(self, xy, dy, release_at=None):
        """Press at xy, move up by dy in steps, release there or at release_at."""
        x, y = xy
        self.L.sq8l_gl_mouse(self.v, WM_MOUSEMOVE, x, y, 0)
        self.L.sq8l_gl_mouse(self.v, WM_LBUTTONDOWN, x, y, MK_LBUTTON)
        for step in (dy // 3, 2 * dy // 3, dy):
            self.L.sq8l_gl_mouse(self.v, WM_MOUSEMOVE, x, y - step, MK_LBUTTON)
        rx, ry = release_at if release_at else (x, y - dy)
        self.L.sq8l_gl_mouse(self.v, WM_LBUTTONUP, rx, ry, 0)
        self.idle()

    def double_click(self, xy):
        """Down, up, WM_LBUTTONDBLCLK (= DblClick + a down), up: what Windows sends."""
        x, y = xy
        self.L.sq8l_gl_mouse(self.v, WM_MOUSEMOVE, x, y, 0)
        self.L.sq8l_gl_mouse(self.v, WM_LBUTTONDOWN, x, y, MK_LBUTTON)
        self.L.sq8l_gl_mouse(self.v, WM_LBUTTONUP, x, y, 0)
        self.L.sq8l_gl_mouse(self.v, 0x203, x, y, MK_LBUTTON)
        self.L.sq8l_gl_mouse(self.v, WM_LBUTTONUP, x, y, 0)
        self.idle()

    def knob(self, i, field="value"):
        """A float field of knob i (the state dump stores floats as their bits)."""
        bits = json.loads(self.cstr(self.L.sq8l_gl_state))["knobs"][i][field]
        return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]

    def lcd_row(self, y):
        row = json.loads(self.cstr(self.L.sq8l_gl_state))["lcd"][y]
        return "".join(chr(c[0]) for c in row)

    def lcd_attrs(self, y):
        """The attribute of every cell of row y: 0 plain, 1 underline, 2 dot, 3 both."""
        return [c[1] for c in json.loads(self.cstr(self.L.sq8l_gl_state))["lcd"][y]]

    def pages(self):
        return json.loads(self.cstr(self.L.sq8l_gl_pages))

    def program_byte(self, off):
        """A byte of the edit buffer's current program (state image: slots at +4, ring +0xa90)."""
        img = ctypes.create_string_buffer(0xB8C)
        self.L.sq8l_gl_editbuffer(self.v, img)
        ring = int.from_bytes(img.raw[0xA90:0xA94], "little")
        return img.raw[4 + ring * 0x21C + off]


def text(item):
    return item["text"].replace("&", "")


def param_of(sub, name):
    """A parameter of a sub-page of the pages dump, by name."""
    for p in sub["params"]:
        if p["name"] == name:
            return p
    raise AssertionError(f"the sub-page has no {name!r}: {[p['name'] for p in sub['params']]}")


def main():
    L = ctypes.CDLL(LIB_PATH)
    vp = ctypes.c_void_p
    for name, args, res in [
        ("sq8l_gl_new", [ctypes.c_int], vp),
        ("sq8l_gl_set_host", [vp, ctypes.c_int, ctypes.c_int, ctypes.c_char_p], None),
        ("sq8l_gl_show", [vp], None), ("sq8l_gl_idle", [vp, ctypes.c_int32], None),
        ("sq8l_gl_mouse", [vp, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32], None),
        ("sq8l_gl_cancel_mouse_mode", [vp], None),
        ("sq8l_gl_menu_choice", [vp, ctypes.POINTER(ctypes.c_int32), ctypes.c_int32], None),
        ("sq8l_gl_file_answer", [vp, ctypes.c_char_p], None),
        ("sq8l_gl_events", [vp, ctypes.c_char_p, ctypes.c_int32], ctypes.c_int32),
        ("sq8l_gl_state", [vp, ctypes.c_char_p, ctypes.c_int32], ctypes.c_int32),
        ("sq8l_gl_settings", [vp, ctypes.POINTER(ctypes.c_int32)], None),
        ("sq8l_gl_port_setting", [vp, ctypes.c_int32], ctypes.c_int32),
        ("sq8l_gl_poly_override", [vp], ctypes.c_int32),
        ("sq8l_gl_output_cut", [vp], ctypes.c_int32),
        ("sq8l_gl_knob_active", [vp, ctypes.c_int32], ctypes.c_int32),
        ("sq8l_gl_toggle_shown", [vp, ctypes.c_int32], ctypes.c_int32),
        ("sq8l_gl_wheel", [vp, ctypes.c_int32, ctypes.c_int32, ctypes.c_double, ctypes.c_int32], None),
        ("sq8l_gl_pages", [vp, ctypes.c_char_p, ctypes.c_int32], ctypes.c_int32),
        ("sq8l_gl_ctr", [vp, ctypes.c_int, ctypes.c_int, ctypes.c_int], None),
        ("sq8l_gl_context_menu", [vp, ctypes.c_int32, ctypes.c_int32], None),
        ("sq8l_gl_editbuffer", [vp, ctypes.c_char_p], None),
        ("sq8l_gl_write_dotted", [vp, ctypes.c_int32, ctypes.c_int32, ctypes.c_char_p, ctypes.c_int32],
         ctypes.c_int32),
        ("sq8l_gl_version", [], ctypes.c_char_p),
    ]:
        getattr(L, name).argtypes = args
        getattr(L, name).restype = res

    ed = Editor(L)
    ed.choose([])
    ed.click(NUM_LCD, right=True)
    check(len(ed.popups(ed.events())) == 1, "right click on the program number: program list")
    # The OPTIONS menu is not tested: its layout and state are left to inspection. The items
    # are located by caption only so the setting checks below can click them.
    opts = ed.menu(OPTIONS)
    OPT_GAIN = opt_index(opts, "Output gain")
    OPT_POLY = opt_index(opts, "Polyphony")
    OPT_SWAP = opt_index(opts, "Down arrow loads next program")
    OPT_CONFIRM = opt_index(opts, "Ask before loading banks/libraries")

    # 1. left click on the program number opens the program list
    ed.choose([])
    ed.click(NUM_LCD)
    trees = ed.popups(ed.events())
    check(len(trees) == 1 and sum(1 for it in trees[0] if it.get("break")) == 3,
          "left click on the program number: program list (4 columns)")

    # 1b. the menu is modal and keeps the button up, so the editor only ever sees the button
    # down (issue #8: the program number stayed captured and swallowed every later click).
    ed.choose([])
    ed.press(NUM_LCD)
    check(len(ed.popups(ed.events())) == 1, "left button down alone: program list")
    p0 = ed.program()
    ed.click(UP)
    check(not ed.popups(ed.events()), "the click after the list: no second program list")
    check(ed.program() == p0[0] + "%03d" % (int(p0[1:]) + 1), "the click after the list reaches the arrow")

    # 2. arrows, then swapped from the OPTIONS menu
    p0 = ed.program()
    ed.click(UP)
    p1 = ed.program()
    ed.click(DOWN)
    check(p1 == p0[0] + "%03d" % (int(p0[1:]) + 1) and ed.program() == p0, f"up = next ({p0} -> {p1} -> back)")
    ed.click(UP)
    ed.choose([OPT_SWAP])
    ed.click(OPTIONS)
    ed.events()
    check(ed.settings()[4] == 1, "swapProgUpDn set to 1 (saved in [gui] like the original's key)")
    check(ed.menu(OPTIONS)[OPT_SWAP]["checked"], "menu item checked")
    p0 = ed.program()
    ed.click(DOWN)
    p1 = ed.program()
    ed.click(UP)
    check(p1 == p0[0] + "%03d" % (int(p0[1:]) + 1) and ed.program() == p0, f"swapped: down = next ({p0} -> {p1})")
    ed.choose([OPT_SWAP])
    ed.click(OPTIONS)
    ed.events()
    check(ed.settings()[4] == 0 and not ed.menu(OPTIONS)[OPT_SWAP]["checked"], "unswapped again")

    # 3. load prompts: library, bank (save first, then load the saved files)
    def file_cmd(item, path):
        ed.answer_file(path)
        ed.choose([item])
        ed.click(FILE)
        return [e for e in ed.events() if e["ev"] == "msgbox"]

    file_cmd(FILE_SAVE_LIB, "/sandbox/lib.8XL")
    file_cmd(FILE_SAVE_BANK, "/sandbox/bank.8XL")
    boxes = file_cmd(FILE_LOAD_LIB, "/sandbox/lib.8XL") + file_cmd(FILE_LOAD_BANK, "/sandbox/bank.8XL")
    check([b["text"][:13] for b in boxes] == ["Load library?", "Load bank? (T"], "prompts when asked (default)")
    ed.choose([OPT_CONFIRM])
    ed.click(OPTIONS)
    ed.events()
    check(L.sq8l_gl_port_setting(ed.v, 0) == 0, "[port] confirmLoad set to 0")
    check(not ed.menu(OPTIONS)[OPT_CONFIRM]["checked"], "menu item unchecked")
    boxes = file_cmd(FILE_LOAD_LIB, "/sandbox/lib.8XL") + file_cmd(FILE_LOAD_BANK, "/sandbox/bank.8XL")
    check(boxes == [], f"no prompts when off ({[b['text'] for b in boxes]})")
    ed.choose([OPT_CONFIRM])
    ed.click(OPTIONS)
    ed.events()
    check(L.sq8l_gl_port_setting(ed.v, 0) == 1, "asking again")

    # 4. OPTIONS -> Polyphony: the override of this instance (item 1 is the separator)
    for k, n in ((7, 16), (11, 64), (2, 1), (0, 0)):
        ed.choose([OPT_POLY, k])
        ed.click(OPTIONS)
        ed.events()
        sub = ed.menu(OPTIONS)[OPT_POLY]["sub"]
        checked = [i["checked"] for i in sub if not i.get("separator")]
        want = [(j if j == 0 else j + 1) == k for j in range(11)]
        check(L.sq8l_gl_poly_override(ed.v) == n and checked == want,
              f"override {n or 'set by program'}: {L.sq8l_gl_poly_override(ed.v)}, menu checked")

    # 4b. OPTIONS -> Output gain: the cut of this instance (issue #52), which also goes to
    # SQ8L.ini (port[2]) as the cut a newly created instance starts at. The cut is what is
    # checked, not the submenu's captions or their order.
    for cap, n in (("-6 dB", 6), ("-12 dB", 12), ("-18 dB", 18), ("-24 dB", 24), ("0 dB", 0)):
        gain = ed.menu(OPTIONS)[OPT_GAIN]
        ed.choose([OPT_GAIN, sub_index(gain, cap)])
        ed.click(OPTIONS)
        ed.events()
        ticked = [text(i) for i in ed.menu(OPTIONS)[OPT_GAIN].get("sub", []) if i["checked"]]
        check(L.sq8l_gl_output_cut(ed.v) == n and L.sq8l_gl_port_setting(ed.v, 2) == n
              and ticked == [cap],
              f"cut {n} dB: instance {L.sq8l_gl_output_cut(ed.v)}, ini "
              f"{L.sq8l_gl_port_setting(ed.v, 2)}, ticked {ticked}")

    # 5. EMU -> VOICES: the playable voices of the program itself
    EMU_PAGE, EMU_SUB, VOICES = 17, 1, 5
    params = ed.pages()[EMU_PAGE]["subs"][EMU_SUB]["params"]
    found = [p for p in params if p["name"] == "VOICES"]
    p = found[0] if found else {}
    check(len(found) == 1 and p.get("knob") == 9 and p.get("x") == 33 and p.get("y") == 1,
          f"VOICES under VSTEAL: knob {p.get('knob')}, column {p.get('x')}, row {p.get('y')}")
    check(p.get("min") == 1 and p.get("max") == 64 and p.get("paramIndex") == 375 and p.get("width") == 2,
          f"VOICES is 1..64 and writes program byte 0x197 (parameter {p.get('paramIndex')})")
    L.sq8l_gl_ctr(ed.v, 0, EMU_PAGE, EMU_SUB)
    ed.idle()
    shown = ed.lcd_row(1)[33:42]
    check(shown == "VOICES=08" and ed.program_byte(0x197) == 0,
          f"a program of the original has no VOICES and shows the SQ80's 8 ({shown!r})")
    # the knob snaps to the 8 the display shows, so turning it goes to 7 or 9, and the
    # program keeps its 0 until the parameter is really edited
    check(ed.knob(9) == 8.0, f"the knob snaps to 8 for such a program ({ed.knob(9)})")
    L.sq8l_gl_ctr(ed.v, 1, VOICES, 24)
    ed.idle()
    shown = ed.lcd_row(1)[33:42]
    check(ed.program_byte(0x197) == 24 and shown == "VOICES=24",
          f"24 voices: program byte {ed.program_byte(0x197)}, display {shown!r}")
    # a program that has the parameter puts the knob on its own value (the refresh that runs
    # when the page is selected, i.e. what a program change does)
    L.sq8l_gl_ctr(ed.v, 0, EMU_PAGE, EMU_SUB)
    ed.idle()
    check(ed.knob(9) == 24.0, f"the knob follows the program's own value ({ed.knob(9)})")

    # 6. OSC -> DIR: the wave playback direction of each oscillator (issue #44). The pages
    # are patched strings (GuiLogicDataPort.cpp): if a pattern ever stopped matching, the
    # generated page would come through unchanged and these checks are what catches it.
    print("Wave direction:")
    OSC_SUB, WF, DIR = 0, 4, 5
    for page, param in ((1, 8), (2, 27), (3, 46)):
        params = ed.pages()[page]["subs"][OSC_SUB]["params"]
        found = [p for p in params if p["name"] == "DIR"]
        p = found[0] if found else {}
        check(len(found) == 1 and p.get("knob") == 4 and p.get("x") == 40 and p.get("y") == 0,
              f"OSC{page} DIR beside the wave: knob {p.get('knob')}, column {p.get('x')}, row {p.get('y')}")
        check(p.get("min") == 0 and p.get("max") == 1 and p.get("paramIndex") == param and p.get("width") == 2,
              f"OSC{page} DIR is 0..1 and writes parameter {p.get('paramIndex')} (want {param})")
        # WAVE moved one knob left and kept its 6 cells; the MODS slots below must still
        # reach their own parameters, which no longer follow from the ordinal.
        w = [q for q in params if q["name"] == "WAVE"][0]
        mods = {q["name"]: q.get("paramIndex") for q in params if q["name"].startswith("MOD")}
        base = (page - 1) * 19
        check(w.get("knob") == 3 and w.get("x") == 30 and w.get("width") == 6,
              f"OSC{page} WF: knob {w.get('knob')}, column {w.get('x')}, width {w.get('width')}")
        check(mods == {"MOD1.SEL": base + 4, "MOD1.AMT": base + 5,
                       "MOD2.SEL": base + 6, "MOD2.AMT": base + 7},
              f"OSC{page} pitch mods still reach parameters {base + 4}..{base + 7}: {mods}")

    L.sq8l_gl_ctr(ed.v, 0, 1, OSC_SUB)
    ed.idle()
    row = ed.lcd_row(0)
    check(row[30:33] == "WF=" and row[40:42] == "->" and ed.program_byte(0x02D) == 0,
          f"a program of the original plays forward: {row[30:42]!r}")
    L.sq8l_gl_ctr(ed.v, 1, DIR, 1)
    ed.idle()
    row = ed.lcd_row(0)
    check(row[40:42] == "<-" and ed.program_byte(0x02D) == 1,
          f"reversed: display {row[40:42]!r}, program byte {ed.program_byte(0x02D)}")
    check(row[5:9] == "OCT=" and row[12:17] == "SEMI=" and row[21:26] == "FINE=" and row[33:39].strip() != "",
          f"OCT/SEMI/FINE keep the columns they had, the wave name its 6 cells: {row[:44]!r}")
    L.sq8l_gl_ctr(ed.v, 1, DIR, 0)
    ed.idle()
    check(ed.lcd_row(0)[40:42] == "->" and ed.program_byte(0x02D) == 0, "and back to forward")
    # the colon of the 16-segment font is blank, so the label has to use "="
    check(":" not in ed.lcd_row(0) and ":" not in ed.lcd_row(1), "no colon on the display (it draws blank)")

    # the LFO wave field is 6 cells now, so an oscillator wave spells its name out
    LFO1_PAGE, LFO_SUB, LFO_WAVE = 6, 0, 4
    params = ed.pages()[LFO1_PAGE]["subs"][LFO_SUB]["params"]
    w = [q for q in params if q["name"] == "WAVE"][0]
    check(w.get("width") == 6 and w.get("knob") == 4,
          f"LFO1 WF is 6 cells on knob 4 (width {w.get('width')}, knob {w.get('knob')})")
    L.sq8l_gl_ctr(ed.v, 0, LFO1_PAGE, LFO_SUB)
    ed.idle()
    L.sq8l_gl_ctr(ed.v, 1, LFO_WAVE, 5)  # the first oscillator wave after the 5 LFO shapes
    ed.idle()
    shown = ed.lcd_row(0)[38:44]
    check(shown.strip() == "SAW", f"LFO wave 5 spells the oscillator wave out ({shown!r}, was ' 00')")

    # the OSC1-3 page only follows the rename: its three wave values must not move
    L.sq8l_gl_ctr(ed.v, 0, 0, 1)
    ed.idle()
    row = ed.lcd_row(0)
    # WF= at 8-10 puts its "=" above the one of OCT= (7-10) on the row below, and the three
    # wave values stay at 13, 21 and 29 where the original had them.
    oct_row = ed.lcd_row(1)  # the OCT row of this same sub-page
    check(row[8:11] == "WF=" and oct_row[7:11] == "OCT=" and row[10] == oct_row[10],
          f"OSC1-3 WF= lines up with OCT= below: {row[:12]!r} over {oct_row[:12]!r}")
    check(row[13:19].strip() != "" and row[21:27].strip() != "" and row[29:35].strip() != "",
          f"OSC1-3 keeps the wave columns under the knobs: {row[:44]!r}")

    # 7. the knobs a page does not use are inactive (issue #24: drawn faded)
    print("Inactive knobs:")
    for page, sub in ((EMU_PAGE, EMU_SUB), (0, 0)):
        L.sq8l_gl_ctr(ed.v, 0, page, sub)
        ed.idle()
        used = ed.pages()[page]["subs"][sub]["knobs"]
        want = [k >= 0 for k in used] + [False] * (10 - len(used))
        got = [ed.knob_active(i) for i in range(10)]
        check(got == want, f"page {page}.{sub}: active knobs {got} follow the page's {want}")
    check(any(not a for a in got), "and at least one of them really is inactive")

    # 8. the cursor disappears while a knob is turned (issue #25)
    print("Hide cursor when editing:")
    ed = Editor(L)
    OPT_MOUSE = opt_index(ed.menu(OPTIONS), "Mouse")
    mouse = ed.menu(OPTIONS)[OPT_MOUSE]
    MOUSE_HIDE_CURSOR = sub_index(mouse, HIDE_CURSOR)
    check(mouse["sub"][MOUSE_HIDE_CURSOR]["checked"], f"'{HIDE_CURSOR}' is on by default")
    L.sq8l_gl_ctr(ed.v, 0, 0, 0)
    ed.idle()
    # a slot that really is a knob: a two-state parameter shows a soft button instead and is
    # clicked rather than turned, so it has no turn to hide the cursor for (issue #43)
    active = next(i for i in range(10) if ed.knob_active(i) and not ed.toggle_shown(i))
    before = ed.knob(active)
    up = 60 if before < ed.knob(active, "max") else -60  # a direction the value can move in
    ed.events()
    ed.drag_knob(active, dy=up)
    evs = ed.events()
    start = knob_xy(active)
    check(ed.cursor_hidden_at(evs, start) and ed.knob(active) != before,
          f"a turn hides the cursor, keeps it where it started, shows it "
          f"({ed.cursor_events(evs)})")
    # a turn the editor loses rather than finishes must not leave the cursor hidden
    ed.drag_knob(active, dy=up, release=False)
    L.sq8l_gl_cancel_mouse_mode(ed.v)
    ed.L.sq8l_gl_mouse(ed.v, WM_MOUSEMOVE, *knob_xy(active), MK_LBUTTON)
    ed.idle()
    check(ed.cursor_hidden_at(ed.events(), knob_xy(active)),
          "a turn that loses the mouse is undone the same way")
    # an inactive knob turns nothing, so it does not take the cursor either
    inactive = next((i for i in range(10) if not ed.knob_active(i)), -1)
    check(inactive >= 0, f"page 0.0 has an inactive knob ({inactive})")
    ed.drag_knob(inactive)
    check(ed.cursor_events(ed.events()) == [], "turning a faded knob leaves the cursor alone")
    # A double click opens the value menu, and the host then cancels the capture, which is
    # all the editor hears of it: the drag the second press started must still end, or the
    # cursor would stay hidden until the next mouse move. VOICES on the EMU page has a menu
    # (a two-value parameter would just toggle).
    L.sq8l_gl_ctr(ed.v, 0, EMU_PAGE, EMU_SUB)
    ed.idle()
    x, y = knob_xy(9)
    L.sq8l_gl_mouse(ed.v, WM_MOUSEMOVE, x, y, 0)
    L.sq8l_gl_mouse(ed.v, WM_LBUTTONDOWN, x, y, MK_LBUTTON)
    L.sq8l_gl_mouse(ed.v, WM_LBUTTONUP, x, y, 0)
    ed.choose([])  # the value menu the double click opens, dismissed
    ed.events()
    L.sq8l_gl_mouse(ed.v, 0x203, x, y, MK_LBUTTON)  # WM_LBUTTONDBLCLK
    L.sq8l_gl_cancel_mouse_mode(ed.v)               # the menu took the mouse
    ed.idle()
    evs = ed.events()
    check(ed.popups(evs) and ed.cursor_events(evs)[-1:] == [True],
          f"a double click that opens the value menu leaves the cursor visible "
          f"({ed.cursor_events(evs)})")
    # switched off in OPTIONS
    ed.choose([OPT_MOUSE, MOUSE_HIDE_CURSOR])
    ed.click(OPTIONS)
    check(ed.settings()[1] == 0, f"switching it off writes gui[1] ({ed.settings()[1]})")
    ed.events()
    ed.drag_knob(9, dy=-60)
    check(ed.cursor_events(ed.events()) == [], "with it off the cursor stays")

    # The turn has no bounds: the cursor is pinned, so the mouse can keep moving away from
    # the same screen point for as long as the user likes. Without the lock only the first
    # of these moves would do anything -- the rest arrive at a point the cursor is already
    # at -- which is what running into the edge of the monitor looks like.
    ed = Editor(L)
    L.sq8l_gl_ctr(ed.v, 0, EMU_PAGE, EMU_SUB)
    L.sq8l_gl_ctr(ed.v, 1, VOICES, 1)
    ed.idle()
    x, y = knob_xy(9)
    ed.events()
    L.sq8l_gl_mouse(ed.v, WM_MOUSEMOVE, x, y, 0)
    L.sq8l_gl_mouse(ed.v, WM_LBUTTONDOWN, x, y, MK_LBUTTON)
    steps = []
    for _ in range(5):  # small enough that five of them do not reach the top of the range
        L.sq8l_gl_mouse(ed.v, WM_MOUSEMOVE, x, y - 8, MK_LBUTTON)
        steps.append(ed.knob(9))
    L.sq8l_gl_mouse(ed.v, WM_LBUTTONUP, x, y - 8, 0)
    ed.idle()
    check(len(set(steps)) == 5 and steps == sorted(steps) and steps[-1] < 64,
          f"five moves from the same point keep turning the knob ({steps})")
    check(ed.cursor_hidden_at(ed.events(), (x, y)),
          "and the cursor sits at the start point the whole way")

    # ...but it does stop at the end of the range: a turn driven well past the top used to
    # pile the overshoot up in the accumulator, and the way back had to unwind all of it
    # before the knob answered again.
    L.sq8l_gl_ctr(ed.v, 1, VOICES, 32)
    ed.idle()
    L.sq8l_gl_mouse(ed.v, WM_MOUSEMOVE, x, y, 0)
    L.sq8l_gl_mouse(ed.v, WM_LBUTTONDOWN, x, y, MK_LBUTTON)
    for _ in range(10):  # far past the top of 1..64
        L.sq8l_gl_mouse(ed.v, WM_MOUSEMOVE, x, y - 40, MK_LBUTTON)
    at_top = ed.knob(9)
    L.sq8l_gl_mouse(ed.v, WM_MOUSEMOVE, x, y + 40, MK_LBUTTON)
    backed_off = ed.knob(9)
    L.sq8l_gl_mouse(ed.v, WM_LBUTTONUP, x, y + 40, 0)
    ed.idle()
    check(at_top == 64.0 and backed_off < at_top,
          f"the overshoot is not stored: one move back off the top answers "
          f"({at_top} -> {backed_off})")

    # 9. a right click on a knob opens its value menu (the original: a double click)
    print("Right click on a knob:")
    ed = Editor(L)
    L.sq8l_gl_ctr(ed.v, 0, EMU_PAGE, EMU_SUB)
    ed.idle()
    voices = 9  # the VOICES knob of the EMU page, 1..64
    ed.choose([])
    ed.context_menu(knob_xy(voices))
    trees = ed.popups(ed.events())
    items = [text(i) for i in trees[-1]] if trees else []
    values = [i for i in items if i.isdigit()]
    check(values[:1] == ["08"] and values[1:] == ["%02d" % v for v in range(1, 65)],
          f"the value menu of VOICES: the current value, then 1..64 ({len(values)} items)")
    # the same menu a double click opens
    ed.choose([])
    x, y = knob_xy(voices)
    L.sq8l_gl_mouse(ed.v, WM_MOUSEMOVE, x, y, 0)
    L.sq8l_gl_mouse(ed.v, WM_LBUTTONDOWN, x, y, MK_LBUTTON)
    L.sq8l_gl_mouse(ed.v, WM_LBUTTONUP, x, y, 0)
    L.sq8l_gl_mouse(ed.v, 0x203, x, y, MK_LBUTTON)  # WM_LBUTTONDBLCLK
    L.sq8l_gl_mouse(ed.v, WM_LBUTTONUP, x, y, 0)
    ed.idle()
    dbl = ed.popups(ed.events())
    check(bool(dbl) and [text(i) for i in dbl[-1]] == items, "a double click still opens it")
    # choosing a value writes the parameter
    ed.choose([items.index("24")])
    ed.context_menu(knob_xy(voices))
    check(ed.program_byte(0x197) == 24 and ed.knob(voices) == 24.0,
          f"choosing an item sets the parameter ({ed.program_byte(0x197)})")
    # a knob the page does not use keeps the page popup
    faded = next(i for i in range(10) if not ed.knob_active(i))
    ed.choose([])
    ed.context_menu(knob_xy(faded))
    trees = ed.popups(ed.events())
    check(bool(trees) and text(trees[-1][0]) == "Modulation usage...",
          "a faded knob still gets the page popup")

    # 10. a parameter with only two values is a soft button, not a knob (issue #43)
    print("Two-state parameters:")
    ed = Editor(L)
    # MODE 1/3: SYNC, AM, MONO and GLIDE over RESTART- VC, ENV, OSC, CYC -- seven fields of
    # two values and one with a range, so the same row shows both kinds.
    MODE_PAGE, MODE_SUB = 17, 0
    L.sq8l_gl_ctr(ed.v, 0, MODE_PAGE, MODE_SUB)
    ed.idle(2000)  # past the welcome message, which holds the display for its first 66 ticks
    sub = ed.pages()[MODE_PAGE]["subs"][MODE_SUB]
    want = [False] * 10
    for p in sub["params"]:
        if p["knob"] >= 0 and p["max"] - p["min"] == 1:
            want[p["knob"]] = True
    check(1 < sum(want) < 10, "the MODE page mixes two-state fields with a knob and empty slots")
    got = [ed.toggle_shown(i) for i in range(10)]
    check(got == want, f"a button in every two-state slot and nowhere else: {got}")

    sync, glide = param_of(sub, "SYNC"), param_of(sub, "GLIDE")

    def shows(p):
        """What p's value field reads on the display (the value follows p's label)."""
        start = p["x"] + len(p["label"])
        return ed.lcd_row(p["y"])[start:start + p["width"]].strip()

    field = cell_xy(sync["x"] + len(sync["label"]), sync["y"])
    button = knob_xy(sync["knob"])
    off = shows(sync)
    ed.click(button)
    on = shows(sync)
    check({off, on} == {"OFF", "ON"}, f"a click on the button flips SYNC: {off} -> {on}")
    ed.click(button)
    check(shows(sync) == off, "and a second click puts it back")
    ed.click(field)
    check(shows(sync) == on, "a click on its field in the display does the same")
    ed.click(field)
    check(shows(sync) == off, "and so does the next one")

    # a drag is what the button replaces: it must not move the value, and (unlike a knob
    # turn, issue #25) it must not take the cursor away either
    ed.events()
    ed.drag(field, 60)
    check(shows(sync) == off, "dragging the field changes nothing")
    check(ed.cursor_events(ed.events()) == [], "and does not hide the cursor, as a turn would")
    ed.drag(button, 60)
    check(shows(sync) == off, "a press dragged off the button is cancelled, as on any button")

    # FUN_0045b2b0 flips a range of two instead of showing a menu of it; with the single
    # click doing that, the double click must not, or two clicks and a double click differ
    ed.events()
    ed.double_click(field)
    check(shows(sync) == off, "a double click is two clicks and lands where it started")
    check(ed.popups(ed.events()) == [], "and opens no value menu")

    # the slots that are not two-state are untouched
    check(not ed.toggle_shown(glide["knob"]), "GLIDE, which has a range, keeps its knob")
    before = ed.knob(glide["knob"])
    ed.drag_knob(glide["knob"], dy=20)
    check(ed.knob(glide["knob"]) != before,
          f"and still turns ({before} -> {ed.knob(glide['knob'])})")

    # 11. the mouse wheel over a knob, or over the display field of one (issue #35)
    print("Mouse wheel:")
    ed = Editor(L)
    L.sq8l_gl_ctr(ed.v, 0, MODE_PAGE, MODE_SUB)
    ed.idle(2000)
    sub = ed.pages()[MODE_PAGE]["subs"][MODE_SUB]
    glide, sync = param_of(sub, "GLIDE"), param_of(sub, "SYNC")
    knob = knob_xy(glide["knob"])
    field = cell_xy(glide["x"] + len(glide["label"]), glide["y"])

    start = ed.knob(glide["knob"])
    ed.wheel(knob, 1.0)
    check(ed.knob(glide["knob"]) == start + 1, f"a notch up adds one ({start} -> {ed.knob(glide['knob'])})")
    ed.wheel(knob, -1.0)
    check(ed.knob(glide["knob"]) == start, "and a notch down takes it off again")
    ed.wheel(field, 1.0)
    check(ed.knob(glide["knob"]) == start + 1, "the wheel over its field in the display does the same")
    ed.wheel(field, -1.0)

    # a delta several notches long steps that many times: the notch is the unit, not the event
    ed.wheel(knob, 3.0)
    check(ed.knob(glide["knob"]) == start + 3, f"a delta of three notches steps three times ({ed.knob(glide['knob'])})")
    ed.wheel(knob, -3.0)

    ed.wheel(knob, 1.0, MK_CONTROL)
    check(ed.knob(glide["knob"]) == start + WHEEL_CTRL_STEP,
          f"Ctrl makes a notch a jump of {WHEEL_CTRL_STEP} ({ed.knob(glide['knob'])})")
    ed.wheel(knob, -1.0, MK_CONTROL)
    check(ed.knob(glide["knob"]) == start, "and back")

    # what one notch is worth is learned from the smallest delta the device ever reports, so a
    # device that trickles is not ten times slower than one that sends a detent at a time
    fine = Editor(L)
    L.sq8l_gl_ctr(fine.v, 0, MODE_PAGE, MODE_SUB)
    fine.idle(2000)
    base = fine.knob(glide["knob"])
    fine.wheel(knob, 0.1)                    # the first delta is taken for one notch
    check(fine.knob(glide["knob"]) == base + 1, "a trickling device still steps once per notch")
    fine.wheel(knob, 1.0)                    # ten of those notches at once
    check(fine.knob(glide["knob"]) == base + 11,
          f"and a delta ten notches long steps ten times ({fine.knob(glide['knob'])})")

    # the fields that are buttons now, and the slots the page does not use, ignore it
    before_sync = ed.lcd_row(sync["y"])[sync["x"]:sync["x"] + len(sync["label"]) + sync["width"]]
    ed.wheel(knob_xy(sync["knob"]), 1.0)
    check(ed.lcd_row(sync["y"])[sync["x"]:sync["x"] + len(sync["label"]) + sync["width"]] == before_sync,
          "a two-state slot ignores the wheel, as it is a button now")
    faded = next(i for i in range(10) if not ed.knob_active(i))
    ed.wheel(knob_xy(faded), 1.0)
    check(ed.knob(glide["knob"]) == start, f"and a slot the page does not use ({faded}) turns nothing")

    # natural scrolling inverts it, for a touchpad. Off the floor of the range first, or an
    # inverted notch would only hit the clamp and prove nothing.
    ed.wheel(knob, 5.0)
    mid = ed.knob(glide["knob"])
    check(mid == start + 5, f"five notches up from the floor of the range ({mid})")
    opts = ed.menu(OPTIONS)
    OPT_MOUSE = opt_index(opts, "Mouse")
    mouse = opts[OPT_MOUSE]
    MOUSE_NATURAL = sub_index(mouse, NATURAL_SCROLL)
    check(not mouse["sub"][MOUSE_NATURAL]["checked"], "off by default, so the wheel keeps working as it did")
    ed.choose([OPT_MOUSE, MOUSE_NATURAL])
    ed.click(OPTIONS)
    check(L.sq8l_gl_port_setting(ed.v, 1) == 1,
          f"switching it on writes port[1] ({L.sq8l_gl_port_setting(ed.v, 1)})")
    ed.wheel(knob, 1.0)
    check(ed.knob(glide["knob"]) == mid - 1, f"a notch up now takes one off ({ed.knob(glide['knob'])})")
    ed.wheel(knob, -1.0)
    check(ed.knob(glide["knob"]) == mid, "and a notch down adds it back")

    # 12. the version of this build, injected by cmake/Version.cmake. On the VFD a '.' is the
    # decimal-point attribute of the cell before it, not a character: the '.' glyph lights the
    # underline segment (the same one the highlight uses), while the attribute draws a real dot
    # and costs no column.
    w = L.sq8l_gl_write_dotted(ed.v, 0, 0, b"1.0.0.624BF5D", 0)
    check(w == 10, f"a dotted version takes 10 columns, not the 13 of its characters ({w})")
    check(ed.lcd_row(0)[:10] == "100624BF5D", f"the dots are not written as cells ({ed.lcd_row(0)[:10]!r})")
    check(ed.lcd_attrs(0)[:10] == [2, 2, 2, 0, 0, 0, 0, 0, 0, 0],
          f"each one is the decimal point of the cell before it ({ed.lcd_attrs(0)[:10]})")
    check("." not in ed.lcd_row(0), "no '.' reaches the display, where it would draw an underline")
    L.sq8l_gl_write_dotted(ed.v, 0, 0, b"1.0", 1)
    check(ed.lcd_attrs(0)[:2] == [3, 1],
          f"a dot on a highlighted cell is attribute 3, both ({ed.lcd_attrs(0)[:2]})")
    check(L.sq8l_gl_write_dotted(ed.v, 0, 1, b".5", 0) == 1 and ed.lcd_row(1)[0] == "5",
          "a leading dot has no cell to mark and is dropped")
    check(L.sq8l_gl_write_dotted(ed.v, 0, 1, b"1..0", 0) == 2 and ed.lcd_attrs(1)[:2] == [2, 0],
          "a doubled dot marks the same cell once")

    # the boot message of the first instance carries it (EditorController::show)
    version = L.sq8l_gl_version().decode()
    check(version.count(".") >= 2, f"the build injected a version: {version!r}")
    boot = Editor(L)
    # a message is centred on cells cleared to 0, not to blanks
    greeting, line = boot.lcd_row(0).strip("\0"), boot.lcd_row(1).strip("\0")
    check("CROSS WAVE SYNTHESIZER" in greeting, f"the first instance greets ({greeting!r})")
    check(line == ("VERSION " + version.upper()).replace(".", ""),
          f"and names the build below it ({line!r})")
    lead = len(boot.lcd_row(1)) - len(boot.lcd_row(1).lstrip("\0"))
    check(lead == 22 - len(line) // 2,
          f"centred on the columns it occupies, not on its characters ({lead} blank before it)")
    check(boot.lcd_attrs(1).count(2) == version.count("."),
          f"one decimal-point attribute per dot of {version!r} ({boot.lcd_attrs(1).count(2)})")
    check("." not in line, "the version's dots are attributes here too")

    print(f"{'FAILED' if failures else 'OK'}: {len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
