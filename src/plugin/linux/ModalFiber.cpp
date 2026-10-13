#include "ModalFiber.h"

#include <sys/mman.h>
#include <unistd.h>

#include <utility>

namespace sq8l::linux_ui {

namespace {
// The editor logic, the layout of a drawn menu or dialog and a library or bank load included.
constexpr size_t kStackSize = 1u << 20;
// makecontext() hands its function ints only: the fiber being started is passed here.
thread_local ModalFiber* starting = nullptr;

size_t pageSize() { return static_cast<size_t>(sysconf(_SC_PAGESIZE)); }
}  // namespace

ModalFiber::ModalFiber() {
    void* p = mmap(nullptr, kStackSize + pageSize(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return;  // start() then runs the logic on the caller's stack, as before
    mprotect(p, pageSize(), PROT_NONE);  // the stack grows down: an overflow faults here
    stack_ = p;
}

ModalFiber::~ModalFiber() {
    if (stack_ != nullptr) munmap(stack_, kStackSize + pageSize());
}

void ModalFiber::start(std::function<void()> body) {
    if (state_ != State::idle || stack_ == nullptr) {
        body();  // already on the fiber (or no stack): just call it
        return;
    }
    body_ = std::move(body);
    getcontext(&fiber_);
    fiber_.uc_stack.ss_sp = static_cast<char*>(stack_) + pageSize();
    fiber_.uc_stack.ss_size = kStackSize;
    fiber_.uc_link = &caller_;  // where a finished body goes: the latest start or resume
    starting = this;
    makecontext(&fiber_, &ModalFiber::entry, 0);
    enter();
}

void ModalFiber::resume() {
    if (state_ == State::suspended) enter();
}

void ModalFiber::yield() {
    if (state_ != State::running) return;
    state_ = State::suspended;
    swapcontext(&fiber_, &caller_);
}

void ModalFiber::entry() {
    ModalFiber* self = starting;
    starting = nullptr;
    self->body_();
    self->body_ = nullptr;
    self->state_ = State::idle;
}  // -> uc_link

void ModalFiber::enter() {
    state_ = State::running;
    swapcontext(&caller_, &fiber_);  // back when the body yields or finishes
}

}  // namespace sq8l::linux_ui
