#pragma once

#include "rocket/net/event_loop.h"
#include "rocket/net/fd_event.h"
#include "rocket/net/io_thread_group.h"
#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/tcp/tcp_acceptor.h"
#include "rocket/net/tcp/tcp_connect.h"
#include "rocket/net/timer_event.h"
#include <memory>
#include <set>

namespace rocket {

// Forward declarations
class EventLoop;
class IOThreadGroup;
class FdEvent;

class TcpServer {
  public:
    explicit TcpServer(NetAddr::s_ptr local_addr);

    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    TcpServer(TcpServer&&) = delete;
    TcpServer& operator=(TcpServer&&) = delete;

    void start();

  private:
    void init();

    void onAccept();

    void clearClientTimerFunc();

    TcpAcceptor::s_ptr m_acceptor;

    NetAddr::s_ptr m_local_addr;

    std::unique_ptr<EventLoop> m_main_event_loop;

    std::unique_ptr<IOThreadGroup> m_io_thread_group;

    std::unique_ptr<FdEvent> m_listen_fd_event;

    int m_client_counts{0};

    std::set<TcpConnection::s_ptr> m_client;

    TimerEvent::s_ptr m_clear_client_timer_event;
};

} // namespace rocket
