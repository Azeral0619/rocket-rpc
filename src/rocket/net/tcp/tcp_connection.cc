#include "rocket/net/tcp/tcp_connection.h"

#include "rocket/common/log.h"
#include "rocket/net/event_loop.h"

#include <algorithm>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <span>
#include <sys/socket.h>
#include <unistd.h>

namespace rocket {

namespace {

constexpr std::size_t kMaxReadPerRound = 64 * 1024;

} // namespace

TcpConnection::TcpConnection(EventLoop* loop, int fd, NetAddr::s_ptr local_addr, NetAddr::s_ptr peer_addr,
                             std::unique_ptr<AbstractCoder> coder, TcpConnectionType type)
    : m_loop(loop), m_local_addr(std::move(local_addr)), m_peer_addr(std::move(peer_addr)), m_fd(fd), m_type(type),
      m_fd_event(std::make_unique<FdEvent>(fd)), m_coder(std::move(coder)),
      m_in_buffer(kDefaultBufferSize), m_out_buffer(kDefaultBufferSize) {

    m_fd_event->setNonBlock();

    const int enabled = 1;
    if (::setsockopt(m_fd, IPPROTO_TCP, TCP_NODELAY, &enabled,
                     sizeof(enabled)) != 0) {
        ROCKET_LOG_WARN("failed to enable TCP_NODELAY fd={} errno={}", m_fd,
                        errno);
    }
}

TcpConnection::~TcpConnection() {
    discardWriteQueue();
    if (m_fd >= 0) {
        ::close(m_fd);
    }
    ROCKET_LOG_DEBUG("~TcpConnection fd={}", m_fd);
}

void TcpConnection::connectEstablished() {
    m_loop->assertInLoopThread();
    if (m_state.load(std::memory_order_acquire) != TcpState::NotConnected) {
        return;
    }

    m_state.store(TcpState::Connected, std::memory_order_release);

    m_fd_event->listen(FdEvent::TriggerEvent::IN_EVENT, [weak = weak_from_this()] {
        if (auto conn = weak.lock()) {
            conn->handleRead();
        }
    });
    m_fd_event->setErrorCallback([weak = weak_from_this()] {
        if (auto conn = weak.lock()) {
            conn->handleError();
        }
    });

    m_loop->addEpollEvent(m_fd_event.get());

    if (m_connection_callback) {
        m_connection_callback(shared_from_this());
    }
}

void TcpConnection::connectInProgress(ConnectCallback cb) {
    m_loop->assertInLoopThread();
    if (m_state.load(std::memory_order_acquire) != TcpState::NotConnected) {
        if (cb) cb(EISCONN);
        return;
    }

    auto completed = std::make_shared<std::atomic<bool>>(false);
    auto finish = [weak = weak_from_this(), completed, cb = std::move(cb)]() mutable {
        bool expected = false;
        if (!completed->compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }

        auto conn = weak.lock();
        if (!conn) return;

        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (::getsockopt(conn->m_fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0) {
            so_error = errno;
        }

        conn->m_loop->deleteEpollEvent(conn->m_fd_event.get());
        conn->m_fd_event->clearCallbacks();

        if (so_error != 0) {
            conn->m_state.store(TcpState::Closed, std::memory_order_release);
            ROCKET_LOG_ERROR("TcpConnection: async connect failed, SO_ERROR={} ({})",
                             so_error, strerror(so_error));
            if (cb) cb(so_error);
            return;
        }

        conn->connectEstablished();
        if (cb) cb(0);
    };

    m_fd_event->listen(FdEvent::TriggerEvent::OUT_EVENT, finish);
    m_fd_event->setErrorCallback(std::move(finish));
    m_loop->addEpollEvent(m_fd_event.get());
}

void TcpConnection::connectDestroyed() {
    m_loop->assertInLoopThread();
    m_state.store(TcpState::Closed, std::memory_order_release);
    m_loop->deleteEpollEvent(m_fd_event.get());
    m_fd_event->clearCallbacks();
    // NOTE: do NOT fire m_close_callback here — the owner
    // (TcpServer::removeConnection) already handled the removal
    // before calling connectDestroyed.
}

void TcpConnection::send(AbstractProtocol::s_ptr message) {
    if (!message) {
        return;
    }
    const auto state = m_state.load(std::memory_order_acquire);
    if (state == TcpState::HalfClosing || state == TcpState::Closed) {
        ROCKET_LOG_WARN("send on closed/half-closing connection fd={}", m_fd);
        return;
    }

    if (m_loop->isInLoopThread()) {
        sendInLoop(std::move(message));
        return;
    }

    // Cross-thread fast path: push to this connection's MPSC queue. Keeping
    // the producer hot spot per connection avoids one shared EventLoop tail.
    // Only the first message in a batch schedules a drain callback;
    // subsequent messages are picked up by the same drain.
    const std::size_t estimated_bytes =
        std::max<std::size_t>(1, message->estimatedWireSize());
    auto* node = MpscNodePool::alloc<QueuedWrite>(
        QueuedWrite{std::move(message), estimated_bytes});
    if (!tryReserveWriteQueue(estimated_bytes)) {
        MpscNodePool::free(node);
        handleWriteQueueOverload();
        return;
    }

    m_write_queue.push(node);

    bool expected = false;
    if (m_write_queued.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        m_loop->queueInLoop([weak = weak_from_this()] {
            if (auto conn = weak.lock()) {
                conn->drainWriteQueue();
            }
        });
    }
}

void TcpConnection::sendInLoop(AbstractProtocol::s_ptr message) {
    sendInLoopBatch(
        std::span<const AbstractProtocol::s_ptr>(&message, 1));
}

void TcpConnection::sendInLoopBatch(
    std::span<const AbstractProtocol::s_ptr> messages) {
    m_loop->assertInLoopThread();
    const auto state = m_state.load(std::memory_order_acquire);
    if (state == TcpState::HalfClosing || state == TcpState::Closed) {
        return;
    }
    if (messages.empty()) {
        return;
    }

    if (!m_coder->encode(messages, m_out_buffer)) {
        ROCKET_LOG_ERROR("encode failed or output buffer limit exceeded, closing fd={}", m_fd);
        handleError();
        return;
    }

    // High-water mark edge trigger (checked once per batch).
    if (m_high_water_mark > 0 && m_out_buffer.readAble() >= m_high_water_mark && !m_sending_above_hwm) {
        m_sending_above_hwm = true;
        if (m_hwm_callback) {
            m_hwm_callback(shared_from_this(), m_out_buffer.readAble());
        }
    }

    // A single decoded RPC usually fits in the socket send buffer.  Write it
    // immediately and avoid allocating/locking an EventLoop task merely to
    // perform the same non-blocking write at the end of the turn.  When one
    // read decoded multiple frames, defer once so all synchronous responses
    // are encoded into one output batch.
    if (m_direct_output_flush && !m_defer_output_flush && !m_flush_queued &&
        !m_fd_event->isListening(FdEvent::TriggerEvent::OUT_EVENT)) {
        const bool all_written = flushOutputInLoop();
        if (m_state.load(std::memory_order_acquire) != TcpState::Closed) {
            updateWriteInterest(all_written);
        }
    } else {
        scheduleOutputFlush();
    }
}

void TcpConnection::drainWriteQueue() {
    m_loop->assertInLoopThread();

    // Collect drained messages and encode them all at once (batch encode).
    // This avoids N per-message encode+enableWriting cycles.
    std::vector<AbstractProtocol::s_ptr> messages;
    messages.reserve(256);
    for (int round = 0; round < 3; ++round) {
        int batch = 0;
        std::size_t drained_bytes = 0;
        messages.clear();
        while (batch < 256) {
            MpscNode* raw = m_write_queue.pop();
            if (!raw) break;

            auto* node = static_cast<TypedMpscNode<QueuedWrite>*>(raw);
            drained_bytes += node->data.estimated_bytes;
            if (m_state.load(std::memory_order_acquire) == TcpState::Connected) {
                messages.push_back(std::move(node->data.message));
            }
            MpscNodePool::free(node);
            ++batch;
        }
        if (batch > 0) {
            releaseWriteQueue(static_cast<std::size_t>(batch),
                              drained_bytes);
        }
        if (!messages.empty()) {
            sendInLoopBatch(messages);
        }
    }

    m_write_queued.store(false, std::memory_order_release);

    // Re-check: a producer may have pushed between the last pop and
    // the store above.  If so, re-arm the drain.
    if (!m_write_queue.empty()) {
        bool expected = false;
        if (m_write_queued.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            m_loop->queueInLoop([weak = weak_from_this()] {
                if (auto conn = weak.lock()) {
                    conn->drainWriteQueue();
                }
            });
        }
    }
}

void TcpConnection::discardWriteQueue() noexcept {
    std::size_t discarded_messages = 0;
    std::size_t discarded_bytes = 0;
    while (MpscNode* raw = m_write_queue.pop()) {
        auto* node = static_cast<TypedMpscNode<QueuedWrite>*>(raw);
        ++discarded_messages;
        discarded_bytes += node->data.estimated_bytes;
        MpscNodePool::free(node);
    }
    if (discarded_messages > 0) {
        releaseWriteQueue(discarded_messages, discarded_bytes);
    }
    m_write_queued.store(false, std::memory_order_relaxed);
}

bool TcpConnection::tryReserveWriteQueue(std::size_t bytes) noexcept {
    if (bytes > kMaxQueuedWriteBytes) return false;

    const std::uint64_t delta =
        (std::uint64_t{1} << kQueuedWriteCountShift) |
        static_cast<std::uint64_t>(bytes);
    const std::uint64_t previous =
        m_queued_write_state.fetch_add(delta, std::memory_order_relaxed);
    const std::size_t previous_messages =
        static_cast<std::size_t>(previous >> kQueuedWriteCountShift);
    const std::size_t previous_bytes =
        static_cast<std::size_t>(previous & kQueuedWriteBytesMask);
    if (previous_messages >= kMaxQueuedWriteMessages ||
        previous_bytes > kMaxQueuedWriteBytes - bytes) {
        m_queued_write_state.fetch_sub(delta, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void TcpConnection::releaseWriteQueue(std::size_t messages,
                                      std::size_t bytes) noexcept {
    const std::uint64_t delta =
        (static_cast<std::uint64_t>(messages) << kQueuedWriteCountShift) |
        static_cast<std::uint64_t>(bytes);
    m_queued_write_state.fetch_sub(delta, std::memory_order_relaxed);
}

void TcpConnection::handleWriteQueueOverload() {
    if (m_write_overload_queued.exchange(true,
                                         std::memory_order_acq_rel)) {
        return;
    }
    const std::uint64_t queued =
        m_queued_write_state.load(std::memory_order_relaxed);
    ROCKET_LOG_ERROR(
        "connection write queue overloaded, closing fd={} queued={} bytes={}",
        m_fd, queued >> kQueuedWriteCountShift,
        queued & kQueuedWriteBytesMask);
    m_loop->queueInLoop([weak = weak_from_this()] {
        if (auto conn = weak.lock()) conn->handleError();
    });
}

void TcpConnection::scheduleOutputFlush() {
    m_loop->assertInLoopThread();
    if (m_flush_queued ||
        m_fd_event->isListening(FdEvent::TriggerEvent::OUT_EVENT)) {
        return;
    }

    m_flush_queued = true;
    m_loop->addTask(
        [weak = weak_from_this()] {
            auto conn = weak.lock();
            if (!conn) return;

            conn->m_flush_queued = false;
            const auto state = conn->m_state.load(std::memory_order_acquire);
            if (state != TcpState::Connected && state != TcpState::HalfClosing) {
                return;
            }

            const bool all_written = conn->flushOutputInLoop();
            if (conn->m_state.load(std::memory_order_acquire) != TcpState::Closed) {
                conn->updateWriteInterest(all_written);
            }
        },
        false);
}

void TcpConnection::enableWriting() {
    m_fd_event->listen(FdEvent::TriggerEvent::OUT_EVENT, [weak = weak_from_this()] {
        if (auto conn = weak.lock()) {
            conn->handleWrite();
        }
    });
    m_loop->addEpollEvent(m_fd_event.get());
}

void TcpConnection::disableWriting() {
    m_fd_event->cancel(FdEvent::TriggerEvent::OUT_EVENT);
    m_loop->addEpollEvent(m_fd_event.get());
}

void TcpConnection::handleRead() {
    m_loop->assertInLoopThread();
    const auto state = m_state.load(std::memory_order_acquire);
    if (state != TcpState::Connected && state != TcpState::HalfClosing) {
        return;
    }

    int saved_errno = 0;
    bool closed = false;

    while (true) {
        if (m_in_buffer.writeAble() == 0) {
            m_in_buffer.resizeBuffer(std::min(m_in_buffer.capacity() * 2, TcpBuffer::kMaxBufferSize));
        }

        const bool use_readv = m_use_readv;
        const std::size_t writable_before_read = m_in_buffer.writeAble();
        const ssize_t n =
            m_in_buffer.readFromFd(m_fd, &saved_errno, use_readv);
        if (n > 0) {
            if (!use_readv &&
                static_cast<std::size_t>(n) == writable_before_read) {
                // The direct-read buffer filled. This connection carries
                // large or pipelined input; retain readv's overflow path for
                // subsequent reads.
                m_use_readv = true;
                continue;
            }
            if (!use_readv ||
                static_cast<std::size_t>(n) < kMaxReadPerRound) {
                break;
            }
            continue;
        }
        if (n == 0) {
            closed = true;
            break;
        }
        if (n < 0) {
            if (saved_errno == EINTR) {
                continue;
            }
            if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) {
                break;
            }
            ROCKET_LOG_ERROR("read error fd={} errno={}", m_fd, saved_errno);
            handleError();
            return;
        }
    }

    if (closed) {
        ROCKET_LOG_INFO("peer closed fd={} peer={}", m_fd, m_peer_addr ? m_peer_addr->toString() : "?");
        handleClose();
        return;
    }

    m_decoded_messages.clear();
    if (!m_coder->decode(m_in_buffer, m_decoded_messages)) {
        m_decoded_messages.clear();
        ROCKET_LOG_ERROR("decoder reported fatal error, closing connection fd={}", m_fd);
        handleError();
        return;
    }

    if (!m_decoded_messages.empty() && m_message_callback) {
        if (m_direct_output_flush) {
            const bool previous_defer = m_defer_output_flush;
            m_defer_output_flush = m_decoded_messages.size() > 1;
            m_message_callback(shared_from_this(), m_decoded_messages);
            m_defer_output_flush = previous_defer;
        } else {
            m_message_callback(shared_from_this(), m_decoded_messages);
        }
    }
    m_decoded_messages.clear();
}

void TcpConnection::handleWrite() {
    m_loop->assertInLoopThread();
    const auto state = m_state.load(std::memory_order_acquire);
    if (state != TcpState::Connected && state != TcpState::HalfClosing) {
        return;
    }

    const bool all_written = flushOutputInLoop();
    if (m_state.load(std::memory_order_acquire) == TcpState::Closed) {
        return;
    }
    updateWriteInterest(all_written);
}

bool TcpConnection::flushOutputInLoop() {
    m_loop->assertInLoopThread();

    int saved_errno = 0;

    while (m_out_buffer.readAble() > 0) {
        const ssize_t n = m_out_buffer.writeToFd(m_fd, &saved_errno);
        if (n > 0) {
            continue;
        }
        if (n < 0) {
            if (saved_errno == EINTR) {
                continue;
            }
            if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) {
                break;
            }
            ROCKET_LOG_ERROR("write error fd={} errno={}", m_fd, saved_errno);
            handleError();
            return false;
        }
        // A zero-byte write made no progress.  Keep the data queued and let
        // EPOLLOUT retry instead of spinning in the EventLoop thread.
        break;
    }

    return m_out_buffer.readAble() == 0;
}

void TcpConnection::updateWriteInterest(bool all_written) {
    m_loop->assertInLoopThread();

    if (all_written) {
        if (m_fd_event->isListening(FdEvent::TriggerEvent::OUT_EVENT)) {
            disableWriting();
        }
        if (m_write_complete_callback) {
            m_write_complete_callback(shared_from_this());
        }
    } else if (!m_fd_event->isListening(FdEvent::TriggerEvent::OUT_EVENT)) {
        enableWriting();
    }

    // High-water mark recovery edge trigger.
    if (m_sending_above_hwm && m_out_buffer.readAble() < m_high_water_mark) {
        m_sending_above_hwm = false;
    }

    if (all_written && m_state.load(std::memory_order_acquire) == TcpState::HalfClosing &&
        m_in_flight.load(std::memory_order_relaxed) == 0) {
        handleClose();
    }
}

void TcpConnection::handleClose() {
    m_loop->assertInLoopThread();
    if (m_state.load(std::memory_order_acquire) == TcpState::Closed) {
        return;
    }
    m_state.store(TcpState::Closed, std::memory_order_release);

    m_loop->deleteEpollEvent(m_fd_event.get());
    m_fd_event->clearCallbacks();

    if (m_close_callback) {
        m_close_callback(shared_from_this());
    }
}

void TcpConnection::handleError() {
    ROCKET_LOG_ERROR("TcpConnection error fd={} peer={}", m_fd, m_peer_addr ? m_peer_addr->toString() : "?");
    handleClose();
}

void TcpConnection::shutdown() {
    if (m_loop->isInLoopThread()) {
        shutdownInLoop();
    } else {
        m_loop->queueInLoop([weak = weak_from_this()] {
            if (auto conn = weak.lock()) {
                conn->shutdownInLoop();
            }
        });
    }
}

void TcpConnection::shutdownInLoop() {
    m_loop->assertInLoopThread();
    if (m_state.load(std::memory_order_acquire) == TcpState::Closed) {
        return;
    }
    m_state.store(TcpState::HalfClosing, std::memory_order_release);

    if (m_out_buffer.readAble() == 0) {
        ::shutdown(m_fd, SHUT_WR);
    }
}

void TcpConnection::shutdownGracefully() {
    shutdown();
}

} // namespace rocket
