"""Extract the data of the editor logic (lcdSetup / lcdControl / formatters) from the original
and generate src/gui/logic/data/GuiLogicData.cpp.

The 8 KB setup routine FUN_0047e340 (unit_47e330) builds the 18 VFD pages: it passes one
definition string per page to the page object (ClcdCtr_pageML.setText, which splits it into
sub-pages of two lines and lets ClcdCtr_pageSL parse each one), then configures every sub-page
(title, menu group, parameter base index, popup menu indent/hidden flags) and the parsed
parameters (hint text, edit buffer parameter index, formatter/popup-text callbacks, popup
layout, change callback). We run it in the emulator, capture the definition strings it passes
(trace of ClcdCtr_pageML.setText at 0x45a2dc on the top level page objects) and dump the
resulting object tree. The C++ port parses the same definition strings with the ported parser
and applies the per sub-page / per parameter setup recorded here.

Also extracted: the VFD character maps of both TLCD3 displays and the text tables used by the
value formatters (unit editBuffer 0x461114..0x461c1c), read from the emulated process memory.

Usage: python re/scripts/extract_gui_logic_data.py [--dump file.json]
"""
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "oracle"))
from vsthost import SQ8LHost  # noqa: E402

OUT = os.path.join(ROOT, "src", "gui", "logic", "data", "GuiLogicData.cpp")

VMT = {0x4587FC: "param", 0x45897C: "SL", 0x458A88: "ML", 0x458B84: "ctr"}
PAGE_ML_SET_TEXT = 0x45A2DC
PAGE_SL_SET_TEXT = 0x459C18

# formatter / popup text / callback code addresses -> C++ enumerator names
FMT = {
    0: "None",
    0x458FF8: "OffOn", 0x45903C: "Unsigned", 0x45907C: "Signed",
    0x461114: "Wave", 0x461184: "ModSource", 0x4612A4: "LfoReset", 0x4612E4: "LfoWave",
    0x4614E4: "LfoHuman", 0x4615F0: "LfoPhase", 0x461698: "LfoDelayMode", 0x4616E4: "LfoModMode",
    0x461784: "LfoPlay", 0x461808: "EnvVelLevel", 0x461898: "EnvT4", 0x461928: "EnvShape",
    0x461990: "EnvT1VMode", 0x4619D4: "BendMode", 0x461A14: "Saturation", 0x461AC0: "VoiceSteal",
    0x461B10: "Dca13Mode", 0x461B5C: "Dca4Mode", 0x461BAC: "DcBlock",
}
POP = {0: "None", 0x4611C4: "ModSource", 0x4613DC: "LfoWave"}
CHANGE = {0: "None", 0x48489C: "Form"}

# text tables (pointer variables in the data segment -> table of ShortStrings)
TABLES = [
    # name, pointer variable, count, entry size, C comment
    ("kWaveNames", 0x4C34F4, 75, 7, "oscillator waves (FUN_00461114)"),
    ("kModSourceNames", 0x4C3450, 147, 8, "modulation sources -1..145 (FUN_00461184)"),
    ("kLfoWaveNames", 0x4C333C, 5, 4, "LFO basic waves (FUN_004612e4)"),
    ("kShapeNames", 0x4C3468, 8, 5, "EXP/TAN shapes (FUN_00461928, LFO waves 75..82)"),
    ("kBendModeNames", 0x4C3260, 7, 8, "pitch bend modes (FUN_004619d4)"),
]
LFO_WAVE_MAP = (0x4C30E8, 70, 4)   # byte table: LFO wave 5..74 -> oscillator wave index
LCD_CHARS = 0x4C3268               # pointer to the VFD character string (main display)
NUMLCD_CHARS = 0x4C3330            # pointer to '0123456789ABCDPU' (program number display)


