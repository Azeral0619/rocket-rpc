#pragma once

#include <atomic>
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
    TimerEvent(TimerEvent&&) noexcept = delete;
    TimerEvent& operator=(TimerEvent&&) noexcept = delete;

    // Pooled factory — prefer over make_shared for short-lived events.
    // Returns a shared_ptr with a custom deleter that recycles the memory.
    static s_ptr create(std::int64_t interval, bool is_repeated, std::function<void()> cb);

    [[nodiscard]] std::int64_t getArriveTime() const noexcept { return m_arrive_time; }

    [[nodiscard]] std::int64_t getInterval() const noexcept { return m_interval; }

    [[nodiscard]] bool isCancelled() const noexcept {
        return m_is_cancelled.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool isRepeated() const noexcept { return m_is_repeated; }

    [[nodiscard]] const std::function<void()>& getCallback() const noexcept { return m_task; }

    void setCancelled(bool value) noexcept {
        m_is_cancelled.store(value, std::memory_order_release);
    }

    void cancel() noexcept {
        m_is_cancelled.store(true, std::memory_order_release);
    }

    void resetArriveTime();

  private:
    std::int64_t m_arrive_time{0}; // ms
    std::int64_t m_interval{0};    // ms
    bool m_is_repeated{false};
    std::atomic<bool> m_is_cancelled{false};
    std::function<void()> m_task;
};

} // namespace rocket
