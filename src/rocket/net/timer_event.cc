#include "rocket/net/timer_event.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>

namespace rocket {

TimerEvent::TimerEvent(std::int64_t interval, bool is_repeated, std::function<void()> cb)
    : m_interval(interval), m_is_repeated(is_repeated), m_task(std::move(cb)) {
    resetArriveTime();
}

void TimerEvent::resetArriveTime() {
    const auto now = std::chrono::steady_clock::now();
    const auto ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
    m_arrive_time = ms.time_since_epoch().count() + m_interval;
}

} // namespace rocket
