// The panel behind the controls (the original's background picture, 626x430) drawn as
// vector shapes for the HD graphics: frames, the red band, the display bezels, the signal
// flow lines, the labels, the logo, with a grain like the original's surfaces. Positions,
// sizes and colours were measured from the picture (internal to src/gui/hd).
#pragma once

#include "HdDraw.h"
#include "TextRenderer.h"

namespace sq8l::gui::hd {

// Draw the panel at S window pixels per form pixel over hd.clip.
void drawPanel(Hd& hd, float S, TextRenderer& text);

// The grain of the original's surfaces at form point (x, y): levels, standard deviation 1.65,
// about a form pixel in size, the same at every scale (also used on the buttons).
float grain(float x, float y);

}  // namespace sq8l::gui::hd
