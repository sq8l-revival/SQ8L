// (port) Text for the main VFD where '.' is the decimal point of the cell before it: the '.'
// glyph lights the underline and costs a column, the attribute costs none. Main VFD only —
// numLcd has one frame per glyph.
#pragma once

#include <string_view>

namespace sq8l::gui {

class LcdDisplay;

// Columns `text` occupies: its length without the dots.
int dottedWidth(std::string_view text);

// Writes `text` at (col, row) with `attr`, folding each '.' into the cell before it (attr 1
// becomes 3). A leading dot is dropped. Returns the columns written.
int writeDotted(LcdDisplay& lcd, int col, int row, std::string_view text, int attr);

}  // namespace sq8l::gui
