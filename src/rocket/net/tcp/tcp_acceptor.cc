#include "rocket/net/tcp/tcp_acceptor.h"
#include "rocket/net/tcp/net_addr.h"

#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <utility>

namespace rocket {

TcpAcceptor::TcpAcceptor(NetAddr::s_ptr local_addr) : TcpAcceptor(std::move(local_addr), Config{}) {}

TcpAcceptor::TcpAcceptor(NetAddr::s_ptr local_addr, Config config)
    : m_local_addr(std::move(local_addr)), m_config(config) {
    if (!m_local_addr) {
        return;
    }

    m_family = m_local_addr->getFamily();

    m_listen_fd = socket(m_family, SOCK_STREAM, 0);
    if (m_listen_fd < 0) {
        return;
    }

    if (m_config.reuse_addr) {
        int opt = 1;
        setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }

    if (m_config.reuse_port) {
        int opt = 1;
        setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    }

    const sockaddr* addr = m_local_addr->getSockAddr();
    socklen_t len = m_local_addr->getSockLen();
    if (bind(m_listen_fd, addr, len) < 0) {
        close(m_listen_fd);
        m_listen_fd = -1;
        return;
    }

    if (listen(m_listen_fd, m_config.backlog) < 0) {
        close(m_listen_fd);
        m_listen_fd = -1;
        return;
    }
}

TcpAcceptor::~TcpAcceptor() {
    if (m_listen_fd >= 0) {
        close(m_listen_fd);
        m_listen_fd = -1;
    }
}

TcpAcceptor::AcceptResult TcpAcceptor::accept() const {
    AcceptResult result;

    if (m_listen_fd < 0) {
        result.error_msg = "Listen socket not initialized";
        return result;
    }

    sockaddr_storage peer_addr{};
    socklen_t peer_len = sizeof(peer_addr);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    int client_fd = ::accept(m_listen_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
    if (client_fd < 0) {
        result.error_msg = std::string("Accept failed: ") + strerror(errno);
        return result;
    }

    result.client_fd = client_fd;

    if (peer_addr.ss_family == AF_INET) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        auto* addr_in = reinterpret_cast<sockaddr_in*>(&peer_addr);
        result.peer_addr = std::make_shared<IPNetAddr>(*addr_in);
    } else {
        result.error_msg = "Unsupported address family";
        close(client_fd);
        result.client_fd = -1;
    }

    return result;
}

int TcpAcceptor::getListenFd() const noexcept { return m_listen_fd; }

bool TcpAcceptor::isListening() const noexcept { return m_listen_fd >= 0; }

NetAddr::s_ptr TcpAcceptor::getLocalAddr() const noexcept { return m_local_addr; }

} // namespace rocket