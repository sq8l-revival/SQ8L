#pragma once
#include <cstdint>
namespace sq8l {
// Drain an existing wrapper queue without replaying semantic edits. Stale values
// are consumed; a rejected publication retains that event and its entire suffix.
template<class Event, class Current, class Publish>
uint32_t drainClapPanelNotifications(const Event* events, uint32_t count,
                                     Current current, Publish publish) {
    uint32_t consumed = 0;
    for (; consumed < count; ++consumed) {
        if (!current(events[consumed])) continue;
        if (!publish(events[consumed])) break;
    }
    return consumed;
}
}
