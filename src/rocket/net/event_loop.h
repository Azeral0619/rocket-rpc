#pragma once

#include "rocket/net/fd_event.h"
#include "rocket/net/mpsc_queue.h"
#include "rocket/net/poller/poller.h"
#include "rocket/net/timer_event.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace rocket {

class WakeupChannel;
class Timer;
class TcpConnection;
struct AbstractProtocol;

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

    [[nodiscard]] std::size_t pendingWriteCount() const noexcept {
        return m_queued_write_messages.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t pendingWriteBytes() const noexcept {
        return m_queued_write_bytes.load(std::memory_order_relaxed);
    }

    static constexpr std::size_t kMaxQueuedWriteMessages = 65536;
    static constexpr std::size_t kMaxQueuedWriteBytes = 64ULL * 1024 * 1024;

    [[nodiscard]] static EventLoop* GetCurrentEventLoop();

  private:
    friend class TcpConnection;

    // Data-plane cross-thread writes use a per-loop mailbox. The EventLoop is
    // the sole consumer and therefore the sole mutator of connection buffers.
    [[nodiscard]] bool tryQueueWrite(
        std::shared_ptr<TcpConnection> connection,
        std::shared_ptr<AbstractProtocol> message);

    void initWakeup();
    void processEvents(const std::vector<Poller::ActiveEvent>& events);
    void processWriteMailbox();
    void discardWriteMailbox() noexcept;
    [[nodiscard]] bool tryAcquireWriteProducer() noexcept;
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

    // Keep high-volume writes separate from control tasks. A slow data-plane
    // producer must not hide cancel/close/stop work behind response traffic.
    MpscQueue m_write_mailbox;
    std::atomic<bool> m_write_mailbox_pending{false};
    std::atomic<std::size_t> m_queued_write_messages{0};
    std::atomic<std::size_t> m_queued_write_bytes{0};
    // High bit closes the producer gate; low bits count producers currently
    // touching the mailbox. Destruction closes the gate before draining.
    std::atomic<std::uint32_t> m_write_producer_state{0};

    static constexpr std::uint32_t kWriteProducerClosed = 1U << 31;
    static constexpr std::uint32_t kWriteProducerCountMask =
        kWriteProducerClosed - 1;

    std::unique_ptr<Timer> m_timer;

    std::atomic<bool> m_is_looping{false};
    bool m_valid{false};

  public:
    // Connection count for least-connections load balancing (Hical pattern).
    // Updated by client pools and TcpServer as connections enter/leave.
    std::atomic<std::size_t> m_connection_count{0};
};

} // namespace rocket
