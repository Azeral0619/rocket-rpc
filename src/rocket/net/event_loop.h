#pragma once

#include "rocket/net/timer_event.h"
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <sys/epoll.h>
#include <thread>
#include <vector>

namespace rocket {

class FdEvent;
class WakeUpFdEvent;
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

    void addEpollEvent(FdEvent* event);

    void deleteEpollEvent(FdEvent* event);

    [[nodiscard]] bool isInLoopThread() const noexcept;

    void addTask(std::function<void()> cb, bool is_wake_up = false);

    void addTimerEvent(const TimerEvent::s_ptr& event);

    [[nodiscard]] bool isLooping() const noexcept;

    [[nodiscard]] std::thread::id getThreadId() const noexcept { return m_thread_id; }

    [[nodiscard]] static EventLoop* GetCurrentEventLoop();

  private:
    void dealWakeup();

    void initWakeUpFdEvent();

    void initTimer();

    void processEvents(const std::vector<epoll_event>& events, int nfds);

    void processPendingTasks();

    std::thread::id m_thread_id;

    int m_epoll_fd{-1};

    int m_wakeup_fd{-1};

    std::unique_ptr<WakeUpFdEvent> m_wakeup_fd_event;

    bool m_stop_flag{false};

    std::set<int> m_listen_fds;

    std::queue<std::function<void()>> m_pending_tasks;

    mutable std::mutex m_mutex;

    std::unique_ptr<Timer> m_timer;

    bool m_is_looping{false};
};

} // namespace rocket
