#pragma once

#include "rocket/net/coder/abstract_protocol.h"
#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/tcp/tcp_connect.h"
#include "rocket/net/timer_event.h"
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace rocket {

// Forward declarations
class EventLoop;
class FdEvent;

class TcpClient {
  public:
    using s_ptr = std::shared_ptr<TcpClient>;

    explicit TcpClient(NetAddr::s_ptr peer_addr);

    ~TcpClient();

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;
    TcpClient(TcpClient&&) = delete;
    TcpClient& operator=(TcpClient&&) = delete;

    void connect(const std::function<void()>& done);

    void writeMessage(AbstractProtocol::s_ptr message, std::function<void(AbstractProtocol::s_ptr)> done);

    void readMessage(std::string_view msg_id, std::function<void(AbstractProtocol::s_ptr)> done);

    void stop() const;

    [[nodiscard]] int getConnectErrorCode() const noexcept;

    [[nodiscard]] std::string getConnectErrorInfo() const noexcept;

    [[nodiscard]] NetAddr::s_ptr getPeerAddr() const noexcept;

    [[nodiscard]] NetAddr::s_ptr getLocalAddr() const noexcept;

    void initLocalAddr();

    void addTimerEvent(TimerEvent::s_ptr timer_event) const;
    NetAddr::s_ptr m_peer_addr;
    NetAddr::s_ptr m_local_addr;

    EventLoop* m_event_loop{nullptr};

    int m_fd{-1};
    FdEvent* m_fd_event{nullptr};

    TcpConnection::s_ptr m_connection;

    int m_connect_error_code{0};
    std::string m_connect_error_info;
};

} // namespace rocket
