"""Differential test of the editor logic (src/gui/logic) against the original editor.

The original editor runs headless in the emulator (oracle/gui_driver.Editor). The C++ side is
an EditorView driven by the EditorController (tests/capi_gui_logic.cpp), initialised with the
same engine state (library, edit buffer, settings). Both receive the same Win32 mouse messages
(form coordinates), the same timer ticks and the same answers to popup menus / message boxes /
file and modal dialogs; after every action the complete state is compared:

  * the cells (character + attribute) of both TLCD3 displays,
  * all value/range/drag fields of the 10 TGraphKnobB,
  * AniIdx / pressed / hover of the 33 TGraphButtons, the 3 LED values,
  * StatusLabel1 / StatusLabel2 captions, the program name edit text,
  * the edit buffer object (5 program slots, ring, extension buffers, bank, program, flags) and
    the library programs, the settings,
  * the page controller (page, sub-page, display lock, message timer, the current sub-page's
    parameter values / value texts / highlight flags), the form's fields, the mouse jump,
  * every popup menu (texts incl. VCL hotkeys, checks, radio, column breaks, default,
    separators, sub-menus, screen position), message box, file dialog, modal dialog content
    and SetCursorPos call (in order).

Test-only additions to the emulated Windows (patched into oracle/winapi's API registry here; the
oracle itself only got the radio/default/break flags in oracle/gui.menu_tree and a VirtualAlloc
fix: a MEM_RESERVE at a given address fails, as the emulated heap is not free there):
  * TrackPopupMenu / MessageBoxA / GetOpenFileNameA / GetSaveFileNameA answer from scripts;
  * PeekMessageA / DispatchMessageA: the modal loops of TCustomForm.ShowModal (WRITE / bank
    select dialog, MIDI port dialog, modulation usage, about) get the posted messages and then
    the messages of the scripted user actions (WM_COMMAND BN_CLICKED / LBN_SELCHANGE /
    LBN_DBLCLK, WM_KEYDOWN, clicks), finally WM_CLOSE;
  * the system window procedure keeps list box items / selection (LB_*), multi-line edit text
    (EM_*) and check box state (BM_*) for the dialogs' TListBox / TMemo / TCheckBox;
  * winmm MIDI: port names, open (succeeding or failing), SysEx out recorded, SysEx in delivered
    to the input callback (MIM_LONGDATA).

Scenarios (all compared after every step): pages (the page tree field by field, every value of
every parameter of every sub-page, every value popup), basic (every page button / sub-page:
knob drags vertical and horizontal fine, VFD clicks, double clicks with popup choices, VFD
drags), programs/menus (arrows, bank, program popups, page popup, OPTIONS, FILE, INFO,
buttons, hints), dialogs (WRITE/compare, mod usage, about, name edit), files (library / bank
load-save-init, SysEx program / bank import-export through the sandbox, message boxes),
midi (SEND / REQ / receive with ports), host (program changes from the host), random (seeded
random action sequences). A coverage report (pages x parameters x actions) ends the run.

The popup menu *trees* are not compared with the original: the port regroups OPTIONS and adds
items the original has no counterpart for, and GUI parity is not a goal of the port. The menus
are still driven, so every command they dispatch is exercised and its effect compared.

Run:  SQ8L_TESTAPI=$PWD/build/libsq8l_testapi.dylib .venv/bin/python tests/test_gui_logic.py [scenario ...]
"""
import ctypes
import json
import os
import random
import struct
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "oracle"))
sys.path.insert(0, os.path.join(ROOT, "tests"))

import gui as oracle_gui  # noqa: E402
import winapi  # noqa: E402
from gui_driver import Editor, form_components  # noqa: E402
from test_gui_state import BUTTON_NAMES, FORM_FIELDS  # noqa: E402

LIB_PATH = os.environ.get("SQ8L_TESTAPI", os.path.join(ROOT, "build", "libsq8l_testapi.dylib"))
WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDBLCLK = 0x200, 0x201, 0x202, 0x203
WM_RBUTTONDOWN, WM_RBUTTONUP = 0x204, 0x205
MK_LBUTTON, MK_RBUTTON, MK_SHIFT = 1, 2, 4
LIB_GLOBAL = 0x494314
PROG = 0x21C
EB_FIELDS = [(4, 0xA90 - 4 + 4), (0xA98, 0x6C), (0xB04, 0x6C), (0xB70, 8), (0xB84, 2), (0xB88, 4)]
MOUSEJUMP_PTR = 0x4C346C

COMPS = form_components()


def _api():
    L = ctypes.CDLL(LIB_PATH)
    vp, i32, u32, cp = ctypes.c_void_p, ctypes.c_int32, ctypes.c_uint32, ctypes.c_char_p
    P = ctypes.POINTER
    sig = {
        "sq8l_gl_new": ([i32], vp), "sq8l_gl_free": ([vp], None),
        "sq8l_gl_set_library": ([vp, cp, cp, i32], None), "sq8l_gl_set_editbuffer": ([vp, cp], None),
        "sq8l_gl_set_settings": ([vp, P(i32), P(i32)], None), "sq8l_gl_set_host": ([vp, i32, i32, cp], None),
        "sq8l_gl_set_form_origin": ([vp, i32, i32], None), "sq8l_gl_show": ([vp], None),
        "sq8l_gl_mouse": ([vp, i32, i32, i32, i32], None), "sq8l_gl_context_menu": ([vp, i32, i32], None),
        "sq8l_gl_idle": ([vp, i32], None), "sq8l_gl_timer": ([vp], None), "sq8l_gl_pump": ([vp], i32),
        "sq8l_gl_post": ([vp, u32, u32, i32], None), "sq8l_gl_name_focus": ([vp, i32, cp], None),
        "sq8l_gl_name_key": ([vp, i32], None), "sq8l_gl_set_name_text": ([vp, cp], None),
        "sq8l_gl_menu_choice": ([vp, P(i32), i32], None), "sq8l_gl_msg_answer": ([vp, i32], None),
        "sq8l_gl_file_answer": ([vp, cp], None), "sq8l_gl_set_file": ([vp, cp, cp, i32], None),
        "sq8l_gl_get_file": ([vp, cp, cp, i32], i32), "sq8l_gl_modal_script": ([vp, cp], None),
        "sq8l_gl_events": ([vp, cp, i32], i32), "sq8l_gl_editbuffer": ([vp, cp], None),
        "sq8l_gl_library": ([vp, cp], None), "sq8l_gl_settings": ([vp, P(i32)], None),
        "sq8l_gl_state": ([vp, cp, i32], i32), "sq8l_gl_pages": ([vp, cp, i32], i32),
        "sq8l_gl_format": ([i32, i32, i32, cp, cp, i32], i32), "sq8l_gl_popup_text": ([i32, i32, cp, cp, i32], i32),
        "sq8l_gl_mod_usage": ([cp, cp, i32], i32), "sq8l_gl_clear_answers": ([vp], None),
        "sq8l_gl_set_midi": ([vp, cp, cp, i32], None), "sq8l_gl_sysex_received": ([vp, cp, i32], None),
        "sq8l_gl_ctr": ([vp, i32, i32, i32], None), "sq8l_gl_host_program": ([vp, i32], None),
    }
    for name, (args, res) in sig.items():
        fn = getattr(L, name)
        fn.argtypes = args
        fn.restype = res
    return L


API = None


def api():
    global API
    if API is None:
        API = _api()
    return API


def cstr(fn, *args, size=1 << 20):
    buf = ctypes.create_string_buffer(size)
    n = fn(*args, buf, size)
    assert n < size, "buffer too small"
    return buf.raw[:n].decode("latin1")


