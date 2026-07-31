#include "rocket/net/event_loop.h"

#include "rocket/common/log.h"
#include "rocket/net/timer.h"
#include "rocket/net/poller/wakeup_channel.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace rocket {

namespace {
constexpr int kEpollWaitTimeoutMs = -1; // infinite
thread_local EventLoop* t_current_event_loop = nullptr;
} // namespace

EventLoop::EventLoop() {
    m_poller = Poller::createDefault();
    if (!m_poller) {
        ROCKET_LOG_ERROR("EventLoop: Poller::createDefault() failed");
        return;
    }

    initWakeup();
    m_timer = std::make_unique<Timer>();
    m_valid = true;
}

EventLoop::~EventLoop() {
    if (t_current_event_loop == this) t_current_event_loop = nullptr;
}

void EventLoop::initWakeup() {
    m_wakeup_channel = WakeupChannel::create();
    if (!m_wakeup_channel) return;
    m_wakeup_fd_event = std::make_unique<FdEvent>(m_wakeup_channel->readFd());
    m_wakeup_fd_event->listen(FdEvent::TriggerEvent::IN_EVENT, [this] { m_wakeup_channel->drain(); });
    m_poller->updateFdEvent(m_wakeup_fd_event.get());
}

void EventLoop::loop() {
    if (!m_valid) {
        ROCKET_LOG_ERROR("EventLoop::loop() called on invalid (failed-construction) loop");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_thread_id = std::this_thread::get_id();
    }
    t_current_event_loop = this;
    m_is_looping.store(true, std::memory_order_release);

    std::vector<Poller::ActiveEvent> active;

    while (!m_stop_flag.load(std::memory_order_acquire)) {
        int timeout_ms = kEpollWaitTimeoutMs;
        if (m_timer) {
            auto ms = m_timer->msUntilNextExpire();
            timeout_ms = ms.has_value() ? static_cast<int>(ms.value()) : kEpollWaitTimeoutMs;
            if (timeout_ms == 0) timeout_ms = 1;
        }

        int num_events = m_poller->poll(timeout_ms, active);
        if (num_events < 0) {
            ROCKET_LOG_ERROR("EventLoop: poll returned fatal error, exiting loop");
            break;
        }

        processEvents(active);
        processTimerEvents();
        processPendingTasks();
    }

    m_is_looping.store(false, std::memory_order_release);
}

void EventLoop::processEvents(const std::vector<Poller::ActiveEvent>& events) {
    for (const auto& ae : events) {
        auto* fd_event = ae.fd_event;
        if (!fd_event) continue;

        if ((ae.event_mask & toMask(FdEvent::TriggerEvent::IN_EVENT)) != 0) {
            auto handler = fd_event->handler(FdEvent::TriggerEvent::IN_EVENT);
            if (handler) handler();
        }
        if ((ae.event_mask & toMask(FdEvent::TriggerEvent::OUT_EVENT)) != 0) {
            auto handler = fd_event->handler(FdEvent::TriggerEvent::OUT_EVENT);
            if (handler) handler();
        }
        if ((ae.event_mask & toMask(FdEvent::TriggerEvent::ERROR_EVENT)) != 0) {
            auto handler = fd_event->handler(FdEvent::TriggerEvent::ERROR_EVENT);
            if (handler) handler();
        }
    }
}

void EventLoop::processPendingTasks() {
    // A task running on the loop may defer follow-up work without waking the
    // poller (for example, one batched socket flush after an MPSC drain).
    // Drain those locally-produced generations before sleeping again.
    while (true) {
        std::queue<std::function<void()>> tasks;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            tasks.swap(m_pending_tasks);
        }
        if (tasks.empty()) {
            break;
        }
        while (!tasks.empty()) {
            auto& task = tasks.front();
            if (task) task();
            tasks.pop();
        }
    }
}

void EventLoop::processTimerEvents() {
    if (!m_timer) return;
    auto now = std::chrono::steady_clock::now();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    m_timer->fireExpired(now_ms);
}

void EventLoop::wakeup() {
    if (m_wakeup_channel) m_wakeup_channel->wakeup();
}

void EventLoop::stop() {
    m_stop_flag.store(true, std::memory_order_release);
    wakeup();
}

void EventLoop::addEpollEvent(FdEvent* event) {
    if (m_poller && event) m_poller->updateFdEvent(event);
}

void EventLoop::deleteEpollEvent(FdEvent* event) {
    if (m_poller && event) m_poller->removeFdEvent(event);
}

bool EventLoop::isInLoopThread() const noexcept {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_thread_id == std::this_thread::get_id();
}

void EventLoop::addTask(std::function<void()> cb, bool is_wake_up) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending_tasks.push(std::move(cb));
    }
    if (is_wake_up) wakeup();
}

void EventLoop::addTimerEvent(const TimerEvent::s_ptr& event) {
    if (m_timer) m_timer->addTimerEvent(event);
}

bool EventLoop::isLooping() const noexcept {
    return m_is_looping.load(std::memory_order_acquire);
}

std::thread::id EventLoop::getThreadId() const noexcept {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_thread_id;
}

void EventLoop::runInLoop(std::function<void()> fn) {
    if (isInLoopThread()) { fn(); }
    else { queueInLoop(std::move(fn)); }
}

void EventLoop::queueInLoop(std::function<void()> fn) { addTask(std::move(fn), true); }

void EventLoop::assertInLoopThread() const noexcept {
    if (!isInLoopThread()) {
        ROCKET_LOG_ERROR("assertInLoopThread() failed: loop belongs to thread {}", getThreadId());
        std::abort();
    }
}

EventLoop* EventLoop::GetCurrentEventLoop() { return t_current_event_loop; }

} // namespace rocket
