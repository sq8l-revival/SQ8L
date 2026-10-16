#include "VfdText.h"

#include "controls/LcdDisplay.h"

namespace sq8l::gui {

// VFD character attributes
namespace {
    constexpr int kNoAttr = 0;
    constexpr int kUnderlineAttr = 1;
    constexpr int kDotAttr = 2;
    constexpr int kUnderlineDotAttr = 3;
}

int dottedWidth(std::string_view text) {
    int n = 0;
    for (char c : text)
        if (c != '.') n++;
    return n;
}

int writeDotted(LcdDisplay& lcd, int col, int row, std::string_view text, int attr) {
    if (row < 0 || row >= lcd.rows()) return 0;
    int x = col;
    for (char c : text) {
        if (c == '.') {
            // The dot belongs to the cell to the left; at the start of the write there is none.
            if (x > col && x - 1 >= 0 && x - 1 < lcd.cols()) {
                const LcdDisplay::Cell& prev = lcd.cell(x - 1, row);
                lcd.setCell(x - 1, row, prev.ch, attr | kDotAttr);
            }
            continue;
        }
        if (x >= 0 && x < lcd.cols()) lcd.setCell(x, row, uint8_t(c), attr);
        x++;  // clipped like writeText: off the grid still advances the column
    }
    return x - col;
}

}  // namespace sq8l::gui
