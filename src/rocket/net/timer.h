#pragma once

#include "rocket/net/timer_event.h"
#include "rocket/net/timing_wheel.h"
#include <cstdint>
#include <memory>
#include <optional>

namespace rocket {

// Heap/wheel-based timer — no timerfd dependency.
// Owned by EventLoop; EventLoop polls msUntilNextExpire() for the epoll_wait
// timeout and calls fireExpired() after each poll cycle.
class Timer {
  public:
    Timer() = default;
    ~Timer() = default;

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    Timer(Timer&&) = delete;
    Timer& operator=(Timer&&) = delete;

    void addTimerEvent(const TimerEvent::s_ptr& event);

    void deleteTimerEvent(const TimerEvent::s_ptr& event);

    [[nodiscard]] std::optional<std::int64_t> msUntilNextExpire() const;

    void fireExpired(std::int64_t now_ms);

    [[nodiscard]] std::size_t pendingCount() const;

  private:
    TimingWheel m_wheel;
};

} // namespace rocket
