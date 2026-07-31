#include "rocket/net/tcp/tcp_acceptor.h"
#include "rocket/net/tcp/net_addr.h"

#include <arpa/inet.h>
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
    : m_local_addr(std::move(local_addr)), m_config(config),
      m_idle_fd(::open("/dev/null", O_RDONLY | O_CLOEXEC)) {
    if (!m_local_addr) {
        return;
    }

    m_family = m_local_addr->getFamily();

    m_listen_fd = socket(m_family, SOCK_STREAM, 0);
    if (m_listen_fd < 0) {
        return;
    }

    const int status_flags = ::fcntl(m_listen_fd, F_GETFL, 0);
    const int descriptor_flags = ::fcntl(m_listen_fd, F_GETFD, 0);
    if (status_flags < 0 || descriptor_flags < 0 ||
        ::fcntl(m_listen_fd, F_SETFL, status_flags | O_NONBLOCK) < 0 ||
        ::fcntl(m_listen_fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
        close(m_listen_fd);
        m_listen_fd = -1;
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
    if (m_idle_fd >= 0) {
        close(m_idle_fd);
        m_idle_fd = -1;
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
    int client_fd;
#ifdef __linux__
    client_fd = ::accept4(m_listen_fd,
                          reinterpret_cast<sockaddr*>(&peer_addr), &peer_len,
                          SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    client_fd =
        ::accept(m_listen_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
#endif
    if (client_fd < 0) {
        const int accept_errno = errno;
        if (accept_errno == EMFILE && m_idle_fd >= 0) {
            // IdleFd recovery: temporarily free one fd slot to consume the
            // pending connection, then immediately close it (sends RST as
            // back-pressure to the peer).  Pattern from Trantor / Hical.
            ::close(m_idle_fd);
            m_idle_fd = -1;

#ifdef __linux__
            client_fd = ::accept4(
                m_listen_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len,
                SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
            client_fd = ::accept(
                m_listen_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
#endif
            if (client_fd >= 0) {
                ::close(client_fd);  // reject — we're out of fds
            }

            // Re-reserve the idle fd for the next EMFILE cycle.
            m_idle_fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        }
        result.error_code = accept_errno;
        if (accept_errno != EAGAIN && accept_errno != EWOULDBLOCK) {
            result.error_msg =
                std::string("Accept failed: ") + strerror(accept_errno);
        }
        return result;
    }

#ifndef __linux__
    const int client_status_flags = ::fcntl(client_fd, F_GETFL, 0);
    const int client_descriptor_flags = ::fcntl(client_fd, F_GETFD, 0);
    if (client_status_flags < 0 || client_descriptor_flags < 0 ||
        ::fcntl(client_fd, F_SETFL, client_status_flags | O_NONBLOCK) < 0 ||
        ::fcntl(client_fd, F_SETFD,
                client_descriptor_flags | FD_CLOEXEC) < 0) {
        result.error_code = errno;
        result.error_msg =
            std::string("Failed to configure accepted socket: ") +
            strerror(result.error_code);
        ::close(client_fd);
        return result;
    }
#endif

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
