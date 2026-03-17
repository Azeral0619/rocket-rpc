#pragma once

#include "rocket/net/coder/abstract_coder.h"
#include "rocket/net/coder/abstract_protocol.h"
#include "rocket/net/fd_event.h"
#include "rocket/net/tcp/net_addr.h"
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rocket {

class EventLoop;
class FdEvent;
class TcpBuffer;
class AbstractCoder;

enum class TcpState : std::uint8_t {
    NotConnected = 1,
    Connected = 2,
    HalfClosing = 3,
    Closed = 4,
};

enum class TcpConnectionType : std::uint8_t {
    TcpConnectionByServer = 1,
    TcpConnectionByClient = 2,
};

class TcpConnection {
  public:
    using s_ptr = std::shared_ptr<TcpConnection>;

    TcpConnection(EventLoop* event_loop, int fd, int buffer_size, NetAddr::s_ptr peer_addr, NetAddr::s_ptr local_addr,
                  TcpConnectionType type = TcpConnectionType::TcpConnectionByServer);

    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;
    TcpConnection(TcpConnection&&) = delete;
    TcpConnection& operator=(TcpConnection&&) = delete;

    void onRead();

    void excute();

    void onWrite();

    void setState(TcpState state) noexcept;

    [[nodiscard]] TcpState getState() const noexcept;

    void clear();

    [[nodiscard]] int getFd() const noexcept;

    void shutdown();

    void setConnectionType(TcpConnectionType type) noexcept;

    void listenWrite();

    void listenRead();

    void pushSendMessage(AbstractProtocol::s_ptr message, std::function<void(AbstractProtocol::s_ptr)> done);

    void pushReadMessage(std::string_view msg_id, std::function<void(AbstractProtocol::s_ptr)> done);

    [[nodiscard]] NetAddr::s_ptr getLocalAddr() const;

    [[nodiscard]] NetAddr::s_ptr getPeerAddr() const;

    void reply(std::vector<AbstractProtocol::s_ptr>& reply_messages);

    [[nodiscard]] EventLoop* getEventLoop() const noexcept { return m_event_loop; }

  private:
    EventLoop* m_event_loop{nullptr};

    NetAddr::s_ptr m_local_addr;
    NetAddr::s_ptr m_peer_addr;

    std::shared_ptr<TcpBuffer> m_in_buffer;
    std::shared_ptr<TcpBuffer> m_out_buffer;

    FdEvent* m_fd_event;

    std::unique_ptr<AbstractCoder> m_coder;

    TcpState m_state{TcpState::NotConnected};

    int m_fd{0};

    TcpConnectionType m_connection_type{TcpConnectionType::TcpConnectionByServer};

    std::vector<std::pair<AbstractProtocol::s_ptr, std::function<void(AbstractProtocol::s_ptr)>>> m_write_dones;

    std::map<std::string, std::function<void(AbstractProtocol::s_ptr)>, std::less<>> m_read_dones;
};

} // namespace rocket
