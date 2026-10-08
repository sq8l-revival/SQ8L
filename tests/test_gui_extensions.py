"""Additions of the port to the editor (not in the original), on the C++ logic alone.

The differential GUI tests (test_gui_logic.py) run with the additions off and must keep
matching the original; this checks the additions themselves:
  * OPTIONS "Down arrow -> next program": the original's hidden swapProgUpDn ini key;
  * OPTIONS "Ask before loading banks/libraries" ([port] confirmLoad): no prompt when off;
  * a left click on the program number opens the program list (the original: right only);
  * EMU "VOICES" (program byte 0x197): the playable voices of the program, 1 to 64, with the
    original's 8 for every program that does not have the parameter;
  * OPTIONS "Polyphony...": the same per instance, 0 = set by the program;
  * OPTIONS "Zoom..." ([port] zoom): the editor's size, 100% to 300%;
  * OPTIONS "HD graphics" ([port] hd): the editor drawn at the window's resolution.

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
MK_LBUTTON, MK_RBUTTON = 1, 2
# Form positions (DFM; numLcd is sized at run time, see test_gui_logic.py)
NUM_LCD = (40, 52)
UP, DOWN = (296, 48), (296, 67)
OPTIONS, FILE = (105, 12), (37, 12)
# OPTIONS items: 0 voice stealing, 1 emulation, 2 line, 3 mouse restore, 4 rmb scroll, 5 line, 6-10
OPT_POLY, OPT_SWAP, OPT_CONFIRM, OPT_ZOOM, OPT_HD = 6, 7, 8, 9, 10
# FILE items: 0 load library, 1 save library, 2 init library, 3 line, 4 load bank, 5 save bank
FILE_LOAD_LIB, FILE_SAVE_LIB, FILE_LOAD_BANK, FILE_SAVE_BANK = 0, 1, 4, 5

failures = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures.append(what)


class Editor:
    def __init__(self, L, extensions):
        self.L = L
        self.v = L.sq8l_gl_new_ext(1) if extensions else L.sq8l_gl_new(1)
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

    def knob(self, i):
        """The position of knob i (the state dump stores floats as their bits)."""
        bits = json.loads(self.cstr(self.L.sq8l_gl_state))["knobs"][i]["value"]
        return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]

    def lcd_row(self, y):
        row = json.loads(self.cstr(self.L.sq8l_gl_state))["lcd"][y]
        return "".join(chr(c[0]) for c in row)

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


def main():
    L = ctypes.CDLL(LIB_PATH)
    vp = ctypes.c_void_p
    for name, args, res in [
        ("sq8l_gl_new", [ctypes.c_int], vp), ("sq8l_gl_new_ext", [ctypes.c_int], vp),
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
        ("sq8l_gl_pages", [vp, ctypes.c_char_p, ctypes.c_int32], ctypes.c_int32),
        ("sq8l_gl_ctr", [vp, ctypes.c_int, ctypes.c_int, ctypes.c_int], None),
        ("sq8l_gl_editbuffer", [vp, ctypes.c_char_p], None),
    ]:
        getattr(L, name).argtypes = args
        getattr(L, name).restype = res

    print("Without the additions (the original's behaviour):")
    ed = Editor(L, extensions=False)
    opts = ed.menu(OPTIONS)
    check(len(opts) == 5, f"OPTIONS has the original's 5 items ({len(opts)})")
    ed.click(NUM_LCD)
    check(not ed.popups(ed.events()), "left click on the program number: no menu")
    ed.choose([])
    ed.click(NUM_LCD, right=True)
    check(len(ed.popups(ed.events())) == 1, "right click on the program number: program list")

    print("With the additions:")
    ed = Editor(L, extensions=True)
    opts = ed.menu(OPTIONS)
    check(len(opts) == 11 and opts[5]["separator"], f"OPTIONS has 6 more items ({len(opts)})")
    zoom = opts[OPT_ZOOM]
    check(text(zoom) == "Zoom..." and [text(i) for i in zoom.get("sub", [])] ==
          ["100%   (SQ8L)", "125%", "150%", "175%", "200%", "250%", "300%"] and
          [i["checked"] for i in zoom["sub"]] == [True] + [False] * 6 and all(i.get("radio") for i in zoom["sub"]),
          f"'{text(zoom)}' 100..300%, radio items, 100% checked (the host's size)")
    check(text(opts[OPT_HD]) == "HD graphics" and not opts[OPT_HD]["checked"],
          f"'{text(opts[OPT_HD])}' unchecked (the host's default)")
    poly = opts[OPT_POLY]
    items = [text(i) for i in poly.get("sub", []) if not i.get("separator")]
    check(text(poly) == "Polyphony..." and items ==
          ["Set by program   (EMU->VOICES parameter)", "1 voice", "2 voices", "4 voices",
           "8 voices   (SQ80)", "12 voices", "16 voices", "24 voices", "32 voices",
           "48 voices", "64 voices"],
          f"'{text(poly)}': set by program and 1..64 voices ({len(items)} items)")
    check([i["checked"] for i in poly["sub"] if not i.get("separator")] ==
          [True] + [False] * 10 and
          all(i.get("radio") for i in poly["sub"] if not i.get("separator")),
          "radio items, 'Set by program' checked by default")
    check(text(opts[OPT_SWAP]) == "Down arrow -> next program" and not opts[OPT_SWAP]["checked"],
          f"'{text(opts[OPT_SWAP])}' unchecked by default")
    check(text(opts[OPT_CONFIRM]) == "Ask before loading banks/libraries" and opts[OPT_CONFIRM]["checked"],
          f"'{text(opts[OPT_CONFIRM])}' checked by default")

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

    # 5. EMU -> VOICES: the playable voices of the program itself
    EMU_PAGE, EMU_SUB, VOICES = 17, 1, 5
    params = ed.pages()[EMU_PAGE]["subs"][EMU_SUB]["params"]
    found = [p for p in params if p["name"] == "VOICES"]
    p = found[0] if found else {}
    check(len(found) == 1 and p.get("knob") == 9 and p.get("x") == 33 and p.get("y") == 1,
          f"VOICES under VSTEAL: knob {p.get('knob')}, column {p.get('x')}, row {p.get('y')}")
    check(p.get("min") == 1 and p.get("max") == 64 and p.get("paramIndex") == 375 and p.get("width") == 2,
          f"VOICES is 1..64 and writes program byte 0x197 (parameter {p.get('paramIndex')})")
    plain = Editor(L, extensions=False)
    check(all(q["name"] != "VOICES" for q in plain.pages()[EMU_PAGE]["subs"][EMU_SUB]["params"]),
          "without the additions the EMU page is the original's")
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

    print(f"{'FAILED' if failures else 'OK'}: {len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
