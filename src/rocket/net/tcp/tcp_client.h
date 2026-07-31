#pragma once

#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/tcp/tcp_connection.h"
#include "rocket/net/coder/abstract_coder.h"
#include "rocket/net/coder/abstract_protocol.h"
#include "rocket/net/event_loop.h"
#include "rocket/net/timer_event.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace rocket {

class TcpClient : public std::enable_shared_from_this<TcpClient> {
  public:
    using s_ptr = std::shared_ptr<TcpClient>;
    using CoderFactory = std::function<std::unique_ptr<AbstractCoder>()>;
    using ReadCallback = std::function<void(AbstractProtocol::s_ptr)>;
    using RegisterCallback = std::function<bool(MessageId)>;

    // Owned mode: creates its own EventLoop + thread (backward compat).
    TcpClient(NetAddr::s_ptr peer_addr, CoderFactory coder_factory);

    // Shared mode: uses an externally-managed EventLoop (e.g. from IOThreadGroup).
    // The caller must ensure the EventLoop outlives this TcpClient.
    TcpClient(NetAddr::s_ptr peer_addr, CoderFactory coder_factory, EventLoop* loop,
              bool direct_output_flush = false);

    ~TcpClient();

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;
    TcpClient(TcpClient&&) = delete;
    TcpClient& operator=(TcpClient&&) = delete;

    void connect(std::function<void()> done);
    int connectSync(int timeout_ms = 5000);

    void send(AbstractProtocol::s_ptr msg);
    // Atomically bind an ID to the request, register its response callback and
    // send it from the connection's owning IO thread.  The optional timer is
    // armed in that same operation. RegisterCallback may reject a request
    // which completed before its queued setup reached the IO thread.
    void sendRequest(AbstractProtocol::s_ptr msg, TimerEvent::s_ptr timer,
                     ReadCallback cb, RegisterCallback on_registered = {});
    void readMessage(MessageId msg_id, ReadCallback cb);
    void cancelRead(MessageId msg_id);

    void addTimerEvent(TimerEvent::s_ptr ev) { m_loop->addTimerEvent(ev); }

    AbstractProtocol::s_ptr requestSync(AbstractProtocol::s_ptr req, int timeout_ms = 5000);

    void stop();

    [[nodiscard]] int getConnectErrorCode() const noexcept {
        return m_connect_error_code.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::string getConnectErrorInfo() const;
    [[nodiscard]] NetAddr::s_ptr getPeerAddr() const noexcept { return m_peer_addr; }
    [[nodiscard]] NetAddr::s_ptr getLocalAddr() const noexcept { return m_local_addr; }
    [[nodiscard]] EventLoop* getLoop() const noexcept { return m_loop; }

    // Returns true when the underlying TCP connection is in Connected state
    // (used by pool health checks to detect dead connections).
    [[nodiscard]] bool isConnected() const noexcept {
        return m_connection && m_connection->getState() == TcpState::Connected;
    }

  private:
    void onMessage(const TcpConnection::s_ptr& conn, std::vector<AbstractProtocol::s_ptr>& msgs);
    void onClose(const TcpConnection::s_ptr& conn);
    void sendRequestInLoop(AbstractProtocol::s_ptr msg, TimerEvent::s_ptr timer,
                           ReadCallback cb, RegisterCallback on_registered);
    void registerReadInLoop(MessageId msg_id, ReadCallback cb);
    void cancelReadInLoop(MessageId msg_id);
    void initLocalAddr(int fd);
    void setConnectError(int code, std::string info);

    NetAddr::s_ptr m_peer_addr;
    NetAddr::s_ptr m_local_addr;
    CoderFactory m_coder_factory;

    // Always the active EventLoop pointer (owned or external).
    EventLoop* m_loop{nullptr};
    // Non-null only in owned mode — holds the self-created EventLoop.
    std::unique_ptr<EventLoop> m_owned_loop;
    bool m_owns_loop{false};
    bool m_direct_output_flush{false};
    // Thread for owned mode only (shared mode uses IOThreadGroup's threads).
    std::thread m_thread;

    TcpConnection::s_ptr m_connection;

    std::atomic<int> m_connect_error_code{0};
    std::string m_connect_error_info;

    // Accessed only by m_loop. The unsynchronized PMR pool recycles erased
    // nodes, avoiding both the old tree walk and per-RPC malloc/free. A small
    // reserve prevents warm-path bucket growth without imposing a large fixed
    // cost on 10k-connection deployments.
    std::pmr::unsynchronized_pool_resource m_pending_resource;
    std::pmr::unordered_map<MessageId, ReadCallback> m_read_callbacks{
        &m_pending_resource};

    mutable std::mutex m_mutex;
    std::atomic<bool> m_stopped{false};
    std::condition_variable m_cv;
    AbstractProtocol::s_ptr m_sync_response;
};

} // namespace rocket
