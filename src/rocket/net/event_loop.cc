#include "rocket/net/event_loop.h"

#include "rocket/common/log.h"
#include "rocket/net/coder/abstract_protocol.h"
#include "rocket/net/timer.h"
#include "rocket/net/poller/wakeup_channel.h"
#include "rocket/net/tcp/tcp_connection.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace rocket {

namespace {
constexpr int kEpollWaitTimeoutMs = -1; // infinite
constexpr int kMaxPendingTaskRounds = 8;
constexpr std::size_t kMaxWritesPerRound = 1024;
constexpr std::size_t kWriteClockCheckMask = 63;
constexpr auto kMaxWriteTimePerRound = std::chrono::microseconds(200);
thread_local EventLoop* t_current_event_loop = nullptr;

struct PendingWrite {
    std::shared_ptr<TcpConnection> connection;
    std::shared_ptr<AbstractProtocol> message;
    std::size_t estimated_bytes{0};
};

using PendingWriteNode = TypedMpscNode<PendingWrite>;

class WriteProducerPermit {
  public:
    explicit WriteProducerPermit(std::atomic<std::uint32_t>& state) noexcept
        : m_state(&state) {}
    ~WriteProducerPermit() {
        m_state->fetch_sub(1, std::memory_order_release);
    }

    WriteProducerPermit(const WriteProducerPermit&) = delete;
    WriteProducerPermit& operator=(const WriteProducerPermit&) = delete;

