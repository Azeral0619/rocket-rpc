#include "rocket/net/timer.h"
#include "rocket/common/log.h"
#include "rocket/net/timer_event.h"
#include <algorithm>
#include <array>
#include <bits/time.h>
#include <bits/types/struct_itimerspec.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace rocket {

namespace {
constexpr std::int64_t kMillisecondsPerSecond = 1000;
constexpr std::int64_t kNanosecondsPerMillisecond = 1000000;
constexpr std::size_t kTimerfdBufferSize = 8;
} // namespace

Timer::Timer() {
    m_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (m_fd < 0) {
        return;
    }
    ROCKET_LOG_DEBUG("timer fd={}", m_fd);

    listen(TriggerEvent::IN_EVENT, [this]() { onTimer(); });
}

void Timer::addTimerEvent(const TimerEvent::s_ptr& event) {
    if (!event) {
        return;
    }

    bool need_reset = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_pending_events.empty() || event->getArriveTime() < m_pending_events.begin()->first) {
            need_reset = true;
        }

        m_pending_events.insert({event->getArriveTime(), event});
    }

    if (need_reset) {
        resetArriveTime();
    }
}

void Timer::deleteTimerEvent(const TimerEvent::s_ptr& event) {
    if (!event) {
        return;
    }

    event->setCancelled(true);

    std::lock_guard<std::mutex> lock(m_mutex);

    auto range = m_pending_events.equal_range(event->getArriveTime());
    for (auto it = range.first; it != range.second;) {
        if (it->second == event) {
            it = m_pending_events.erase(it);
        } else {
            ++it;
        }
    }
    ROCKET_LOG_DEBUG("success delete TimerEvent at arrive time {}", event->getArriveTime());
}

void Timer::onTimer() {
    std::array<char, kTimerfdBufferSize> buf{};
    while (::read(m_fd, buf.data(), buf.size()) != -1 || errno != EAGAIN) {
    }

    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
    const std::int64_t current_time = now_ms.time_since_epoch().count();

    std::vector<TimerEvent::s_ptr> tasks_to_execute;
    std::vector<TimerEvent::s_ptr> tasks_to_readd;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_pending_events.begin();
        auto end_it = it;

        while (end_it != m_pending_events.end() && end_it->first <= current_time) {
            if (!end_it->second->isCancelled()) {
                tasks_to_execute.push_back(end_it->second);

                if (end_it->second->isRepeated()) {
                    tasks_to_readd.push_back(end_it->second);
                }
            }
            ++end_it;
        }

        m_pending_events.erase(m_pending_events.begin(), end_it);
    }

    for (auto& task : tasks_to_readd) {
        task->resetArriveTime();
        addTimerEvent(task);
    }

    resetArriveTime();

    for (const auto& task : tasks_to_execute) {
        if (task->getCallback()) {
            task->getCallback()();
        }
    }
}

void Timer::resetArriveTime() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_pending_events.empty()) {
        return;
    }

    const std::int64_t next_arrive_time = m_pending_events.begin()->first;

    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
    const std::int64_t current_time = now_ms.time_since_epoch().count();

    const std::int64_t interval = std::max<std::int64_t>(0, next_arrive_time - current_time);

    struct itimerspec new_value{};

    new_value.it_value.tv_sec = interval / kMillisecondsPerSecond;
    new_value.it_value.tv_nsec = (interval % kMillisecondsPerSecond) * kNanosecondsPerMillisecond;

    const int ret = ::timerfd_settime(m_fd, 0, &new_value, nullptr);
    if (ret != 0) {
        ROCKET_LOG_ERROR("timerfd_settime error, errno={}, error={}", errno, strerror(errno));
    }
}

std::size_t Timer::pendingCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pending_events.size();
}

} // namespace rocket
