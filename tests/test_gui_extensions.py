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
  * the knobs a display page has no parameter for are marked inactive (drawn faded);
  * OPTIONS "Mouse" -> "Hide cursor when editing": the cursor is hidden and pinned for the
    length of a knob turn, so the turn has no bounds, and comes back where it started;
  * a right click on a knob opens its value menu (the original: only a double click).

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
# lcdKnob0-9 (DFM): 40 x 40 at these centres, 0-4 top row, 5-9 bottom row
def knob_xy(i):
    return 118 + 96 * (i % 5) + 20, (194 if i < 5 else 304) + 20

# Form positions (DFM; numLcd is sized at run time, see test_gui_logic.py)
NUM_LCD = (40, 52)
UP, DOWN = (296, 48), (296, 67)
OPTIONS, FILE = (105, 12), (37, 12)
# OPTIONS items: 0 voice stealing, 1 emulation, 2 line, 3 mouse restore, 4 rmb scroll, 5 line, 6-10
OPT_POLY, OPT_SWAP, OPT_CONFIRM, OPT_ZOOM = 6, 7, 8, 9
OPT_MOUSE, MOUSE_HIDE_CURSOR = 3, 1
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

    def knob(self, i, field="value"):
        """A float field of knob i (the state dump stores floats as their bits)."""
        bits = json.loads(self.cstr(self.L.sq8l_gl_state))["knobs"][i][field]
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
        ("sq8l_gl_knob_active", [vp, ctypes.c_int32], ctypes.c_int32),
        ("sq8l_gl_pages", [vp, ctypes.c_char_p, ctypes.c_int32], ctypes.c_int32),
        ("sq8l_gl_ctr", [vp, ctypes.c_int, ctypes.c_int, ctypes.c_int], None),
        ("sq8l_gl_context_menu", [vp, ctypes.c_int32, ctypes.c_int32], None),
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
    check(len(opts) == 10 and opts[5]["separator"], f"OPTIONS has 5 more items ({len(opts)})")
    zoom = opts[OPT_ZOOM]
    check(text(zoom) == "Zoom..." and [text(i) for i in zoom.get("sub", [])] ==
          ["100%   (SQ8L)", "125%", "150%", "175%", "200%", "250%", "300%"] and
          [i["checked"] for i in zoom["sub"]] == [True] + [False] * 6 and all(i.get("radio") for i in zoom["sub"]),
          f"'{text(zoom)}' 100..300%, radio items, 100% checked (the host's size)")
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
    ed = Editor(L, extensions=True)
    mouse = ed.menu(OPTIONS)[OPT_MOUSE]
    check([text(i) for i in mouse.get("sub", [])] ==
          ["Restore position after popup menus", "Hide cursor when editing"],
          f"OPTIONS '{text(mouse)}' has the two mouse options")
    check(mouse["sub"][MOUSE_HIDE_CURSOR]["checked"], "'Hide cursor when editing' is on by default")
    L.sq8l_gl_ctr(ed.v, 0, 0, 0)
    ed.idle()
    active = next(i for i in range(10) if ed.knob_active(i))
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
    ed = Editor(L, extensions=True)
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
    ed = Editor(L, extensions=True)
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
    # and without the additions a knob gets the original's page popup
    plain = Editor(L, extensions=False)
    plain.choose([])
    plain.context_menu(knob_xy(0))
    trees = plain.popups(plain.events())
    check(bool(trees) and text(trees[-1][0]) == "Modulation usage...",
          "without the additions a knob gets the page popup, like the original")

    print(f"{'FAILED' if failures else 'OK'}: {len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
