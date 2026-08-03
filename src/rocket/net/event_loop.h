#pragma once

#include "rocket/net/fd_event.h"
#include "rocket/net/poller/poller.h"
#include "rocket/net/timer_event.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace rocket {

class WakeupChannel;
class Timer;

class EventLoop {
  public:
    EventLoop();

    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;
    EventLoop(EventLoop&&) = delete;
    EventLoop& operator=(EventLoop&&) = delete;

    void loop();

    void wakeup();

    void stop();

    // Register / update / remove an fd event with the poller.
    void addEpollEvent(FdEvent* event);
    void deleteEpollEvent(FdEvent* event);

    [[nodiscard]] bool isInLoopThread() const noexcept;

    void addTask(std::function<void()> cb, bool is_wake_up = false);

    void addTimerEvent(const TimerEvent::s_ptr& event);
    void deleteTimerEvent(const TimerEvent::s_ptr& event);

    [[nodiscard]] bool isLooping() const noexcept;

    [[nodiscard]] std::thread::id getThreadId() const noexcept;

    void runInLoop(std::function<void()> fn);
    void queueInLoop(std::function<void()> fn);
    void assertInLoopThread() const noexcept;

    [[nodiscard]] static EventLoop* GetCurrentEventLoop();

  private:
    void initWakeup();
    void processEvents(const std::vector<Poller::ActiveEvent>& events);
    void processPendingTasks();
    void processTimerEvents();

    std::thread::id m_thread_id;
    mutable std::mutex m_state_mutex;

    std::unique_ptr<Poller> m_poller;
    std::unique_ptr<WakeupChannel> m_wakeup_channel;
    std::unique_ptr<FdEvent> m_wakeup_fd_event;

    std::atomic<bool> m_stop_flag{false};

    std::queue<std::function<void()>> m_pending_tasks;
    mutable std::mutex m_mutex;
    // Coalesce cross-thread task notifications. The task queue remains the
    // control plane; only the empty-to-pending transition writes eventfd/pipe.
    std::atomic<bool> m_task_wakeup_pending{false};

    std::unique_ptr<Timer> m_timer;

    std::atomic<bool> m_is_looping{false};
    bool m_valid{false};

  public:
    // Connection count for least-connections load balancing (Hical pattern).
    // Updated by client pools and TcpServer as connections enter/leave.
    std::atomic<std::size_t> m_connection_count{0};
};

} // namespace rocket