  private:
    std::atomic<std::uint32_t>* m_state;
};

bool tryReserve(std::atomic<std::size_t>& counter, std::size_t amount,
                std::size_t limit) noexcept {
    std::size_t current = counter.load(std::memory_order_relaxed);
    while (current <= limit && amount <= limit - current) {
        if (counter.compare_exchange_weak(
                current, current + amount, std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}
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
    m_write_producer_state.fetch_or(kWriteProducerClosed,
                                    std::memory_order_acq_rel);
    while ((m_write_producer_state.load(std::memory_order_acquire) &
            kWriteProducerCountMask) != 0) {
        std::this_thread::yield();
    }
    discardWriteMailbox();
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
        processWriteMailbox();
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

bool EventLoop::tryQueueWrite(
    std::shared_ptr<TcpConnection> connection,
    std::shared_ptr<AbstractProtocol> message) {
    if (!connection || !message || connection->getLoop() != this ||
        !tryAcquireWriteProducer()) {
        return false;
    }
    WriteProducerPermit producer_permit(m_write_producer_state);
    if (m_stop_flag.load(std::memory_order_acquire)) return false;

    const std::size_t estimated_bytes =
        std::max<std::size_t>(1, message->estimatedWireSize());
    if (!connection->tryReserveMailboxWrite(estimated_bytes)) {
        return false;
    }
    if (!tryReserve(m_queued_write_messages, 1,
                    kMaxQueuedWriteMessages)) {
        connection->releaseMailboxWrite(estimated_bytes);
        return false;
    }
    if (!tryReserve(m_queued_write_bytes, estimated_bytes,
                    kMaxQueuedWriteBytes)) {
        m_queued_write_messages.fetch_sub(1, std::memory_order_acq_rel);
        connection->releaseMailboxWrite(estimated_bytes);
        return false;
    }

    auto* node = new (std::nothrow) PendingWriteNode(
        PendingWrite{connection, std::move(message), estimated_bytes});
    if (!node) {
        m_queued_write_bytes.fetch_sub(estimated_bytes,
                                       std::memory_order_acq_rel);
        m_queued_write_messages.fetch_sub(1, std::memory_order_acq_rel);
        connection->releaseMailboxWrite(estimated_bytes);
        return false;
    }

    m_write_mailbox.push(node);
    if (!m_write_mailbox_pending.exchange(true,
                                           std::memory_order_acq_rel)) {
        wakeup();
    }
    return true;
}

bool EventLoop::tryAcquireWriteProducer() noexcept {
    std::uint32_t state =
        m_write_producer_state.load(std::memory_order_relaxed);
    for (;;) {
        if ((state & kWriteProducerClosed) != 0 ||
            (state & kWriteProducerCountMask) ==
                kWriteProducerCountMask) {
            return false;
        }
        if (m_write_producer_state.compare_exchange_weak(
                state, state + 1, std::memory_order_acquire,
                std::memory_order_relaxed)) {
            return true;
        }
    }
}

void EventLoop::processWriteMailbox() {
    assertInLoopThread();
    if (!m_write_mailbox_pending.load(std::memory_order_acquire)) return;

    const auto start = std::chrono::steady_clock::now();
    std::size_t processed = 0;
    while (processed < kMaxWritesPerRound) {
        MpscNode* raw = m_write_mailbox.pop();
        if (!raw) break;

        auto* node = static_cast<PendingWriteNode*>(raw);
        PendingWrite command = std::move(node->data);
        delete node;

        m_queued_write_messages.fetch_sub(1, std::memory_order_acq_rel);
        m_queued_write_bytes.fetch_sub(command.estimated_bytes,
                                       std::memory_order_acq_rel);
        if (command.connection) {
            command.connection->releaseMailboxWrite(command.estimated_bytes);
            if (command.connection->getLoop() == this && command.message) {
                command.connection->sendFromMailboxInLoop(
                    std::move(command.message));
            }
        }

        ++processed;
        if ((processed & kWriteClockCheckMask) == 0 &&
            std::chrono::steady_clock::now() - start >=
                kMaxWriteTimePerRound) {
            break;
        }
    }

    m_write_mailbox_pending.store(false, std::memory_order_release);
    if (!m_write_mailbox.empty()) {
        bool expected = false;
        if (m_write_mailbox_pending.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            // Give socket events and timers a chance before the next bounded
            // mailbox drain.
            wakeup();
        }
    }
}

void EventLoop::discardWriteMailbox() noexcept {
    while (MpscNode* raw = m_write_mailbox.pop()) {
        auto* node = static_cast<PendingWriteNode*>(raw);
        PendingWrite command = std::move(node->data);
        delete node;

        m_queued_write_messages.fetch_sub(1, std::memory_order_relaxed);
        m_queued_write_bytes.fetch_sub(command.estimated_bytes,
                                       std::memory_order_relaxed);
        if (command.connection) {
            command.connection->releaseMailboxWrite(command.estimated_bytes);
        }
    }
    m_write_mailbox_pending.store(false, std::memory_order_relaxed);
}

void EventLoop::processPendingTasks() {
    // A task running on the loop may defer follow-up work without waking the
    // poller (for example, one batched socket flush after an MPSC drain).
    // Drain those locally-produced generations before sleeping again.
    for (int round = 0; round < kMaxPendingTaskRounds; ++round) {
        std::queue<std::function<void()>> tasks;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            tasks.swap(m_pending_tasks);
        }
        if (tasks.empty()) {
            return;
        }
        while (!tasks.empty()) {
            auto& task = tasks.front();
            if (task) task();
            tasks.pop();
        }
    }

    // Preserve reactor fairness if tasks continuously enqueue more tasks.
    // Leave the remainder for the next iteration and make its poll return
    // immediately even when the follow-up was queued without a wakeup.
    bool has_pending = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        has_pending = !m_pending_tasks.empty();
    }
    if (has_pending) {
        wakeup();
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
    // All hot-path calls made by the owner can be answered from TLS without
    // touching the state mutex. Foreign threads still take the lock so the
    // initial publication of m_thread_id remains synchronized.
    if (t_current_event_loop == this) {
        return true;
    }
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

void EventLoop::deleteTimerEvent(const TimerEvent::s_ptr& event) {
    if (m_timer) m_timer->deleteTimerEvent(event);
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