class Dumper:
    def __init__(self):
        self.h = SQ8LHost(gui=True)
        self.h.load()
        self.h.start()
        self.e = self.h.emu
        self.page_texts = {}
        self.sl_texts = {}
        self.e.trace(PAGE_ML_SET_TEXT, self._on_ml_text, lambda emu, ctx: None)
        self.e.trace(PAGE_SL_SET_TEXT, self._on_sl_text, lambda emu, ctx: None)
        self.h.open_editor()
        self.g = self.h.gui
        self.g.idle(300)
        form_hwnd = next(h for h, w in self.g.windows.items() if w.cls == "tplugeditform")
        self.form = list(self.g.windows[form_hwnd].props.values())[0]

    def lstr(self, p):
        if not p:
            return ""
        n = self.e.s32(p - 4)
        return self.e.read(p, n).decode("latin1")

    def dynlen(self, p):
        return self.e.s32(p - 4) if p else 0

    def _on_ml_text(self, emu):
        r = emu.regs()
        if emu.s32(r["eax"] + 0x1C) == 0:   # top level page object (depth 0)
            self.page_texts[r["eax"]] = self.lstr(r["edx"])

    def _on_sl_text(self, emu):
        r = emu.regs()
        self.sl_texts[r["eax"]] = self.lstr(r["edx"])

    def sub_pages(self, page):
        """page (ML depth 0) -> list of SL objects (through ML1 -> ML2 -> SL)."""
        e = self.e
        ml1s = e.u32(page + 0xC)
        assert self.dynlen(ml1s) == 1
        ml1 = e.u32(ml1s)
        out = []
        ml2s = e.u32(ml1 + 0xC)
        for i in range(self.dynlen(ml2s)):
            ml2 = e.u32(ml2s + 4 * i)
            sls = e.u32(ml2 + 0xC)
            assert self.dynlen(sls) == 1
            out.append(e.u32(sls))
        return out

    def param(self, p):
        e = self.e
        s = lambda off: e.s32(p + off)  # noqa: E731
        u = lambda off: e.u32(p + off)  # noqa: E731
        return dict(
            knob=s(4), index=s(8), ordinal=s(0xC), name=self.lstr(u(0x10)), hint=self.lstr(u(0x14)),
            label=self.lstr(u(0x18)), valueText=self.lstr(u(0x1C)), width=s(0x20), x=s(0x24), y=s(0x28),
            min=s(0x2C), max=s(0x30), value=s(0x34), fmt=u(0x38), fmtData=u(0x3C), pop=u(0x40),
            customFmt=e.u8(p + 0x48), f4c=s(0x4C), filter=u(0x50), change=u(0x58), userData=s(0x60),
            showNumber=e.u8(p + 0x64), f65=e.u8(p + 0x65), perColumn=s(0x68), editor=u(0x70),
            paramIndex=s(0x74), highlight=e.u8(p + 0x78), mouseCb=u(0x80))

    def sub_page(self, sl):
        e = self.e
        s = lambda off: e.s32(sl + off)  # noqa: E731
        params = e.u32(sl + 0x18)
        knobs = e.u32(sl + 0x1C)
        return dict(
            text=self.sl_texts.get(sl), title=self.lstr(e.u32(sl + 0x14)), c=s(0xC), group=s(0x10),
            editor=e.u32(sl + 0x20), base=s(0x24), offset=s(0x28), indent=s(0x2C), hidden=s(0x30),
            knobs=[e.s32(knobs + 4 * i) for i in range(self.dynlen(knobs))],
            params=[self.param(e.u32(params + 4 * i)) for i in range(self.dynlen(params))])

    def tree(self):
        e = self.e
        ctr = e.u32(self.form + 0x4DC)
        pages = e.u32(ctr + 4)
        out = []
        for i in range(self.dynlen(pages)):
            page = e.u32(pages + 4 * i)
            out.append(dict(text=self.page_texts.get(page),
                            subs=[self.sub_page(sl) for sl in self.sub_pages(page)]))
        return out

    def short_strings(self, ptr_var, count, size):
        base = self.e.u32(ptr_var)
        out = []
        for i in range(count):
            raw = self.e.read(base + i * size, size)
            out.append(raw[1:1 + raw[0]].decode("latin1"))
        return out

    def tables(self):
        t = {name: self.short_strings(ptr, n, sz) for name, ptr, n, sz, _ in TABLES}
        base = self.e.u32(LFO_WAVE_MAP[0])
        t["kLfoWaveMap"] = [self.e.u8(base + i * LFO_WAVE_MAP[2]) for i in range(LFO_WAVE_MAP[1])]
        return t

    def lcd_maps(self):
        e = self.e
        out = {}
        for name, off, var in (("lcd", 0x390, LCD_CHARS), ("numLcd", 0x3B4, NUMLCD_CHARS)):
            lcd = e.u32(self.form + off)
            out[name] = dict(
                chars=e.read(e.u32(var), 256).split(b"\0")[0].hex(),
                map=[e.s32(lcd + 0x228 + 4 * c) for c in range(256)],
                frames=e.s32(lcd + 0x224), cols=e.s32(lcd + 0x218), rows=e.s32(lcd + 0x21C),
                geometry=[e.s32(lcd + o) for o in range(0x1F8, 0x218, 4)])
        return out


