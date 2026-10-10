// The mouse pointer on X11 (the editor's "jumping mouse"), through a connection of our own:
// window ids are server-wide, so this works with the window pugl created. Under Wayland
// (XWayland) warping is refused by the compositor and does nothing.
#pragma once

#include <cstdint>

namespace sq8l::x11 {

// Pointer position in `window` pixels; false if unavailable.
bool queryPointer(uintptr_t window, int& x, int& y);
void warpPointer(uintptr_t window, int x, int y);
// Show the normal cursor over `window`, or a fully transparent one (the editor hides the
// cursor while a knob is turned). Only affects the plugin's own window.
void setCursorVisible(uintptr_t window, bool visible);

}  // namespace sq8l::x11
