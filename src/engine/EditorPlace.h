// (port, issue #45) Where the editor was left: the page and view on the display, the knob slot
// the keyboard focus was on, the page each group of page buttons was last on, the view the LFO
// pages share and the one the envelope pages share, and the focused slot of every sub-page
// visited so far.
//
// The engine only stores this and puts part of it in the host chunk, so that a project opens
// where it was left (OPTIONS -> Keyboard -> Page memory). It is opaque there: what the fields
// mean is the editor's business, and the editor is what reads them back. It lives in the engine
// rather than in the editor so that closing and reopening the editor window does not forget it.
#pragma once

#include <cstdint>

namespace sq8l {

struct EditorPlace {
    // 18 pages of at most 3 views; the index reserves 4 a page (page * 4 + sub).
    static constexpr int kNumSubPages = 72;

    int8_t page = -1;        // -1: nothing saved, which is a new instance and an old project
    int8_t sub = -1;
    int8_t knob = -1;        // the focused knob slot, -1 none
    int8_t groupPage[3] = {-1, -1, -1};  // the LFO / envelope / matrix page last used
    int8_t groupSub[2] = {0, 0};         // the view the LFOs share, and the envelopes'
    // Not carried in the chunk - there is no room for it, and it is not what a project needs
    // to remember. Seeded from `knob` when a chunk is loaded, and filled in again by browsing.
    int8_t focusBySub[kNumSubPages];

    EditorPlace() {
        for (int8_t& f : focusBySub) f = -1;
    }

    bool valid() const { return page >= 0; }
    static int index(int page, int sub) {
        const int i = page * 4 + sub;
        return (page >= 0 && sub >= 0 && sub < 4 && i < kNumSubPages) ? i : -1;
    }
};

}  // namespace sq8l
