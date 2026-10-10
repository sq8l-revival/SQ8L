#include "X11Pointer.h"

#include <X11/Xlib.h>

namespace sq8l::x11 {

namespace {
Display* display() {
    static Display* d = XOpenDisplay(nullptr);  // kept for the life of the process
    return d;
}
}  // namespace

bool queryPointer(uintptr_t window, int& x, int& y) {
    Display* d = display();
    if (!d || !window) return false;
    ::Window root, child;
    int rx, ry, wx, wy;
    unsigned int mask;
    if (!XQueryPointer(d, static_cast<::Window>(window), &root, &child, &rx, &ry, &wx, &wy, &mask)) return false;
    x = wx;
    y = wy;
    return true;
}

void warpPointer(uintptr_t window, int x, int y) {
    Display* d = display();
    if (!d || !window) return;
    XWarpPointer(d, None, static_cast<::Window>(window), 0, 0, 0, 0, x, y);
    XFlush(d);
}

void setCursorVisible(uintptr_t window, bool visible) {
    Display* d = display();
    if (!d || !window) return;
    if (visible) {
        XUndefineCursor(d, static_cast<::Window>(window));  // back to the parent's cursor
    } else {
        // X11 has no "no cursor": define one from an empty 1x1 bitmap. Kept for the life of
        // the process, like the connection.
        static ::Cursor blank = None;
        if (blank == None) {
            char zero[1] = {0};
            Pixmap p = XCreateBitmapFromData(d, DefaultRootWindow(d), zero, 1, 1);
            XColor black{};
            blank = XCreatePixmapCursor(d, p, p, &black, &black, 0, 0);
            XFreePixmap(d, p);
        }
        XDefineCursor(d, static_cast<::Window>(window), blank);
    }
    XFlush(d);
}

}  // namespace sq8l::x11
