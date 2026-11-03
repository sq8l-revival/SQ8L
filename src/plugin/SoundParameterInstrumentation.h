// Qualification-only recorder. Disabled builds contain no clocks or TLS access.
#pragma once
#ifndef SQ8L_SOUND_PARAMETER_INSTRUMENTATION
#define SQ8L_SOUND_PARAMETER_INSTRUMENTATION 0
#endif
#if SQ8L_SOUND_PARAMETER_INSTRUMENTATION
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
namespace sq8l::sound::instrumentation {
enum class Operation { Setter, Getter, Accepted, Refresh, Render, Apply, Projection, Replacement };
struct Record {
    Operation operation{};
    uint64_t begin=0, acquired=0, released=0, end=0;
    unsigned acquisitions=0;
};
struct Recorder {
    std::array<Record, 16384> records{};
    size_t count=0, dropped=0;
    uint64_t allocations=0, deallocations=0;
};
inline thread_local Recorder* recorder=nullptr;
inline thread_local unsigned depth=0;
inline uint64_t now() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
// Attach/detach only on the recording thread, outside measured scopes. Read after join.
class Scope {
public:
    explicit Scope(Operation operation) noexcept : output(recorder) {
        if (output) { record.operation=operation; record.begin=now(); ++depth; }
    }
    void acquired() noexcept { if(output) { record.acquired=now(); ++record.acquisitions; } }
    void released() noexcept { if(output) record.released=now(); }
    ~Scope() {
        if(!output) return;
        record.end=now(); --depth;
        if(output->count<output->records.size()) output->records[output->count++]=record;
        else ++output->dropped;
    }
private:
    Recorder* output;
    Record record{};
};
class Lock {
public:
    Lock(std::recursive_mutex& mutex, Operation operation) : scope(operation), mutex(mutex) {
        mutex.lock(); scope.acquired();
    }
    ~Lock() { mutex.unlock(); scope.released(); }
private:
    Scope scope;
    std::recursive_mutex& mutex;
};
}
#define SQ8L_MEASURE_LOCK(m, op) sq8l::sound::instrumentation::Lock lock(m, sq8l::sound::instrumentation::Operation::op)
#define SQ8L_MEASURE_SCOPE(op) sq8l::sound::instrumentation::Scope measurement(sq8l::sound::instrumentation::Operation::op)
#else
#define SQ8L_MEASURE_LOCK(m, op) std::lock_guard<std::recursive_mutex> lock(m)
#define SQ8L_MEASURE_SCOPE(op) do {} while(false)
#endif
