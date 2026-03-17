#pragma once

#include "fd_event.h"
#include "timer_event.h"
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>

namespace rocket {

class Timer : public FdEvent {
  public:
    Timer();

    ~Timer() = default;

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    Timer(Timer&&) = delete;
    Timer& operator=(Timer&&) = delete;

    void addTimerEvent(const TimerEvent::s_ptr& event);

    void deleteTimerEvent(const TimerEvent::s_ptr& event);

    void onTimer();

    [[nodiscard]] std::size_t pendingCount() const;

  private:
    void resetArriveTime();

    std::multimap<std::int64_t, TimerEvent::s_ptr> m_pending_events;
    mutable std::mutex m_mutex;
};

} // namespace rocket