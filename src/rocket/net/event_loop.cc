#include "rocket/net/event_loop.h"
#include "rocket/common/log.h"
#include "rocket/net/fd_event.h"
#include "rocket/net/timer.h"
#include "rocket/net/timer_event.h"
#include "rocket/net/wakeup_fd_event.h"
#include <cerrno>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <sys/epoll.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace rocket {

namespace {
thread_local EventLoop* t_current_event_loop = nullptr; // NOLINT
constexpr int kMaxEvents = 10;
} // namespace

EventLoop::EventLoop() : m_thread_id(std::this_thread::get_id()) {
    ROCKET_LOG_DEBUG("EventLoop");
    if (t_current_event_loop != nullptr) {
        return;
    }
    t_current_event_loop = this;

    m_epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (m_epoll_fd < 0) {
        return;
    }

    initWakeUpFdEvent();
    initTimer();
}

EventLoop::~EventLoop() {
    ROCKET_LOG_DEBUG("~EventLoop");
    if (t_current_event_loop == this) {
        t_current_event_loop = nullptr;
    }

    if (m_epoll_fd >= 0) {
        ::close(m_epoll_fd);
    }

    if (m_wakeup_fd >= 0) {
        ::close(m_wakeup_fd);
    }
}

void EventLoop::initWakeUpFdEvent() {
    m_wakeup_fd_event = std::make_unique<WakeUpFdEvent>();
    m_wakeup_fd = m_wakeup_fd_event->getFd();

    addEpollEvent(m_wakeup_fd_event.get());
}

void EventLoop::initTimer() {
    m_timer = std::make_unique<Timer>();
    addEpollEvent(m_timer.get());
}

void EventLoop::loop() {
    m_is_looping = true;

    static thread_local std::vector<epoll_event> events(kMaxEvents);

    while (!m_stop_flag) {
        const int nfds = ::epoll_wait(m_epoll_fd, events.data(), kMaxEvents, -1);

        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        processEvents(events, nfds);
        processPendingTasks();
    }

    m_is_looping = false;
}

void EventLoop::processEvents(const std::vector<epoll_event>& events, int nfds) {
    for (int i = 0; i < nfds; ++i) {
        const epoll_event& ev = events[static_cast<std::size_t>(i)];
        auto* fd_event = static_cast<FdEvent*>(ev.data.ptr);

        if ((ev.events & EPOLLIN) != 0) {
            const auto& handler = fd_event->handler(FdEvent::TriggerEvent::IN_EVENT);
            if (handler) {
                handler();
            }
        }

        if ((ev.events & EPOLLOUT) != 0) {
            const auto& handler = fd_event->handler(FdEvent::TriggerEvent::OUT_EVENT);
            if (handler) {
                handler();
            }
        }

        if ((ev.events & EPOLLERR) != 0) {
            const auto& handler = fd_event->handler(FdEvent::TriggerEvent::ERROR_EVENT);
            if (handler) {
                handler();
            }
        }
    }
}

void EventLoop::processPendingTasks() {
    std::queue<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        tasks.swap(m_pending_tasks);
    }

    while (!tasks.empty()) {
        auto& task = tasks.front();
        if (task) {
            task();
        }
        tasks.pop();
    }
}

void EventLoop::wakeup() {
    if (m_wakeup_fd_event) {
        m_wakeup_fd_event->wakeup();
    }
}

void EventLoop::stop() {
    m_stop_flag = true;
    wakeup();
}

void EventLoop::addEpollEvent(FdEvent* event) {
    if (event == nullptr || event->getFd() < 0) {
        return;
    }

    epoll_event ev = event->getEpollEvent();
    ev.data.ptr = event;

    const int fd = event->getFd();

    if (!m_listen_fds.contains(fd)) {
        // 新增
        if (::epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            // 错误处理
            return;
        }
        m_listen_fds.insert(fd);
    } else {
        // 修改
        if (::epoll_ctl(m_epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) {
            // 错误处理
        }
    }
}

void EventLoop::deleteEpollEvent(FdEvent* event) {
    if (event == nullptr || event->getFd() < 0) {
        return;
    }

    const int fd = event->getFd();

    if (m_listen_fds.contains(fd)) {
        if (::epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, fd, nullptr) < 0) {
            // 错误处理
        }
        m_listen_fds.erase(fd);
    }
}

bool EventLoop::isInLoopThread() const noexcept { return m_thread_id == std::this_thread::get_id(); }

void EventLoop::addTask(std::function<void()> cb, bool is_wake_up) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending_tasks.push(std::move(cb));
    }

    if (is_wake_up) {
        wakeup();
    }
}

void EventLoop::addTimerEvent(const TimerEvent::s_ptr& event) {
    if (m_timer) {
        m_timer->addTimerEvent(event);
    }
}

bool EventLoop::isLooping() const noexcept { return m_is_looping; }

void EventLoop::dealWakeup() {
    // WakeUpFdEvent 会自动读取清除事件
}

EventLoop* EventLoop::GetCurrentEventLoop() { return t_current_event_loop; }

} // namespace rocket
