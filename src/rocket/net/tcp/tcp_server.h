#pragma once

#include "rocket/net/event_loop.h"
#include "rocket/net/io_thread_group.h"
#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/tcp/tcp_acceptor.h"
#include "rocket/net/tcp/tcp_connection.h"
#include "rocket/net/fd_event.h"
#include "rocket/net/coder/abstract_coder.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace rocket {

class TcpServer {
  public:
    using CoderFactory = std::function<std::unique_ptr<AbstractCoder>()>;

    TcpServer(NetAddr::s_ptr local_addr, CoderFactory coder_factory);

    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    TcpServer(TcpServer&&) = delete;
    TcpServer& operator=(TcpServer&&) = delete;

    void setMessageCallback(TcpConnection::MessageCallback cb) { m_message_cb = std::move(cb); }
    void setConnectionCallback(TcpConnection::ConnectionCallback cb) { m_connection_cb = std::move(cb); }
    void setReadyCallback(std::function<void()> cb) { m_ready_cb = std::move(cb); }
    void setHighWaterMarkCallback(TcpConnection::HighWaterMarkCallback cb, std::size_t hwm) {
        m_hwm_cb = std::move(cb);
        m_high_water_mark = hwm;
    }

    void start();
    void stop();

    [[nodiscard]] NetAddr::s_ptr getListenAddr() const noexcept { return m_local_addr; }

  private:
    void onAccept();
    void removeConnection(const TcpConnection::s_ptr& conn);
    void init();

    TcpAcceptor::s_ptr m_acceptor;
    NetAddr::s_ptr m_local_addr;
    CoderFactory m_coder_factory;

    std::unique_ptr<EventLoop> m_main_loop;
    std::unique_ptr<IOThreadGroup> m_io_group;
    std::unique_ptr<FdEvent> m_listen_fd_event;

    std::set<TcpConnection::s_ptr> m_connections;
    mutable std::mutex m_connections_mutex;

    TcpConnection::MessageCallback m_message_cb;
    TcpConnection::ConnectionCallback m_connection_cb;
    std::function<void()> m_ready_cb;
    TcpConnection::HighWaterMarkCallback m_hwm_cb;
    std::size_t m_high_water_mark{0};

    std::atomic<bool> m_running{false};
    std::mutex m_stop_mutex;
    std::mutex m_lifecycle_mutex;
    std::condition_variable m_lifecycle_cv;
    bool m_main_loop_active{false};
    std::thread::id m_start_thread_id;
};

} // namespace rocket
