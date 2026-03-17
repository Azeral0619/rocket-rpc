#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace rocket {

class TimerEvent {

  public:
    using s_ptr = std::shared_ptr<TimerEvent>;

    TimerEvent(std::int64_t interval, bool is_repeated, std::function<void()> cb);

    ~TimerEvent() = default;

    TimerEvent(const TimerEvent&) = delete;
    TimerEvent& operator=(const TimerEvent&) = delete;
    TimerEvent(TimerEvent&&) noexcept = default;
    TimerEvent& operator=(TimerEvent&&) noexcept = default;

    [[nodiscard]] std::int64_t getArriveTime() const noexcept { return m_arrive_time; }

    [[nodiscard]] std::int64_t getInterval() const noexcept { return m_interval; }

    [[nodiscard]] bool isCancelled() const noexcept { return m_is_cancelled; }

    [[nodiscard]] bool isRepeated() const noexcept { return m_is_repeated; }

    [[nodiscard]] const std::function<void()>& getCallback() const noexcept { return m_task; }

    void setCancelled(bool value) noexcept { m_is_cancelled = value; }

    void cancel() noexcept { m_is_cancelled = true; }

    void resetArriveTime();

  private:
    std::int64_t m_arrive_time{0}; // ms
    std::int64_t m_interval{0};    // ms
    bool m_is_repeated{false};
    bool m_is_cancelled{false};
    std::function<void()> m_task;
};

} // namespace rocket