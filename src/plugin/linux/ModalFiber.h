// The editor's logic on a stack of its own, so that a drawn menu or dialog never holds up
// the host (issue #36).
//
// The editor logic is a port of synchronous code: a popup menu, a message box or a file
// dialog returns the user's answer, and the code goes on from there. On Windows and macOS
// the system's modal loop keeps the host's own events flowing meanwhile. On Linux the menus
// and dialogs are drawn in the plug-in window, and a loop of ours would run inside the
// host's call into the plug-in: the host's whole UI thread waits until the menu closes, and
// a desktop that pings the host's window (GNOME after 5 s) reports it as not responding.
//
// So the logic runs here instead. When it has to wait for the user, yield() goes back to
// the host's call, which returns; each later call from the host resumes it for a look, and
// once the answer is there the logic simply carries on where it stopped. One thread
// throughout, the host's UI thread: only the stack changes.
#pragma once

#include <functional>
#include <ucontext.h>

namespace sq8l::linux_ui {

class ModalFiber {
public:
    ModalFiber();
    ~ModalFiber();
    ModalFiber(const ModalFiber&) = delete;
    ModalFiber& operator=(const ModalFiber&) = delete;

    // Run `body` on the fiber's stack until it finishes or yields. Not while a body is
    // suspended (suspended()): there is one stack, and the logic is not reentrant anyway.
    void start(std::function<void()> body);
    // Go on with the suspended body until it yields again or finishes.
    void resume();
    // (on the fiber) back to whoever started or resumed it
    void yield();

    bool suspended() const { return state_ == State::suspended; }
    bool inside() const { return state_ == State::running; }

private:
    static void entry();
    void enter();  // switch to the fiber, until it yields or finishes

    enum class State { idle, running, suspended };
    State state_ = State::idle;
    std::function<void()> body_;
    void* stack_ = nullptr;  // mmap'ed, with a guard page below
    ucontext_t fiber_{}, caller_{};
};

}  // namespace sq8l::linux_ui