def f32bits(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


# ===================================================================== the original
class OracleLogic:
    """The original editor in the emulator, with scripted platform answers and state readers."""

    def __init__(self, program=None):
        self.answers_msg = []
        self.answers_file = []
        self.modal_scripts = []
        self.events = []
        self.modal_queue = []     # messages of the current modal dialog action
        self.modal_state = {}     # dialog hwnd -> dict(obj, script, closes)
        self.lb, self.memo, self.chk = {}, {}, {}   # emulated LISTBOX / multi-line EDIT / check box
        self._wincls = {}         # hwnd -> (object, Delphi class name, wndproc)
        self._patch_apis()
        self.ed = Editor(program=program)
        # the first paint of the window (the LEDs fit their size to the GIF frame when painted,
        # which matters for the mouse hit test); the port's test API renders once after show
        self.ed.frame()
        self.e = self.ed.emu
        self.g = self.ed.g
        self.form = self.e.u32(self.g.windows[self.ed.form].wndproc + 9)
        self.objs = {name: self.e.u32(self.form + off) for name, off in FORM_FIELDS.items()}
        self.master = self.e.u32(self.form + 0x4D4)
        self.eb = self.e.u32(self.form + 0x4D8)
        self.lib = self.e.u32(LIB_GLOBAL)
        self.cfg = self.e.u32(self.master + 0xFE0)
        self.knob_objs = [self.e.u32(self.e.u32(self.form + 0x4E8) + 4 * i) for i in range(10)]
        self.npopups = len(self.g.popups)
        self.hwnd_of = {}
        for h, w in self.g.windows.items():
            try:
                self.hwnd_of[self.e.u32(w.wndproc + 9)] = h
            except Exception:
                pass

    # ------------------------------------------------------------ API patches (test only)
    def _patch_apis(self):
        me = self

        def message_box(e, hwnd, text, caption, typ):
            r = 6 if typ == 4 else 1
            if me.answers_msg:
                r = me.answers_msg.pop(0)
            me.events.append(dict(ev="msgbox", text=e.cstr(text), caption=e.cstr(caption), flags=typ, result=r))
            return r
        winapi._REGISTRY["MessageBoxA"] = (4, message_box)

        def set_cursor(e, x, y):
            g = oracle_gui.gui(e)
            g.cursor = (oracle_gui.s32(x), oracle_gui.s32(y))
            me.events.append(dict(ev="setcursor", x=g.cursor[0], y=g.cursor[1]))
            return 1
        winapi._REGISTRY["SetCursorPos"] = (2, set_cursor)

        me.menu_queue = []

        def track_popup(e, h, flags, x, y, res, hwnd, rect):
            # like oracle/gui.py TrackPopupMenu, the choice taken from a queue of item paths
            g = oracle_gui.gui(e)
            tree = oracle_gui.menu_tree(e, h)
            g.popups.append((tree, oracle_gui.s32(x), oracle_gui.s32(y)))
            path = me.menu_queue.pop(0) if me.menu_queue else []
            cmd = path_to_id(tree, path) if path else 0
            if not cmd:
                return 0
            if flags & 0x100:
                return cmd
            g.posted.append((hwnd, 0x111, cmd & 0xFFFF, 0))
            return 1
        winapi._REGISTRY["TrackPopupMenu"] = (7, track_popup)

        # ---- modal dialogs: TCustomForm.ShowModal loops on Application.HandleMessage
        # (PeekMessage / DispatchMessage). While one of the editor's dialogs is open, PeekMessage
        # delivers the posted messages and then the messages of the scripted user actions;
        # without script the dialog is closed (WM_CLOSE -> mrCancel).
        def peek_message(e, pmsg, hwnd, mn, mx, remove):
            m = me._modal_next(e, bool(remove & 1))
            if m is None:
                return 0
            e.write(pmsg, struct.pack("<7I", *(v & 0xFFFFFFFF for v in (m[0], m[1], m[2], m[3], 0, 0, 0))))
            return 1
        winapi._REGISTRY["PeekMessageA"] = (5, peek_message)

        def dispatch_message(e, pmsg):
            h, msg, wp, lp = struct.unpack("<4I", e.read(pmsg, 16))
            w = oracle_gui.gui(e).win(h)
            if not w:
                return 0
            return e.chain_calls([(w.wndproc, [h, msg, wp, lp])], lambda r: r[0], 1)
        winapi._REGISTRY["DispatchMessageA"] = (1, dispatch_message)

        # ---- the system controls of the dialogs (the oracle's system window procedure only
        # keeps the window text): list box items / selection, multi-line edit, check box state
        if not hasattr(oracle_gui.GuiState, "_gl_orig_sys"):
            oracle_gui.GuiState._gl_orig_sys = oracle_gui.GuiState._sys_wndproc

        def sys_wndproc(gs, emu, hwnd, msg, wp, lp):
            w = gs.win(hwnd)
            cls = w.cls if w else ""
            if cls == "tlistbox" and 0x180 <= msg <= 0x1B0:
                return me._listbox(emu, hwnd, msg, wp, lp)
            if cls == "tmemo" and (0xB0 <= msg <= 0xDA or msg == 0xC):
                r = me._memo(emu, w, msg, wp, lp)
                if r is not None:
                    return r
            if cls == "tcheckbox" and msg in (0xF0, 0xF1):
                if msg == 0xF1:
                    me.chk[hwnd] = wp
                    return 0
                return me.chk.get(hwnd, 0)
            return oracle_gui.GuiState._gl_orig_sys(gs, emu, hwnd, msg, wp, lp)
        oracle_gui.GuiState._sys_wndproc = sys_wndproc

        # ---- TOpenDialog / TSaveDialog: scripted answers (file paths in the sandbox)
        def file_dialog(save):
            def fn(e, p):
                f = struct.unpack("<16I", e.read(p, 64))
                parts, a = [], f[3]
                while a:
                    t = e.cstr(a)
                    if not t:
                        break
                    parts.append(t)
                    a += len(t) + 1
                ev = dict(ev="filedialog", save=save, dir=e.cstr(f[11]) or "", filter="|".join(parts),
                          ext=e.cstr(f[15]) or "", name=e.cstr(f[7]) or "")
                if f[12] and e.cstr(f[12]):
                    ev["title"] = e.cstr(f[12])
                ans = me.answers_file.pop(0) if me.answers_file else ""
                ev["result"] = ans
                me.events.append(ev)
                if not ans:
                    return 0
                e.write_cstr(f[7], ans, f[8])
                return 1
            return fn
        winapi._REGISTRY["GetOpenFileNameA"] = (1, file_dialog(False))
        winapi._REGISTRY["GetSaveFileNameA"] = (1, file_dialog(True))

        # ---- winmm MIDI ports: names, open (succeeds if midi_ok), SysEx out recorded as events,
        # input buffers kept for midi_receive()
        me.midi_in_names, me.midi_out_names, me.midi_ok = [], [], False
        me.midi_in = None          # (handle, callback, instance, flags) of the open input
        me.midi_in_buffers = []

        def caps(names, size):
            def fn(e, dev, p, cb):
                if dev >= len(names):
                    return 2                       # MMSYSERR_BADDEVICEID
                e.write(p, b"\0" * min(cb, size))
                e.write(p + 8, names[dev].encode("latin1")[:31] + b"\0")
                return 0
            return fn
        winapi._REGISTRY["midiInGetNumDevs"] = (0, lambda e: len(me.midi_in_names))
        winapi._REGISTRY["midiOutGetNumDevs"] = (0, lambda e: len(me.midi_out_names))
        winapi._REGISTRY["midiInGetDevCapsA"] = (3, caps(me.midi_in_names, 44))
        winapi._REGISTRY["midiOutGetDevCapsA"] = (3, caps(me.midi_out_names, 52))

        def out_open(e, ph, dev, cb, inst, flags):
            if not me.midi_ok or dev >= len(me.midi_out_names):
                return 4                           # MMSYSERR_ALLOCATED
            e.w32(ph, 0x7000 + dev)
            return 0
        winapi._REGISTRY["midiOutOpen"] = (5, out_open)

        def out_long(e, h, hdr, cb):
            p, n = struct.unpack("<II", e.read(hdr, 8))
            me.events.append(dict(ev="midi", data=e.read(p, n).hex()))
            e.w32(hdr + 16, e.u32(hdr + 16) | 1)   # MHDR_DONE
            return 0
        winapi._REGISTRY["midiOutLongMsg"] = (3, out_long)
        winapi._REGISTRY["midiOutPrepareHeader"] = (3, lambda e, h, hdr, cb: 0)
        winapi._REGISTRY["midiOutReset"] = (1, lambda e, h: 0)
        winapi._REGISTRY["midiOutClose"] = (1, lambda e, h: 0)

        def in_open(e, ph, dev, cb, inst, flags):
            if not me.midi_ok or dev >= len(me.midi_in_names):
                return 4
            e.w32(ph, 0x7100 + dev)
            me.midi_in = (0x7100 + dev, cb, inst, flags)
            me.midi_in_buffers = []
            return 0
        winapi._REGISTRY["midiInOpen"] = (5, in_open)

        def in_add(e, h, hdr, cb):
            me.midi_in_buffers.append(hdr)
            return 0
        winapi._REGISTRY["midiInAddBuffer"] = (3, in_add)
        winapi._REGISTRY["midiInPrepareHeader"] = (3, lambda e, h, hdr, cb: 0)
        winapi._REGISTRY["midiInStart"] = (1, lambda e, h: 0)
        winapi._REGISTRY["midiInStop"] = (1, lambda e, h: 0)

        def in_close(e, h):
            me.midi_in = None
            return 0
        winapi._REGISTRY["midiInClose"] = (1, in_close)

    def set_midi(self, ins, outs, ok):
        self.midi_in_names[:] = ins
        self.midi_out_names[:] = outs
        self.midi_ok = ok

    def midi_receive(self, data):
        """A SysEx message arrives on the open MIDI input (MIM_LONGDATA); False if none is open."""
        if not self.midi_in or not self.midi_in_buffers:
            return False
        h, cb, inst, flags = self.midi_in
        hdr = self.midi_in_buffers.pop(0)
        p, n = struct.unpack("<II", self.e.read(hdr, 8))
        d = bytes(data[:n])
        self.e.write(p, d)
        self.e.w32(hdr + 8, len(d))
        self.e.w32(hdr + 16, self.e.u32(hdr + 16) | 1)
        kind = flags & 0x70000
        if kind == 0x30000:                    # CALLBACK_FUNCTION
            self.e.call(cb, h, 0x3C4, inst, hdr, 0)
        elif kind == 0x10000:                  # CALLBACK_WINDOW: MM_MIM_LONGDATA
            self.g.posted.append((cb, 0x3C4, h, hdr))
        else:
            raise RuntimeError(f"MIDI in callback kind {flags:#x}")
        return True

    # ------------------------------------------------------------ emulated dialog controls
    def _listbox(self, e, hwnd, msg, wp, lp):
        lb = self.lb.setdefault(hwnd, dict(items=[], sel=-1, data={}, top=0))
        items, i = lb["items"], oracle_gui.s32(wp)
        if msg == 0x180:                       # LB_ADDSTRING
            items.append(e.cstr(lp) or "")
            return len(items) - 1
        if msg == 0x181:                       # LB_INSERTSTRING
            i = len(items) if i < 0 else i
            if i > len(items):
                return -1
            items.insert(i, e.cstr(lp) or "")
            return i
        if msg == 0x182:                       # LB_DELETESTRING
            if 0 <= i < len(items):
                items.pop(i)
                return len(items)
            return -1
        if msg == 0x184:                       # LB_RESETCONTENT
            items.clear()
            lb["sel"] = -1
            return 0
        if msg == 0x186:                       # LB_SETCURSEL
            lb["sel"] = i if 0 <= i < len(items) else -1
            return lb["sel"]
        if msg in (0x188, 0x19F):              # LB_GETCURSEL / LB_GETCARETINDEX
            return lb["sel"]
        if msg == 0x187:                       # LB_GETSEL
            return int(i == lb["sel"] and i >= 0)
        if msg == 0x189:                       # LB_GETTEXT
            if 0 <= i < len(items):
                return e.write_cstr(lp, items[i])
            return -1
        if msg == 0x18A:                       # LB_GETTEXTLEN
            return len(items[i]) if 0 <= i < len(items) else -1
        if msg == 0x18B:                       # LB_GETCOUNT
            return len(items)
        if msg == 0x18E:                       # LB_GETTOPINDEX
            return lb["top"]
        if msg == 0x197:                       # LB_SETTOPINDEX
            lb["top"] = max(i, 0)
            return 0
        if msg == 0x199:                       # LB_GETITEMDATA
            return lb["data"].get(i, 0)
        if msg == 0x19A:                       # LB_SETITEMDATA
            lb["data"][i] = lp
            return 0
        if msg == 0x190:                       # LB_GETSELCOUNT (single selection)
            return -1
        if msg == 0x1A1:                       # LB_GETITEMHEIGHT
            return 13
        return 0

    def _memo(self, e, w, msg, wp, lp):
        st = self.memo.setdefault(w.hwnd, dict(sel=(0, 0)))
        if msg == 0xC:                         # WM_SETTEXT: selection back to 0
            st["sel"] = (0, 0)
            return None
        t = w.text
        starts = [0]
        k = t.find("\r\n")
        while k >= 0:
            starts.append(k + 2)
            k = t.find("\r\n", k + 2)

        def line_of(c):
            return max(j for j, s0 in enumerate(starts) if s0 <= c)

        def line_end(j):
            return starts[j + 1] - 2 if j + 1 < len(starts) else len(t)
        i = oracle_gui.s32(wp)
        if msg == 0xBA:                        # EM_GETLINECOUNT
            return len(starts)
        if msg == 0xBB:                        # EM_LINEINDEX
            j = line_of(st["sel"][0]) if i < 0 else i
            return starts[j] if j < len(starts) else -1
        if msg == 0xC1:                        # EM_LINELENGTH
            if i < 0:
                return 0
            j = line_of(min(i, len(t)))
            return line_end(j) - starts[j]
        if msg == 0xB1:                        # EM_SETSEL
            a, b = i, oracle_gui.s32(lp)
            if a < 0:
                a = b = st["sel"][1]
            if b < 0 or b > len(t):
                b = len(t)
            a = min(a, len(t))
            st["sel"] = (min(a, b), max(a, b))
            return 1
        if msg == 0xB0:                        # EM_GETSEL
            a, b = st["sel"]
            if wp:
                e.w32(wp, a)
            if lp:
                e.w32(lp, b)
            return (b << 16) | a
        if msg == 0xC2:                        # EM_REPLACESEL
            a, b = st["sel"]
            s0 = e.cstr(lp, maxlen=1 << 20) or ""
            w.text = t[:a] + s0 + t[b:]
            st["sel"] = (a + len(s0), a + len(s0))
            return 0
        if msg == 0xC4:                        # EM_GETLINE
            if not 0 <= i < len(starts):
                return 0
            n = e.u16(lp)
            data = t[starts[i]:line_end(i)][:n].encode("latin1")
            e.write(lp, data)
            return len(data)
        return 0

    # ------------------------------------------------------------ modal dialogs
    DIALOGS = {"TSelSingleForm": "select", "TmidiSelForm": "midi", "TModInfoForm": "modinfo",
               "TAboutForm": "about"}

    def _class_of(self, e, h, w):
        c = self._wincls.get(h)
        if c is None or c[2] != w.wndproc:
            try:
                obj = e.u32(w.wndproc + 9)
                vmt = e.u32(obj)
                p = e.u32(vmt - 0x2C)
                name = e.read(p + 1, e.read(p, 1)[0]).decode("latin1")
            except Exception:
                obj, name = 0, ""
            c = self._wincls[h] = (obj, name, w.wndproc)
        return c[0], c[1]

    def _hwnd_of_obj(self, e, obj):
        g = oracle_gui.gui(e)
        for h, w in g.windows.items():
            if self._class_of(e, h, w)[0] == obj:
                return h
        return 0

    def _active_dialog(self, e):
        g = oracle_gui.gui(e)
        best = None
        for h, w in g.windows.items():
            if w.style & 0x40000000:           # WS_CHILD
                continue
            obj, name = self._class_of(e, h, w)
            if name in self.DIALOGS and (best is None or h > best[0]):
                best = (h, obj, self.DIALOGS[name])
        return best

    def _dialog_event(self, e, kind, hwnd, obj):
        g = oracle_gui.gui(e)

        def text_of(off):
            h = self._hwnd_of_obj(e, e.u32(obj + off))
            return g.windows[h].text if h else None
        if kind in ("modinfo", "about"):
            t = text_of(0x2D0) or ""
            if kind == "about":
                return dict(ev="about", text=t)
            lines = t.split("\r\n")
            if lines and lines[-1] == "":
                lines.pop()
            return dict(ev="modinfo", lines=lines)
        lb = self.lb.get(self._hwnd_of_obj(e, e.u32(obj + 0x2D0)), dict(items=[], sel=-1))
        ev = dict(ev="modal", caption=g.windows[hwnd].text, ok=text_of(0x2D4), cancel=text_of(0x2D8),
                  index=lb["sel"], items=list(lb["items"]))
        if kind == "select":
            ev["compare"] = self.chk.get(self._hwnd_of_obj(e, e.u32(obj + 0x2DC)), 0) == 1
        return ev

    def _dialog_action(self, e, kind, hwnd, obj, a):
        g = oracle_gui.gui(e)

        def ctl(off):
            h = self._hwnd_of_obj(e, e.u32(obj + off))
            return h, (g.windows[h].id if h else 0)
        lst, lid = ctl(0x2D0)
        if kind in ("modinfo", "about"):
            if a == "click":
                return [(lst, 0x201, 1, 0), (lst, 0x202, 0, 0)]
            if a.startswith("key "):
                return [(lst, 0x100, int(a[4:]), 0)]
            return []
        buttons = {"ok": 0x2D4, "cancel": 0x2D8}
        if kind == "select":
            buttons.update(compare=0x2DC, bank=0x2E0)
        if a in buttons:
            h, i = ctl(buttons[a])
            return [(hwnd, 0x111, i & 0xFFFF, h)]          # WM_COMMAND BN_CLICKED
        if a.startswith("item ") or a.startswith("dbl "):
            n = int(a.split()[1])
            lb = self.lb.setdefault(lst, dict(items=[], sel=-1, data={}, top=0))
            if -1 <= n < len(lb["items"]):
                lb["sel"] = n
            out = [(hwnd, 0x111, (1 << 16) | (lid & 0xFFFF), lst)]   # LBN_SELCHANGE
            if a.startswith("dbl "):
                out.append((hwnd, 0x111, (2 << 16) | (lid & 0xFFFF), lst))  # LBN_DBLCLK
            return out
        if a.startswith("key "):
            return [(lst, 0x100, int(a[4:]), 0)]         # WM_KEYDOWN to the list
        return []

    def _modal_next(self, e, remove=True):
        for _ in range(10000):
            dlg = self._active_dialog(e)
            if dlg is None:
                # Application.ProcessMessages outside the dialogs: the posted messages
                self.modal_queue.clear()   # rest of the action that closed the dialog
                g = oracle_gui.gui(e)
                if g.posted:
                    return g.posted.pop(0) if remove else g.posted[0]
                return None
            if self.modal_queue:
                m = self.modal_queue[0]
                if remove:
                    self.modal_queue.pop(0)
                return m
            hwnd, obj, kind = dlg
            st = self.modal_state.get(hwnd)
            if st is None:
                if kind in ("select", "midi"):
                    script = list(self.modal_scripts.pop(0)) if self.modal_scripts else []
                else:
                    script = ["click"]
                st = self.modal_state[hwnd] = dict(obj=obj, script=script, closes=0)
                self.events.append(self._dialog_event(e, kind, hwnd, obj))
            g = oracle_gui.gui(e)
            if g.posted:
                self.modal_queue.append(g.posted.pop(0))
                continue
            if st["script"]:
                self.modal_queue.extend(self._dialog_action(e, kind, hwnd, obj, st["script"].pop(0)))
                continue
            st["closes"] += 1
            if st["closes"] > 20:
                raise RuntimeError(f"modal dialog {kind} does not close")
            self.modal_queue.append((hwnd, 0x10, 0, 0))   # WM_CLOSE -> ModalResult := mrCancel
        raise RuntimeError("modal message loop")

    # ------------------------------------------------------------ input
    def mouse(self, msg, x, y, wp=0):
        if msg == WM_MOUSEMOVE or self.g.capture not in self.g.windows:
            self.ed.mouse(msg, x, y, wp)
        else:
            self.ed._send(self.g.capture, msg, wp, x, y)

    def context_menu(self, x, y):
        # WM_CONTEXTMENU to the form (Windows' DefWindowProc passes it up to the form)
        self.g.cursor = (x, y)
        self.e.call(self.g.windows[self.ed.form].wndproc, self.ed.form, 0x7B, self.ed.form,
                    ((y & 0xFFFF) << 16) | (x & 0xFFFF))

    def idle(self, ms):
        self.ed.idle(ms)

    def name_focus(self, focused, text):
        h = self.hwnd_of[self.objs["progNameEdit"]]
        w = self.g.windows[h]
        w.text = text
        self.e.call(w.wndproc, h, 7 if focused else 8, 0, 0)

    def set_name_text(self, text):
        h = self.hwnd_of[self.objs["progNameEdit"]]
        self.g.windows[h].text = text

    def take_events(self):
        out = []
        for tree, x, y in self.g.popups[self.npopups:]:
            out.append(("popup", tree, x, y))
        self.npopups = len(self.g.popups)
        ev, self.events = self.events, []
        return out, ev

    # ------------------------------------------------------------ state
    def lstr(self, p):
        if not p:
            return ""
        return self.e.read(p, self.e.s32(p - 4)).decode("latin1")

    def lcd_cells(self, obj):
        e = self.e
        rows = e.u32(obj + 0x628)
        out = []
        for r in range(e.s32(rows - 4)):
            rp = e.u32(rows + 4 * r)
            n = e.s32(rp - 4)
            raw = e.read(rp, 6 * n)
            out.append([[raw[6 * i + 1], struct.unpack("<i", raw[6 * i + 2:6 * i + 6])[0]] for i in range(n)])
        return out

    def knob(self, o):
        e = self.e
        u, s, b = (lambda off: e.u32(o + off)), (lambda off: e.s32(o + off)), (lambda off: e.u8(o + off))
        return dict(value=u(0x22C), min=u(0x230), max=u(0x234), range=u(0x238), step=u(0x23C), pixFactor=u(0x244),
                    snapZone=u(0x24C), acc=u(0x204), angle=u(0x278), angleK=u(0x27C), maxPixDist=s(0x218),
                    maxFinePixDist=s(0x21C), downX=s(0x208), downY=s(0x20C), lastX=s(0x210), lastY=s(0x214),
                    dragging=b(0x200), dragActive=b(0x201), restoreMouse=b(0x228), intMode=b(0x240))

    def label(self, name):
        p = self.e.u32(self.objs[name] + 0x54)
        return self.e.cstr(p) if p else ""

    def state(self):
        e, f = self.e, self.form
        st = dict(lcd=self.lcd_cells(self.objs["lcd"]), numLcd=self.lcd_cells(self.objs["numLcd"]))
        st["knobs"] = [self.knob(o) for o in self.knob_objs]
        st["buttons"] = {n: [e.s32(self.objs[n] + 0x204), e.s32(self.objs[n] + 0x238), e.u8(self.objs[n] + 0x23C)]
                         for n in BUTTON_NAMES}
        st["leds"] = [e.u32(self.objs[n] + 0x20C) for n in ("ledSync", "ledAm", "ledMono")]
        st["status"] = self.label("StatusLabel1")
        st["voices"] = self.label("StatusLabel2")
        st["name"] = self.g.windows[self.hwnd_of[self.objs["progNameEdit"]]].text
        ctr = e.u32(f + 0x4DC)
        page = e.u32(ctr + 0x18)
        sub, params = 0, []
        if page:
            ml1 = e.u32(e.u32(page + 0xC))
            sub = e.s32(ml1 + 0x10)
            sl = e.u32(e.u32(e.u32(ml1 + 0x14) + 0xC))
            ps = e.u32(sl + 0x18)
            for i in range(e.s32(ps - 4)):
                p = e.u32(ps + 4 * i)
                params.append([e.u8(p + 0x78), e.s32(p + 0x34), self.lstr(e.u32(p + 0x1C))])
        st["ctr"] = dict(page=e.s32(ctr + 0x14), sub=sub, displayLock=e.s32(ctr + 0x24), updateLock=e.s32(ctr + 0x20),
                         msgActive=e.s32(ctr + 0x94), msgTimeout=e.s32(ctr + 0x90), refreshCount=e.s32(ctr + 0x10),
                         mouseDown=e.u8(ctr + 0x70), downCol=e.s32(ctr + 0x78), downRow=e.s32(ctr + 0x7C),
                         restoreMouse=e.u8(ctr + 0x8C), params=params)
        mj = e.u32(e.u32(MOUSEJUMP_PTR))
        st["mouseJump"] = [e.u8(mj + 4), e.s32(mj + 8), e.s32(mj + 0xC)]
        drag = e.u32(f + 0x508)
        st["form"] = dict(restMouseMenu=e.u8(f + 0x504), restMouseKnob=e.u8(f + 0x505), rmbScroll=e.u8(f + 0x511),
                          swap=e.u8(f + 0x510), keyCaptMode=e.s32(f + 0x50C),
                          lcdDragKnob=self.knob_objs.index(drag) if drag else -1, tickDivider=e.s32(f + 0x4E4),
                          midiIn=e.s32(f + 0x4F0), midiOut=e.s32(f + 0x4F4), sysexReq=e.s32(f + 0x4EC))
        st["pending"] = int(bool([m for m in self.g.posted if m[1] != 0x8002 or True]) and bool(self.g.posted))
        return st

    def editbuffer(self):
        img = bytearray(0xB8C)
        for off, n in EB_FIELDS:
            img[off:off + n] = self.e.read(self.eb + off, n)
        return bytes(img)

    def library(self):
        return self.e.read(self.lib + 0x20, 512 * PROG)

    def library_header(self):
        return self.e.read(self.lib + 0x43820, 0x3C), self.e.u8(self.lib + 0xC)

    def settings(self):
        return [self.e.s32(self.cfg + 0x20 + 4 * i) for i in range(6)] + \
               [self.e.s32(self.cfg + 0x38 + 4 * i) for i in range(5)]

    def plugin_dir(self):
        return self.lstr(self.e.u32(self.cfg + 0x1C))

    def max_voices(self):
        return self.e.s32(self.master + 0xF4C)


# ===================================================================== the port
class CppLogic:
    def __init__(self, oracle, first=True):
        self.L = api()
        self.v = self.L.sq8l_gl_new(1 if first else 0)
        o = oracle
        hdr, clean = o.library_header()
        self.L.sq8l_gl_set_library(self.v, o.library(), hdr, clean)
        self.L.sq8l_gl_set_editbuffer(self.v, o.editbuffer())
        s = o.settings()
        self.L.sq8l_gl_set_settings(self.v, (ctypes.c_int32 * 6)(*s[:6]), (ctypes.c_int32 * 5)(*s[6:]))
        self.L.sq8l_gl_set_host(self.v, o.max_voices(), 0, o.plugin_dir().encode("latin1"))

    def __del__(self):
        if getattr(self, "v", None):
            self.L.sq8l_gl_free(self.v)
            self.v = None

    def show(self):
        self.L.sq8l_gl_show(self.v)

    def mouse(self, msg, x, y, wp=0):
        self.L.sq8l_gl_mouse(self.v, msg, x, y, wp)

    def context_menu(self, x, y):
        self.L.sq8l_gl_context_menu(self.v, x, y)

    def idle(self, ms):
        self.L.sq8l_gl_idle(self.v, ms)

    def name_focus(self, focused, text):
        self.L.sq8l_gl_name_focus(self.v, 1 if focused else 0, text.encode("latin1"))

    def set_name_text(self, text):
        self.L.sq8l_gl_set_name_text(self.v, text.encode("latin1"))

    def menu_choice(self, path):
        self.L.sq8l_gl_menu_choice(self.v, (ctypes.c_int32 * max(len(path), 1))(*path), len(path))

    def take_events(self):
        txt = cstr(self.L.sq8l_gl_events, self.v)
        popups, other = [], []
        for line in txt.splitlines():
            if not line.startswith("{"):
                other.append(dict(ev="host", text=line))
                continue
            d = json.loads(line)
            if d["ev"] == "popup":
                popups.append(("popup", d["tree"], d["x"], d["y"]))
            else:
                other.append(d)
        return popups, other

    def state(self):
        return json.loads(cstr(self.L.sq8l_gl_state, self.v))

    def editbuffer(self):
        b = ctypes.create_string_buffer(0xB8C)
        self.L.sq8l_gl_editbuffer(self.v, b)
        img = bytearray(0xB8C)
        for off, n in EB_FIELDS:
            img[off:off + n] = b.raw[off:off + n]
        return bytes(img)

    def library(self):
        b = ctypes.create_string_buffer(512 * PROG)
        self.L.sq8l_gl_library(self.v, b)
        return b.raw

    def settings(self):
        out = (ctypes.c_int32 * 11)()
        self.L.sq8l_gl_settings(self.v, out)
        return list(out)

    def pages(self):
        return json.loads(cstr(self.L.sq8l_gl_pages, self.v))


# ===================================================================== comparison
def path_to_id(tree, path):
    """Command of the item at `path`; 0 (menu dismissed) for a path that does not exist, a
    separator, a disabled item or a sub-menu (items a user can't choose)."""
    level, it = tree, None
    for i in path:
        if not 0 <= i < len(level):
            return 0
        it = level[i]
        level = it.get("sub", [])
    if it is None or it["separator"] or it["disabled"] or it.get("sub"):
        return 0
    return it["id"]


def diff(a, b, path=""):
    """First difference between two JSON-like values, as a string (None if equal)."""
    if type(a) != type(b):
        return f"{path}: {repr(a)[:200]} != {repr(b)[:200]}"
    if isinstance(a, dict):
        for k in sorted(set(a) | set(b)):
            if k not in a or k not in b:
                return f"{path}.{k}: missing in {'original' if k not in a else 'port'}"
            d = diff(a[k], b[k], f"{path}.{k}")
            if d:
                return d
        return None
    if isinstance(a, list):
        if len(a) != len(b):
            return f"{path}: length {len(a)} != {len(b)}"
        for i, (x, y) in enumerate(zip(a, b)):
            d = diff(x, y, f"{path}[{i}]")
            if d:
                return d
        return None
    if a == b:
        return None
    ra, rb = repr(a), repr(b)
    return f"{path}: {ra[:200]} != {rb[:200]}"


class Pair:
    """The original and the port, driven in lockstep."""

    def __init__(self, program=None, first=True, verbose=False):
        self.o = OracleLogic(program)
        self.c = CppLogic(self.o, first)
        self.verbose = verbose
        self.checks = 0
        self.failures = []
        self.coverage = {}
        self.event_counts = {}
        # the oracle Editor() opened the editor (FormShow) and idled 300 ms
        self.c.show()
        self.c.idle(300)
        self.o.take_events()
        self.c.take_events()
        self.compare("open")

    # ------------------------------------------------------------ primitives (both sides)
    def mouse(self, msg, x, y, wp=0):
        self.o.mouse(msg, x, y, wp)
        self.c.mouse(msg, x, y, wp)

    def idle(self, ms):
        self.o.idle(ms)
        self.c.idle(ms)

    def choose(self, path):
        """Answer the next popup menu with the item at `path` (list of indices; [] = dismiss)."""
        self.o.menu_queue.append(list(path))
        self.c.menu_choice(list(path))

    def msg_answer(self, r):
        self.o.answers_msg.append(r)
        self.c.L.sq8l_gl_msg_answer(self.c.v, r)

    def clear_answers(self):
        """Drop scripted answers that were not used (both sides)."""
        self.o.menu_queue.clear()
        self.o.answers_msg.clear()
        self.o.answers_file.clear()
        self.o.modal_scripts.clear()
        self.c.L.sq8l_gl_clear_answers(self.c.v)

    def modal(self, *actions):
        """Script for the next WRITE / bank select / MIDI port dialog: "item N", "dbl N",
        "key K", "compare", "bank", "ok", "cancel" (none left: the dialog is closed)."""
        self.o.modal_scripts.append(list(actions))
        self.c.L.sq8l_gl_modal_script(self.c.v, ";".join(actions).encode("latin1"))

    def file_answer(self, path):
        """Answer of the next open/save dialog ("" = cancel)."""
        self.o.answers_file.append(path)
        self.c.L.sq8l_gl_file_answer(self.c.v, path.encode("latin1"))

    # ------------------------------------------------------------ coverage (pages x parameters x actions)
    def cover(self, page, sub, param, action):
        self.coverage.setdefault((page, sub, param), set()).add(action)

    def ctr_snapshot(self):
        c = self.c.state()["ctr"]
        return c["page"], c["sub"], [(h, v) for h, v, _ in c["params"]]

    def track(self, action, before):
        """Record the parameters whose value changed (or that got selected) by `action`."""
        page, sub, params = before
        page2, sub2, params2 = self.ctr_snapshot()
        if (page, sub) != (page2, sub2) or len(params) != len(params2):
            return
        for k, ((h, v), (h2, v2)) in enumerate(zip(params, params2)):
            if v != v2:
                self.cover(page, sub, k, action)
            elif h2 and not h:
                self.cover(page, sub, k, action + " (select)")

    # ------------------------------------------------------------ MIDI
    def set_midi(self, ins, outs, ok):
        self.o.set_midi(ins, outs, ok)
        self.c.L.sq8l_gl_set_midi(self.c.v, "\n".join(ins).encode("latin1"), "\n".join(outs).encode("latin1"),
                                  1 if ok else 0)

    def midi_receive(self, data):
        if self.o.midi_receive(data):
            self.c.L.sq8l_gl_sysex_received(self.c.v, bytes(data), len(data))
            return True
        return False

    # ------------------------------------------------------------ files (oracle sandbox / C++ map)
    def host_path(self, winpath):
        return self.o.e.api.host_path(winpath)

    def put_file(self, winpath, data):
        hp = self.host_path(winpath)
        os.makedirs(os.path.dirname(hp), exist_ok=True)
        with open(hp, "wb") as f:
            f.write(data)
        self.c.L.sq8l_gl_set_file(self.c.v, winpath.encode("latin1"), data, len(data))

    def get_files(self, winpath):
        hp = self.host_path(winpath)
        o = open(hp, "rb").read() if os.path.isfile(hp) else None
        n = self.c.L.sq8l_gl_get_file(self.c.v, winpath.encode("latin1"), None, 0)
        c = None
        if n >= 0:
            buf = ctypes.create_string_buffer(max(n, 1))
            self.c.L.sq8l_gl_get_file(self.c.v, winpath.encode("latin1"), buf, n)
            c = buf.raw[:n]
        return o, c

    def check_file(self, winpath, what):
        self.checks += 1
        o, c = self.get_files(winpath)
        if o != c:
            d = f"file {winpath}: original {None if o is None else len(o)} bytes, port {None if c is None else len(c)}"
            if o is not None and c is not None and len(o) == len(c):
                i = next(i for i in range(len(o)) if o[i] != c[i])
                d += f", first difference at {i:#x}"
            self.failures.append((what, d))
            print(f"  MISMATCH after {what}: {d}")
            return False
        return True

    # ------------------------------------------------------------ program name edit
    def name_focus(self, focused, text):
        self.o.name_focus(focused, text)
        self.c.name_focus(focused, text)

    def name_key(self, key):
        h = self.o.hwnd_of[self.o.objs["progNameEdit"]]
        self.o.e.call(self.o.g.windows[h].wndproc, h, 0x100, key, 0)
        self.c.L.sq8l_gl_name_key(self.c.v, key)

    # ------------------------------------------------------------ gestures
    def center(self, name):
        """Centre of a control at its run-time position (FormShow moves the knobs; the DFM
        position for graphic controls)."""
        h = self.o.hwnd_of.get(self.o.objs.get(name))
        if h in self.o.g.windows:
            w = self.o.g.windows[h]
            return w.x + w.w // 2, w.y + w.h // 2
        cls, l, t, w, h = COMPS[name]
        return l + w // 2, t + h // 2

    def click_at(self, x, y, double=False, right=False, keys=0, idle=100):
        self.mouse(WM_MOUSEMOVE, x, y, keys)
        if right:
            self.mouse(WM_RBUTTONDOWN, x, y, MK_RBUTTON | keys)
            self.mouse(WM_RBUTTONUP, x, y, keys)
        else:
            self.mouse(WM_LBUTTONDOWN, x, y, MK_LBUTTON | keys)
            self.mouse(WM_LBUTTONUP, x, y, keys)
            if double:
                self.mouse(WM_LBUTTONDBLCLK, x, y, MK_LBUTTON | keys)
                self.mouse(WM_LBUTTONUP, x, y, keys)
        if idle:
            self.idle(idle)

    def click(self, name, **kw):
        self.click_at(*self.center(name), **kw)

    def drag_at(self, x, y, dx, dy, steps=8, keys=0, idle_step=20):
        self.mouse(WM_MOUSEMOVE, x, y, 0)
        self.mouse(WM_LBUTTONDOWN, x, y, MK_LBUTTON | keys)
        for i in range(1, steps + 1):
            self.mouse(WM_MOUSEMOVE, x + dx * i // steps, y + dy * i // steps, MK_LBUTTON | keys)
            if idle_step:
                self.idle(idle_step)
        self.mouse(WM_LBUTTONUP, x + dx, y + dy, keys)
        self.idle(100)

    def context_menu(self, x, y, idle=100):
        self.mouse(WM_MOUSEMOVE, x, y, 0)
        self.o.context_menu(x, y)
        self.c.context_menu(x, y)
        if idle:
            self.idle(idle)

    def lcd_cell_xy(self, col, row, dx=5, dy=8):
        # main LCD at (34, 247): frame 4, cells 12 x 21
        return 34 + 4 + col * 12 + dx, 247 + 4 + row * 21 + dy

    # ------------------------------------------------------------ comparison
    def compare(self, what, library=False):
        self.checks += 1
        so, sc = self.o.state(), self.c.state()
        so.pop("pending", None)
        sc.pop("pending", None)
        d = diff(so, sc, "state")
        po, eo = self.o.take_events()
        pc, ec = self.c.take_events()
        # The popup trees are deliberately not compared with the original: the port regroups
        # OPTIONS and adds items the original has no counterpart for, and GUI parity is not a
        # goal of the port (see docs/modules/gui_logic.md). The menus are still *driven* here,
        # so every command they dispatch is still exercised and its effect on the state and
        # the edit buffer compared below; only the tree itself goes uncompared. po/pc are kept
        # for the popup counter.
        if d is None:
            kinds = ("msgbox", "setcursor", "modal", "modinfo", "about", "filedialog", "midi")
            for e in eo:
                self.event_counts[e["ev"]] = self.event_counts.get(e["ev"], 0) + 1
            self.event_counts["popup"] = self.event_counts.get("popup", 0) + len(po)
            d = diff([e for e in eo if e["ev"] in kinds], [e for e in ec if e["ev"] in kinds], "events")
        if d is None and self.o.editbuffer() != self.c.editbuffer():
            a, b = self.o.editbuffer(), self.c.editbuffer()
            i = next(i for i in range(len(a)) if a[i] != b[i])
            d = f"editbuffer differs at +{i:#x}: {a[i]:#x} != {b[i]:#x}"
        if d is None and self.o.settings() != self.c.settings():
            d = f"settings {self.o.settings()} != {self.c.settings()}"
        if d is None and library and self.o.library() != self.c.library():
            d = "library programs differ"
        if d is not None:
            self.failures.append((what, d))
            if len(self.failures) <= 10:
                print(f"  MISMATCH after {what}: {d}")
            return False
        return True


def oracle_pages(o):
    """The page objects of the original's ClcdCtr (FUN_0047e340), in the shape of sq8l_gl_pages."""
    sys.path.insert(0, os.path.join(ROOT, "re", "scripts"))
    import extract_gui_logic_data as X
    fmt_ix = {a: i for i, a in enumerate(X.FMT)}
    pop_ix = {a: i for i, a in enumerate(X.POP)}
    chg_ix = {a: i for i, a in enumerate(X.CHANGE)}
    e = o.e

    def dyn(p):
        return [e.u32(p + 4 * i) for i in range(e.s32(p - 4))] if p else []

    def param(p):
        s = lambda off: e.s32(p + off)  # noqa: E731
        return dict(knob=s(4), index=s(8), ordinal=s(0xC), name=o.lstr(e.u32(p + 0x10)),
                    hint=o.lstr(e.u32(p + 0x14)), label=o.lstr(e.u32(p + 0x18)), width=s(0x20), x=s(0x24),
                    y=s(0x28), min=s(0x2C), max=s(0x30), fmt=fmt_ix[e.u32(p + 0x38)], pop=pop_ix[e.u32(p + 0x40)],
                    customFmt=e.u8(p + 0x48), showNumber=e.u8(p + 0x64), perColumn=s(0x68), paramIndex=s(0x74),
                    change=chg_ix[e.u32(p + 0x58)])
    ctr = e.u32(o.form + 0x4DC)
    out = []
    for page in dyn(e.u32(ctr + 4)):
        ml1 = dyn(e.u32(page + 0xC))[0]
        subs = []
        for ml2 in dyn(e.u32(ml1 + 0xC)):
            sl = dyn(e.u32(ml2 + 0xC))[0]
            s = lambda off: e.s32(sl + off)  # noqa: E731
            subs.append(dict(title=o.lstr(e.u32(sl + 0x14)), c=s(0xC), group=s(0x10), base=s(0x24), offset=s(0x28),
                             indent=s(0x2C), hidden=s(0x30), knobs=[oracle_gui.s32(k) for k in dyn(e.u32(sl + 0x1C))],
                             params=[param(p) for p in dyn(e.u32(sl + 0x18))]))
        out.append(dict(cur=e.s32(ml1 + 0x10), subs=subs))
    return out


def check_pages(pair):
    """The page tree built by the port == the objects the original built (FUN_0047e340):
    every sub-page and parameter field (texts, layout, ranges, knobs, formatters, callbacks)."""
    pair.checks += 1
    a, b = oracle_pages(pair.o), pair.c.pages()
    for pg in a + b:                 # current sub-page: compared with the state
        pg.pop("cur", None)
    d = diff(a, b, "pages")
    if d:
        pair.failures.append(("page tree", d))
        print(f"  MISMATCH page tree: {d}")
    n = sum(len(s["params"]) for pg in a for s in pg["subs"])
    return len(a), sum(len(pg["subs"]) for pg in a), n


def scenario_values(pair):
    """Every value of every parameter of every sub-page, set through the page controller of both
    (FUN_0045b574 / LcdController::setParam), compared after each value; then the value popup
    of every parameter (double click on its VFD field)."""
    p = pair
    pages, subs, params = check_pages(p)
    print(f"  page tree: {pages} pages, {subs} sub-pages, {params} parameters")
    tree = p.c.pages()
    ctr = p.o.e.u32(p.o.form + 0x4DC)
    nvalues = 0
    for pi, pg in enumerate(tree):
        for si, sp in enumerate(pg["subs"]):
            p.o.e.call(0x45A9A0, ctr, pi, si, conv="register")
            p.c.L.sq8l_gl_ctr(p.c.v, 0, pi, si)
            p.compare(f"select page {pi}.{si}")
            for k, prm in enumerate(sp["params"]):
                orig = p.c.state()["ctr"]["params"][k][1]
                for v in list(range(prm["min"] - 1, prm["max"] + 2)) + [orig]:
                    p.o.e.call(0x45B574, ctr, k, v, conv="register")
                    p.c.L.sq8l_gl_ctr(p.c.v, 1, k, v)
                    nvalues += 1
                    so, sc = p.o.state(), p.c.state()
                    p.checks += 1
                    d = diff([so["lcd"], so["ctr"]["params"]], [sc["lcd"], sc["ctr"]["params"]], "lcd")
                    if d:
                        p.failures.append((f"page {pi}.{si} param {k} = {v}", d))
                        if len(p.failures) <= 10:
                            print(f"  MISMATCH page {pi}.{si} {prm['label']!r} = {v}: {d}")
                        break
                    p.cover(pi, si, k, "value")
                p.compare(f"page {pi}.{si} param {k} all values", library=False)
                # value popup (double click on the field), dismissed
                x, y = p.lcd_cell_xy(prm["x"] + 1, prm["y"])
                p.choose([])
                p.click_at(x, y, double=True, idle=40)
                p.compare(f"page {pi}.{si} param {k} popup")
                p.cover(pi, si, k, "popup")
    print(f"  values set: {nvalues}")


PAGE_BUTTONS = ("buttOsc1", "buttOsc2", "buttOsc3", "buttDca1", "buttDca2", "buttDca3", "buttFilt", "buttDca4",
                "buttModes", "buttLfo1", "buttLfo2", "buttLfo3", "buttLfo4", "buttEnv1", "buttEnv2", "buttEnv3",
                "buttEnv4", "buttMat1", "buttMat2", "buttMat3", "buttWav")


def scenario_basic(pair):
    """Every page button and sub-page: knob drags (vertical, horizontal fine mode), VFD clicks on
    every field, double clicks with a value chosen from the popup, VFD drags."""
    p = pair
    rnd = random.Random(1)
    tree = p.c.pages()
    for bi, button in enumerate(PAGE_BUTTONS):
        p.click(button)
        p.compare(f"click {button}")
        nsub = len(tree[p.c.state()["ctr"]["page"]]["subs"])
        for si in range(nsub):
            if si:
                before = p.ctr_snapshot()
                p.click("pscrDownButton" if si % 2 or bi % 2 else button)
                p.compare(f"{button} next sub-page")
            st = p.c.state()["ctr"]
            page, sub = st["page"], st["sub"]
            what = f"{button} {page}.{sub}"
            for k in range(10):
                x, y = p.center(f"lcdKnob{k}")
                before = p.ctr_snapshot()
                p.drag_at(x, y, 0, -rnd.randint(5, 60), steps=rnd.randint(2, 8))
                p.track("knob drag", before)
                p.compare(f"{what} drag knob {k} up")
                before = p.ctr_snapshot()
                p.drag_at(x, y, rnd.randint(20, 60), rnd.randint(5, 40), steps=6)
                p.track("knob fine drag", before)
                p.compare(f"{what} drag knob {k} fine down")
            for prm in tree[page]["subs"][sub]["params"]:
                if prm["x"] >= 44:          # empty filler at the end of a line (outside the VFD)
                    continue
                x, y = p.lcd_cell_xy(prm["x"] + rnd.randint(0, max(len(prm["label"]) - 1, 0)), prm["y"])
                before = p.ctr_snapshot()
                p.click_at(x, y)
                p.track("lcd click", before)
                p.compare(f"{what} lcd click {prm['label']!r}")
                p.choose([rnd.randrange(min(prm["max"] - prm["min"] + 1, 80))])
                before = p.ctr_snapshot()
                p.click_at(x, y, double=True)
                p.track("popup choice", before)
                p.compare(f"{what} lcd dblclick {prm['label']!r}")
                for sign in rnd.sample((-1, 1), 2):
                    before = p.ctr_snapshot()
                    p.drag_at(x, y, rnd.randint(-20, 20), sign * rnd.randint(8, 60), steps=5)
                    p.track("lcd drag", before)
                    p.compare(f"{what} lcd drag {prm['label']!r} {sign}")
        # the cells between the fields
        for col in range(0, 44, 7):
            for row in (0, 1):
                p.click_at(*p.lcd_cell_xy(col, row))
                p.compare(f"{button} lcd click {col},{row}")


def scenario_programs_menus(pair):
    """Program selection (arrows, bank, popups), page popup, options/info menus, buttons, hints."""
    p = pair
    for name in ("upButton", "upButton", "downButton", "BankButton", "upButton", "BankButton", "BankButton",
                 "downButton", "BankButton", "downButton", "upButton"):
        p.click(name)
        p.compare(f"click {name}")
    # program popup: right click on the program number / double click on the name / numLcd
    x, y = p.center("numLcd") if COMPS["numLcd"][3] else (40, 50)
    for path in ([5], [40], [], [0], [66], [99], [130], [2]):
        p.choose(path)
        if path in ([0], [66]):          # bank items reopen the menu: dismiss the second one
            p.choose([3])
        p.click_at(40, 52, right=True)
        p.compare(f"numLcd program popup {path}")
    for path in ([7], [1]):
        p.choose(path)
        p.click("progNameEdit", double=True)
        p.compare(f"name dblclick popup {path}")
        p.choose(path)
        p.click("progNameEdit", right=True)
        p.compare(f"name right click popup {path}")
    # page popup (WM_CONTEXTMENU) over knobs, LCD (scrolls when enabled), background
    for i, (x, y) in enumerate(((140, 214), (300, 260), (60, 330), (500, 280), (600, 100), (100, 12), (170, 12),
                                (220, 12), (30, 12))):
        for path in ([2 + 3 * i], [], [3]):
            p.choose(path)
            p.context_menu(x, y)
            p.compare(f"context menu at {x},{y} {path}")
    # OPTIONS menu: every item (twice for the toggles)
    opt = [[0, 0], [0, 2], [0, 3], [1, 0, 0], [1, 0, 2], [1, 0, 3], [1, 1, 2], [1, 1, 3], [1, 1, 0], [1, 2, 2],
           [1, 2, 3], [1, 2, 0], [1, 3, 2], [1, 3, 3], [1, 3, 4], [1, 3, 0], [3, 0], [3, 1], [4], [4], [3, 1], [3, 0]]
    for path in opt + [[3, 0], [4]]:
        p.choose(path)
        p.click("menuOptImage")
        p.compare(f"options menu {path}")
        # menus reflect the new state
        p.choose([])
        p.click("menuOptImage")
        p.compare(f"options menu (show) after {path}")
        if path in ([4],):
            p.choose([])
            p.context_menu(300, 260)
            p.compare("context menu over LCD with rmb scroll toggled")
    for path in ([], [0]):
        p.choose(path)
        p.click("menuFileImage")
        p.compare(f"file menu {path}")
    p.choose([])
    p.click("menuInfoImage")
    p.compare("info menu")
    # buttons
    for name in ("buttSync", "buttAm", "buttMono", "buttSync", "buttAm", "buttMono", "buttAm", "initButton",
                 "menuPanicImage", "pscrDownButton", "pscrUpButton", "pscrUpButton"):
        p.click(name)
        p.compare(f"click {name}")
    p.click("buttWav")
    for name in ("pscrDownButton", "pscrDownButton", "pscrDownButton", "pscrUpButton"):
        p.click(name)
        p.compare(f"click {name} on scrollable page")
    # hints: hover every control (and back to the form)
    for name in list(COMPS):
        cls, l, t, w, h = COMPS[name]
        if None in (l, t, w, h) or w <= 0 or h <= 0:
            continue
        p.mouse(WM_MOUSEMOVE, *p.center(name))
        p.compare(f"hover {name}")
        p.mouse(WM_MOUSEMOVE, 5, 300)
        p.compare(f"hover form after {name}")
    for row in (0, 1):
        for col in range(0, 44):
            p.mouse(WM_MOUSEMOVE, *p.lcd_cell_xy(col, row))
            p.compare(f"hover lcd {col},{row}")
    # timer: messages expire, voices
    p.idle(2000)
    p.compare("idle 2 s")


FILE_ITEMS = {"Load library": 0, "Save library": 1, "Init library": 2, "Load bank": 4, "Save bank": 5,
              "Init bank": 6, "Request program": 8, "Receive program": 9, "Send program": 10,
              "Import program file": 11, "Export program file": 12, "Import bank file": 14, "Export bank file": 15}


def file_menu(p, name):
    idx = FILE_ITEMS[name]
    p.choose([idx])
    p.click("menuFileImage")
    tree = p.o.g.popups[-1][0]
    assert tree[idx]["text"].replace("&", "").startswith(name), tree[idx]["text"]


def step(p, what, library=False, idle=0):
    """compare, drop unused answers, optionally let the messages time out and compare again"""
    p.compare(what, library=library)
    p.clear_answers()
    if idle:
        p.idle(idle)
        p.compare(f"{what} (+{idle} ms)", library=library)


def scenario_dialogs(pair):
    """WRITE / compare dialog, mod usage, about, program name editing."""
    p = pair
    scripts = [["cancel"], [], ["item 5", "ok"], ["bank", "item 3", "ok"],
               ["compare", "item 7", "item 9", "compare", "ok"], ["compare", "item 2", "cancel"], ["dbl 12"],
               ["item 4", "key 13"], ["item 6", "key 27"], ["key 65", "item 1", "ok"], ["bank", "bank", "item 0", "ok"],
               ["bank", "bank", "bank", "item 127", "ok"], ["compare", "bank", "item 64", "ok"], ["compare"],
               ["item 0", "compare", "dbl 3"], ["compare", "item 100", "bank", "bank", "ok"], ["item 31", "ok"]]
    for i, s in enumerate(scripts):
        if i % 3 == 2:
            p.click("BankButton")
        p.modal(*s)
        p.click("writeButton")
        step(p, f"WRITE {s}", library=True, idle=1200 if i % 4 == 0 else 0)
    # modulation usage of many programs, about
    for i in range(40):
        p.click("upButton" if i % 9 else "BankButton")
        p.choose([0])
        p.click("menuInfoImage")
        step(p, f"mod usage #{i}")
    p.choose([2])
    p.click("menuInfoImage")
    step(p, "about")
    # program name edit (focus, typing, kill focus, Enter)
    name = p.o.state()["name"]
    p.name_focus(True, name)
    step(p, "name focus")
    for text in ("NEWNAM", "ab", "", "TOOLONGNAME", "x y z", "\u00e9\u00e0#|?"):
        p.o.set_name_text(text)
        p.c.set_name_text(text)
        p.name_focus(False, text)
        step(p, f"name edit {text!r}")
        p.name_focus(True, text)
    p.name_key(65)
    p.name_key(13)
    p.name_focus(False, "ENTER")
    step(p, "name Enter", idle=500)
    # WRITE writes the edited name
    p.name_focus(True, "WRNAME")
    p.o.set_name_text("WRNAME")
    p.c.set_name_text("WRNAME")
    p.modal("item 9", "ok")
    p.click("writeButton")
    step(p, "WRITE with edited name", library=True)


def scenario_files(pair):
    """FILE menu: library / bank load-save-init, SysEx program / bank import-export, MIDI ports."""
    import shutil
    p = pair
    T = "T:\\gl\\"
    shutil.rmtree(p.host_path("T:\\gl"), ignore_errors=True)
    # library: save (cancel / ok), modify, load back (cancel / ok), junk, empty, missing files
    for ans in ("", T + "lib1.8XL"):
        p.file_answer(ans)
        file_menu(p, "Save library")
        step(p, f"save library {ans!r}")
    p.check_file(T + "lib1.8XL", "save library")
    p.modal("item 10", "ok")
    p.click("writeButton")
    step(p, "WRITE before load", library=True)
    for r in (2, 1):
        p.file_answer(T + "lib1.8XL")
        p.msg_answer(r)
        file_menu(p, "Load library")
        step(p, f"load library answer {r}", library=True)
    p.put_file(T + "junk.8XL", bytes(range(256)) * 10)
    p.put_file(T + "empty.8XL", b"")
    for f in ("junk.8XL", "empty.8XL", "missing.8XL"):
        p.file_answer(T + f)
        p.msg_answer(1)
        file_menu(p, "Load library")
        step(p, f"load library {f}", library=True)
    for r in (2, 1):
        p.msg_answer(r)
        file_menu(p, "Init library")
        step(p, f"init library answer {r}", library=True, idle=600)
    p.file_answer(T + "lib1.8XL")
    p.msg_answer(1)
    file_menu(p, "Load library")
    step(p, "load library back", library=True)
    # banks A..D: save, init (cancel / ok), load back (cancel / ok), junk
    for b in range(4):
        f = T + f"bank{b}.8XL"
        p.file_answer(f)
        file_menu(p, "Save bank")
        step(p, f"save bank {b}")
        p.check_file(f, f"save bank {b}")
        for r in (2, 1):
            p.msg_answer(r)
            file_menu(p, "Init bank")
            step(p, f"init bank {b} answer {r}", library=True)
        for r in (2, 1):
            p.file_answer(f)
            p.msg_answer(r)
            file_menu(p, "Load bank")
            step(p, f"load bank {b} answer {r}", library=True)
        p.file_answer(T + "junk.8XL")
        p.msg_answer(1)
        file_menu(p, "Load bank")
        step(p, f"load junk bank {b}", library=True)
        p.click("BankButton")
        step(p, "bank button")
    # SysEx program export / import
    for ans in ("", T + "prog1.SYX"):
        p.file_answer(ans)
        file_menu(p, "Export program file")
        step(p, f"export program {ans!r}")
    p.check_file(T + "prog1.SYX", "export program")
    p.click("initButton")
    step(p, "init program")
    for f, answers in (("prog1.SYX", [2]), ("prog1.SYX", [1]), ("junk.8XL", [1, 7]), ("junk.8XL", [1, 6]),
                       ("empty.8XL", [1])):
        p.file_answer(T + f)
        for r in answers:
            p.msg_answer(r)
        file_menu(p, "Import program file")
        step(p, f"import program {f} {answers}", idle=400)
    # SysEx bank export / import (select dialog)
    for script in (["cancel"], ["item 100", "ok"], ["item 10", "ok"], ["bank", "item 0", "ok"]):
        p.modal(*script)
        p.file_answer(T + "bank1.SYX")
        file_menu(p, "Export bank file")
        step(p, f"export bank {script}")
    p.check_file(T + "bank1.SYX", "export bank")
    for script, answers, f in ((["cancel"], [1], "bank1.SYX"), (["item 20", "ok"], [2], "bank1.SYX"),
                               (["item 20", "ok"], [1], "bank1.SYX"), (["item 120", "ok"], [1], "bank1.SYX"),
                               (["bank", "bank", "item 5", "ok"], [1], "bank1.SYX"),
                               (["item 3", "ok"], [1, 7], "junk.8XL"), (["item 3", "ok"], [1, 6], "junk.8XL"),
                               ([], [1], "missing.SYX")):
        p.modal(*script)
        p.file_answer(T + f)
        for r in answers:
            p.msg_answer(r)
        file_menu(p, "Import bank file")
        step(p, f"import bank {f} {script} {answers}", library=True, idle=300)
    # MIDI (no ports): request / receive / send, SEND / REQ buttons
    for name in ("Request program", "Receive program", "Send program"):
        for script in (["ok"], ["cancel"], []):
            p.modal(*script)
            p.modal(*script)
            file_menu(p, name)
            step(p, f"{name} {script}")
    for name in ("sysExSendButton", "sysExReqButton"):
        for script in (["ok"], ["key 13"], ["key 27"]):
            p.modal(*script)
            p.modal(*script)
            p.click(name)
            step(p, f"{name} {script}")
    shutil.rmtree(p.host_path("T:\\gl"), ignore_errors=True)


def scenario_midi(pair):
    """SEND / REQ with MIDI ports: port dialogs, SysEx dump out, dump received on the input."""
    p = pair
    p.set_midi(["In A", "In B"], ["Out A", "Out B", "Out C"], False)   # ports that can't be opened
    for name in ("Request program", "Receive program", "Send program"):
        for script in (["item 1", "ok"], ["cancel"], ["dbl 0"], ["key 13"], ["item 1", "key 27"]):
            p.modal(*script)
            p.modal("item 2", "ok")
            file_menu(p, name)
            step(p, f"{name} {script} (open fails)")
    p.set_midi(["In A", "In B"], ["Out A", "Out B", "Out C"], True)
    p.modal("item 2", "ok")
    file_menu(p, "Send program")
    dumps = [bytes.fromhex(e["data"]) for e in p.o.events if e["ev"] == "midi"]
    step(p, "send program", idle=1500)
    assert len(dumps) == 3, dumps
    dump = dumps[1]
    p.click("sysExSendButton")
    step(p, "SEND button", idle=300)
    p.click("initButton")
    step(p, "init program")
    p.modal("item 1", "ok")
    p.modal("item 0", "ok")
    file_menu(p, "Request program")
    step(p, "request program", idle=200)
    p.midi_receive(dump)
    step(p, "dump received", idle=300)
    p.midi_receive(dump)
    step(p, "dump received again")
    p.click("downButton")
    step(p, "down")
    p.click("sysExReqButton")
    step(p, "REQ button")
    p.midi_receive(dump[:40])
    step(p, "short dump received", idle=300)
    for data in (dump, bytes([0xF0, 0x7E, 0x00, 0x06, 0x01, 0xF7]), dump[:-1] + b"\x00"):
        p.modal("item 0", "ok")
        file_menu(p, "Receive program")
        step(p, "receive program")
        p.midi_receive(data)
        step(p, f"received {len(data)} bytes", idle=300)
    p.set_midi([], [], False)


def scenario_host(pair):
    """The host changes the program while the editor is open (effSetProgram)."""
    p = pair
    from vsthost import effSetProgram
    rnd = random.Random(5)
    for i in range(30):
        n = rnd.randrange(512) if i % 3 else i * 17 % 512
        p.o.ed.host.dispatch(effSetProgram, value=n)
        p.c.L.sq8l_gl_host_program(p.c.v, n)
        p.compare(f"host program {n}")
        p.idle(rnd.choice((20, 100, 400)))
        p.compare(f"host program {n} idle")
        if i % 5 == 4:
            p.click(rnd.choice(PAGE_BUTTONS))
            p.compare("page after host program change")


OPTION_PATHS = [[0, 0], [0, 2], [0, 3], [1, 0, 0], [1, 0, 2], [1, 0, 3], [1, 1, 0], [1, 1, 2], [1, 1, 3],
                [1, 2, 0], [1, 2, 2], [1, 2, 3], [1, 3, 0], [1, 3, 2], [1, 3, 3], [1, 3, 4], [3, 0], [3, 1], [4]]


def random_action(p, rnd):
    """One random user action on both sides; returns its description."""
    a = rnd.choices(["page", "scroll", "knob", "knobdbl", "lcdclick", "lcddbl", "lcddrag", "lcdright", "context",
                     "updown", "progpopup", "namepopup", "buttons", "options", "write", "info", "hover", "idle",
                     "name"],
                    weights=[8, 4, 14, 2, 8, 8, 6, 3, 4, 4, 3, 2, 3, 3, 3, 1, 4, 3, 2])[0]
    if a == "page":
        b = rnd.choice(PAGE_BUTTONS)
        p.click(b, idle=rnd.choice((0, 20, 100)))
        return f"page {b}"
    if a == "scroll":
        b = rnd.choice(("pscrUpButton", "pscrDownButton"))
        p.click(b)
        return b
    if a == "knob":
        k = rnd.randrange(10)
        x, y = p.center(f"lcdKnob{k}")
        dx = rnd.choice((0, 0, rnd.randint(-80, 80)))
        dy = rnd.randint(-120, 120)
        before = p.ctr_snapshot()
        p.drag_at(x + rnd.randint(-5, 5), y + rnd.randint(-5, 5), dx, dy, steps=rnd.randint(1, 12),
                  keys=rnd.choice((0, 0, MK_SHIFT)), idle_step=rnd.choice((0, 20)))
        p.track("knob drag (random)", before)
        return f"knob {k} drag {dx},{dy}"
    if a == "knobdbl":
        k = rnd.randrange(10)
        p.choose([rnd.randint(0, 20)])
        p.click(f"lcdKnob{k}", double=True)
        return f"knob {k} double click"
    col, row = rnd.randrange(44), rnd.randrange(2)
    x, y = p.lcd_cell_xy(col, row, dx=rnd.randint(1, 10), dy=rnd.randint(1, 18))
    if a == "lcdclick":
        before = p.ctr_snapshot()
        p.click_at(x, y)
        p.track("lcd click (random)", before)
        return f"lcd click {col},{row}"
    if a == "lcddbl":
        before = p.ctr_snapshot()
        p.choose(rnd.choice(([], [rnd.randint(0, 130)])))
        p.click_at(x, y, double=True)
        p.track("popup choice (random)", before)
        return f"lcd double click {col},{row}"
    if a == "lcddrag":
        before = p.ctr_snapshot()
        p.drag_at(x, y, rnd.randint(-30, 30), rnd.randint(-80, 80), steps=rnd.randint(1, 8))
        p.track("lcd drag (random)", before)
        return f"lcd drag {col},{row}"
    if a == "lcdright":
        p.choose(rnd.choice(([], [rnd.randint(0, 40)])))
        p.click_at(x, y, right=True)
        p.context_menu(x, y)
        return f"lcd right click {col},{row}"
    if a == "context":
        x, y = rnd.randrange(626), rnd.randrange(430)
        p.choose(rnd.choice(([], [rnd.randint(0, 45)])))
        p.context_menu(x, y)
        return f"context menu {x},{y}"
    if a == "updown":
        b = rnd.choice(("upButton", "downButton", "BankButton"))
        p.click(b)
        return b
    if a == "progpopup":
        path = rnd.choice(([], [rnd.randint(0, 140)]))
        p.choose(path)
        p.choose([rnd.randint(0, 140)])       # a bank item reopens the menu
        p.click_at(40, 52, right=True)
        return f"program popup {path}"
    if a == "namepopup":
        p.choose([rnd.randint(0, 140)])
        p.click("progNameEdit", double=True)
        return "name double click"
    if a == "buttons":
        b = rnd.choice(("buttSync", "buttAm", "buttMono", "initButton", "menuPanicImage"))
        p.click(b)
        return b
    if a == "options":
        path = rnd.choice(OPTION_PATHS)
        p.choose(path)
        p.click("menuOptImage")
        return f"options {path}"
    if a == "write":
        script = [rnd.choice(["item %d" % rnd.randrange(128), "bank", "compare", "item %d" % rnd.randrange(128)])
                  for _ in range(rnd.randint(0, 4))] + [rnd.choice(["ok", "cancel", "key 13", "key 27", "dbl 7"])]
        p.modal(*script)
        p.click("writeButton")
        return f"WRITE {script}"
    if a == "info":
        p.choose([rnd.choice((0, 2))])
        p.click("menuInfoImage")
        return "info"
    if a == "hover":
        x, y = rnd.randrange(626), rnd.randrange(430)
        p.mouse(WM_MOUSEMOVE, x, y)
        return f"hover {x},{y}"
    if a == "idle":
        ms = rnd.choice((20, 60, 200, 700, 1500))
        p.idle(ms)
        return f"idle {ms}"
    text = "".join(rnd.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789-") for _ in range(rnd.randint(0, 8)))
    p.name_focus(True, p.o.state()["name"])
    p.o.set_name_text(text)
    p.c.set_name_text(text)
    p.name_focus(False, text)
    return f"name {text!r}"


def scenario_random(pair, seeds=(11, 22, 33), actions=350):
    """Long random action sequences (seeded), compared after every action."""
    p = pair
    for seed in seeds:
        rnd = random.Random(seed)
        for i in range(actions):
            what = random_action(p, rnd)
            step(p, f"random {seed}#{i}: {what}", library=(i % 25 == 0))


SCENARIOS = [("pages", scenario_values), ("basic", scenario_basic), ("programs/menus", scenario_programs_menus),
             ("dialogs", scenario_dialogs), ("files", scenario_files), ("midi", scenario_midi),
             ("host", scenario_host), ("random", scenario_random)]


def coverage_report(pair):
    """pages x parameters x actions"""
    tree = pair.c.pages()
    keys = [(pi, si, k) for pi, pg in enumerate(tree) for si, sp in enumerate(pg["subs"]) for k in range(len(sp["params"]))]
    subs = {(pi, si) for pi, si, _ in keys}
    actions = sorted({a for v in pair.coverage.values() for a in v})
    print(f"coverage: {len(tree)} pages, {len(subs)} sub-pages, {len(keys)} parameters")
    print(f"  sub-pages with covered parameters: {len({(a, b) for a, b, _ in pair.coverage})}/{len(subs)}")
    for a in actions:
        n = sum(1 for k in keys if a in pair.coverage.get(k, ()))
        print(f"  {a:28s} {n:4d}/{len(keys)} parameters")
    edit = {"knob drag", "knob fine drag", "knob drag (random)", "popup choice", "popup choice (random)",
            "lcd drag", "lcd drag (random)"}
    editable = [(pi, si, k) for pi, si, k in keys if tree[pi]["subs"][si]["params"][k]["min"] <
                tree[pi]["subs"][si]["params"][k]["max"]]
    n = sum(1 for k in editable if pair.coverage.get(k, set()) & edit)
    print(f"  {'changed by a GUI gesture':28s} {n:4d}/{len(editable)} editable parameters (min < max)")
    missing = [k for k in editable if not pair.coverage.get(k, set()) & edit]
    if missing:
        print(f"  never changed by a gesture: {missing[:20]}{' ...' if len(missing) > 20 else ''}")
    print("  events compared: " + ", ".join(f"{k} {v}" for k, v in sorted(pair.event_counts.items())))


def main():
    only = sys.argv[1:]
    t0 = time.time()
    pair = Pair()
    print(f"open: {'ok' if not pair.failures else pair.failures[0]}  ({time.time() - t0:.1f}s)")
    for name, fn in SCENARIOS:
        if only and name not in only:
            continue
        t0, c0, f0 = time.time(), pair.checks, len(pair.failures)
        fn(pair)
        print(f"{name}: checks {pair.checks - c0}, failures {len(pair.failures) - f0} ({time.time() - t0:.1f}s)")
    coverage_report(pair)
    print(f"total: checks {pair.checks}, failures {len(pair.failures)}")
    return 1 if pair.failures else 0


if __name__ == "__main__":
    sys.exit(main())
