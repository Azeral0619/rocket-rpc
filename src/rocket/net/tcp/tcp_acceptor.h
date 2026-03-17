#pragma once

#include "rocket/net/tcp/net_addr.h"
#include <memory>
#include <string>

namespace rocket {

class TcpAcceptor {
  public:
    using s_ptr = std::shared_ptr<TcpAcceptor>;

    static constexpr int kDefaultBacklog = 511;

    struct Config {
        int backlog{kDefaultBacklog};
        bool reuse_addr{true};
        bool reuse_port{false};
    };

    struct AcceptResult {
        int client_fd{-1};
        NetAddr::s_ptr peer_addr;
        std::string error_msg;

        [[nodiscard]] constexpr bool isValid() const noexcept { return client_fd >= 0; }
    };

    explicit TcpAcceptor(NetAddr::s_ptr local_addr);
    explicit TcpAcceptor(NetAddr::s_ptr local_addr, Config config);

    ~TcpAcceptor();

    TcpAcceptor(const TcpAcceptor&) = delete;
    TcpAcceptor& operator=(const TcpAcceptor&) = delete;
    TcpAcceptor(TcpAcceptor&&) = delete;
    TcpAcceptor& operator=(TcpAcceptor&&) = delete;

    [[nodiscard]] AcceptResult accept() const;

    [[nodiscard]] int getListenFd() const noexcept;

    [[nodiscard]] bool isListening() const noexcept;

    [[nodiscard]] NetAddr::s_ptr getLocalAddr() const noexcept;

  private:
    NetAddr::s_ptr m_local_addr;
    Config m_config;
    int m_family{-1};
    int m_listen_fd{-1};
};

} // namespace rocket