def cstr(s):
    if s is None:
        return "nullptr"
    out = '"'
    for ch in s:
        o = ord(ch)
        if ch in '\\"':
            out += "\\" + ch
        elif 32 <= o < 127:
            out += ch
        else:
            out += f'\\x{o:02x}""'
    return out + '"'


def generate(tree, tables, maps):
    L = []
    w = L.append
    w("// Generated by re/scripts/extract_gui_logic_data.py from the original SQ8L.dll (run in the")
    w("// emulator): the VFD page definitions of FUN_0047e340 with their setup, the character maps")
    w("// of the two TLCD3 displays and the formatter text tables. Do not edit.")
    w('#include "../LcdData.h"')
    w("")
    w("namespace sq8l::gui::data {")
    w("")
    for i, page in enumerate(tree):
        for j, sp in enumerate(page["subs"]):
            w(f"static const ParamSetup kParams_{i}_{j}[] = {{")
            for p in sp["params"]:
                if p["ordinal"] < 0 and not p["hint"] and p["paramIndex"] < 0:
                    continue   # static label: nothing to set up
                fmt = FMT[p["fmt"]] if p["customFmt"] else "None"
                w(f"    {{{p['index']}, {cstr(p['hint'])}, {p['paramIndex']}, Fmt::{fmt}, "
                  f"PopText::{POP[p['pop']]}, {'true' if p['showNumber'] else 'false'}, "
                  f"{p['perColumn']}, Change::{CHANGE[p['change']]}}},  // {p['name']}")
            w("};")
    w("")
    w("const PageSetup kPages[kNumPages] = {")
    for i, page in enumerate(tree):
        w(f"    {{{cstr(page['text'])},")
        w(f"     {len(page['subs'])}, {{")
        for j, sp in enumerate(page["subs"]):
            n = sum(1 for p in sp["params"] if not (p["ordinal"] < 0 and not p["hint"] and p["paramIndex"] < 0))
            w(f"        {{{cstr(sp['title'])}, {sp['c']}, {sp['group']}, {sp['base']}, {sp['offset']}, "
              f"{sp['indent']}, {sp['hidden']}, kParams_{i}_{j}, {n}}},")
        w("     }},")
    w("};")
    w("")
    for name, _, n, sz, comment in TABLES:
        w(f"// {comment}")
        w(f"const char* const {name}[{n}] = {{")
        vals = tables[name]
        for k in range(0, n, 8):
            w("    " + " ".join(cstr(v) + "," for v in vals[k:k + 8]))
        w("};")
    w("// LFO waves 5..74 -> oscillator wave (FUN_004613dc)")
    w("const uint8_t kLfoWaveMap[70] = {")
    vals = tables["kLfoWaveMap"]
    for k in range(0, 70, 14):
        w("    " + " ".join(f"{v}," for v in vals[k:k + 14]))
    w("};")
    w("")
    for name, m in (("kLcdChars", maps["lcd"]), ("kNumLcdChars", maps["numLcd"])):
        chars = bytes.fromhex(m["chars"]).decode("latin1")
        w(f"const char {name}[] = {cstr(chars)};")
    w("")
    w("}  // namespace sq8l::gui::data")
    return "\n".join(L) + "\n"


def main():
    d = Dumper()
    tree = d.tree()
    tables = d.tables()
    maps = d.lcd_maps()
    if "--dump" in sys.argv:
        path = sys.argv[sys.argv.index("--dump") + 1]
        with open(path, "w") as f:
            json.dump(dict(tree=tree, tables=tables, maps=maps), f, indent=1)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w") as f:
        f.write(generate(tree, tables, maps))
    print("pages", len(tree), "sub-pages", sum(len(p["subs"]) for p in tree), "->", OUT)


if __name__ == "__main__":
    main()
