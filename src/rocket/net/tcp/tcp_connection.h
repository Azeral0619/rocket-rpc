#pragma once

#include "rocket/net/coder/abstract_coder.h"
#include "rocket/net/coder/abstract_protocol.h"
#include "rocket/net/fd_event.h"
#include "rocket/net/mpsc_queue.h"
#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/tcp/tcp_buffer.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace rocket {

class EventLoop;

enum class TcpState : std::uint8_t {
    NotConnected = 1,
    Connected = 2,
    HalfClosing = 3,
    Closed = 4,
};

enum class TcpConnectionType : std::uint8_t {
    Server = 1,
    Client = 2,
};

// One TCP connection owned by a single EventLoop. Cross-thread access must go
// through runInLoop/queueInLoop. Lifetime is controlled by shared_ptr; the
// loop holds a weak_ptr via the FdEvent callbacks (tie pattern).
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
  public:
    using s_ptr = std::shared_ptr<TcpConnection>;
    using w_ptr = std::weak_ptr<TcpConnection>;
    using MessageCallback = std::function<void(const s_ptr&, std::vector<AbstractProtocol::s_ptr>&)>;
    using ConnectionCallback = std::function<void(const s_ptr&)>;
    using CloseCallback = std::function<void(const s_ptr&)>;
    using ConnectCallback = std::function<void(int)>;
    using WriteCompleteCallback = std::function<void(const s_ptr&)>;
    using HighWaterMarkCallback = std::function<void(const s_ptr&, std::size_t)>;

    TcpConnection(EventLoop* loop, int fd, NetAddr::s_ptr local_addr, NetAddr::s_ptr peer_addr,
                  std::unique_ptr<AbstractCoder> coder, TcpConnectionType type);

    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;
    TcpConnection(TcpConnection&&) = delete;
    TcpConnection& operator=(TcpConnection&&) = delete;

    void setMessageCallback(MessageCallback cb) { m_message_callback = std::move(cb); }
    void setConnectionCallback(ConnectionCallback cb) { m_connection_callback = std::move(cb); }
    void setCloseCallback(CloseCallback cb) { m_close_callback = std::move(cb); }
    void setWriteCompleteCallback(WriteCompleteCallback cb) { m_write_complete_callback = std::move(cb); }
    void setHighWaterMarkCallback(HighWaterMarkCallback cb, std::size_t hwm) {
        m_hwm_callback = std::move(cb);
        m_high_water_mark = hwm;
    }

    // Must be called in the owning loop thread (usually via queueInLoop).
    void connectEstablished();

    // Register a non-blocking connect with the loop.  The callback runs only
    // after writability confirms success or SO_ERROR reports failure.
    void connectInProgress(ConnectCallback cb);

    // Must be called in the owning loop thread after the connection is removed
    // from the owner (TcpServer/TcpClient).
    void connectDestroyed();

    // Thread-safe: encodes and queues the message on the loop thread.
    void send(AbstractProtocol::s_ptr message);

    // Thread-safe.
    void shutdown();

    // Begin graceful close: reject new sends, finish in-flight work, then close.
    void shutdownGracefully();

    void setState(TcpState state) noexcept { m_state.store(state, std::memory_order_release); }
    [[nodiscard]] TcpState getState() const noexcept { return m_state.load(std::memory_order_acquire); }

    [[nodiscard]] EventLoop* getLoop() const noexcept { return m_loop; }
    [[nodiscard]] NetAddr::s_ptr getLocalAddr() const { return m_local_addr; }
    [[nodiscard]] NetAddr::s_ptr getPeerAddr() const { return m_peer_addr; }
    [[nodiscard]] int getFd() const noexcept { return m_fd; }

    // In-flight request/response count (used by graceful shutdown).
    void incrInFlight() noexcept { m_in_flight.fetch_add(1, std::memory_order_relaxed); }
    void decrInFlight() noexcept { m_in_flight.fetch_sub(1, std::memory_order_relaxed); }
    [[nodiscard]] int inFlight() const noexcept { return m_in_flight.load(std::memory_order_relaxed); }

  private:
    void handleRead();
    void handleWrite();
    void handleClose();
    void handleError();

    void sendInLoop(AbstractProtocol::s_ptr message);
    void sendInLoopBatch(std::vector<AbstractProtocol::s_ptr> messages);
    void drainWriteQueue();
    void scheduleOutputFlush();
    [[nodiscard]] bool flushOutputInLoop();
    void updateWriteInterest(bool all_written);
    void shutdownInLoop();
    void enableWriting();
    void disableWriting();

    EventLoop* m_loop{nullptr};
    NetAddr::s_ptr m_local_addr;
    NetAddr::s_ptr m_peer_addr;
    const int m_fd{-1};
    const TcpConnectionType m_type{TcpConnectionType::Server};
    std::atomic<TcpState> m_state{TcpState::NotConnected};

    std::unique_ptr<FdEvent> m_fd_event;
    std::unique_ptr<AbstractCoder> m_coder;

    TcpBuffer::s_ptr m_in_buffer;
    TcpBuffer::s_ptr m_out_buffer;

    MessageCallback m_message_callback;
    ConnectionCallback m_connection_callback;
    CloseCallback m_close_callback;
    WriteCompleteCallback m_write_complete_callback;
    HighWaterMarkCallback m_hwm_callback;
    std::size_t m_high_water_mark{0};
    bool m_sending_above_hwm{false};

    std::atomic<int> m_in_flight{0};

    // MPSC lock-free write queue: multiple producer threads push messages
    // wait-free; the EventLoop (single consumer) drains in batch.
    MpscQueue m_write_queue;
    std::atomic<bool> m_write_queued{false};
    bool m_flush_queued{false};  // owning EventLoop thread only

    static constexpr std::size_t kDefaultBufferSize = 4096;
};

} // namespace rocket
