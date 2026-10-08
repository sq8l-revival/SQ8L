#!/usr/bin/env python3
"""Regenerate src/gui/text/FontData.cpp from the fonts in third_party/fonts.

    python3 tools/embed_fonts.py
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTS = [("kLiberationSansRegular", "LiberationSans-Regular.ttf"),
         ("kLiberationSansItalic", "LiberationSans-Italic.ttf"),
         ("kLiberationSansBoldItalic", "LiberationSans-BoldItalic.ttf")]

out = ["// Generated from third_party/fonts (Liberation Sans, SIL Open Font License 1.1). Do not edit.",
       "#include <cstddef>", "#include <cstdint>", "", "namespace sq8l::gui::fonts {", ""]
for name, file in FONTS:
    data = open(os.path.join(ROOT, "third_party", "fonts", file), "rb").read()
    n = len(data)
    out += [f"extern const uint8_t {name}[{n}];", f"extern const size_t {name}Size;",
            f"const size_t {name}Size = {n};", f"alignas(4) const uint8_t {name}[{n}] = {{"]
    for i in range(0, n, 32):
        out.append("    " + ",".join(str(b) for b in data[i:i + 32]) + ",")
    out += ["};", ""]
out += ["}  // namespace sq8l::gui::fonts", ""]
with open(os.path.join(ROOT, "src", "gui", "text", "FontData.cpp"), "w", newline="\n") as f:
    f.write("\n".join(out))
