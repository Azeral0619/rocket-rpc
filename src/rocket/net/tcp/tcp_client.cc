#include "rocket/net/tcp/tcp_client.h"

#include "rocket/common/ecode.h"
#include "rocket/common/log.h"
#include "rocket/net/coder/abstract_protocol.h"
#include "rocket/net/event_loop.h"
#include "rocket/net/fd_event.h"
#include "rocket/net/fd_event_group.h"
#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/tcp/tcp_connect.h"
#include "rocket/net/timer_event.h"

#include <cerrno>
#include <cstring>
#include <functional>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace rocket {

namespace {
constexpr int kDefaultBufferSize = 128;
} // namespace

TcpClient::TcpClient(NetAddr::s_ptr peer_addr)
    : m_peer_addr(std::move(peer_addr)), m_event_loop(EventLoop::GetCurrentEventLoop()),
      m_fd(socket(m_peer_addr->getFamily(), SOCK_STREAM, 0)) {

    if (m_fd < 0) {
        ROCKET_LOG_ERROR("TcpClient::TcpClient() error, failed to create fd");
        return;
    }

    m_fd_event = FdEventGroup::getInstance().getFdEvent(m_fd);
    m_fd_event->setNonBlock();

    m_connection = std::make_shared<TcpConnection>(m_event_loop, m_fd, kDefaultBufferSize, m_peer_addr, nullptr,
                                                   TcpConnectionType::TcpConnectionByClient);
    m_connection->setConnectionType(TcpConnectionType::TcpConnectionByClient);
}

TcpClient::~TcpClient() {
    ROCKET_LOG_DEBUG("TcpClient::~TcpClient()");
    if (m_fd > 0) {
        close(m_fd);
    }
}

void TcpClient::connect(const std::function<void()>& done) {
    const int rt = ::connect(m_fd, m_peer_addr->getSockAddr(), m_peer_addr->getSockLen());
    if (rt == 0) {
        ROCKET_LOG_DEBUG("connect [{}] sussess", m_peer_addr->toString());
        m_connection->setState(TcpState::Connected);
        initLocalAddr();
        if (done) {
            done();
        }
    } else if (rt == -1) {
        if (errno == EINPROGRESS) {
            m_fd_event->listen(FdEvent::TriggerEvent::OUT_EVENT, [this, done]() {
                const int rt = ::connect(m_fd, m_peer_addr->getSockAddr(), m_peer_addr->getSockLen());
                if ((rt < 0 && errno == EISCONN) || (rt == 0)) {
                    ROCKET_LOG_DEBUG("connect [{}] sussess", m_peer_addr->toString());
                    initLocalAddr();
                    m_connection->setState(TcpState::Connected);
                } else {
                    if (errno == ECONNREFUSED) {
                        m_connect_error_code = error::kPeerClosed;
                        m_connect_error_info = "connect refused, sys error = " + std::string(strerror(errno));
                    } else {
                        m_connect_error_code = error::kFailedConnect;
                        m_connect_error_info = "connect unkonwn error, sys error = " + std::string(strerror(errno));
                    }
                    ROCKET_LOG_ERROR("connect errror, errno={}, error={}", errno, strerror(errno));
                    close(m_fd);
                    m_fd = socket(m_peer_addr->getFamily(), SOCK_STREAM, 0);
                }

                m_event_loop->deleteEpollEvent(m_fd_event);
                ROCKET_LOG_DEBUG("now begin to done");
                if (done) {
                    done();
                }
            });
            m_event_loop->addEpollEvent(m_fd_event);

            if (!m_event_loop->isLooping()) {
                m_event_loop->loop();
            }
        } else {
            ROCKET_LOG_ERROR("connect errror, errno={}, error={}", errno, strerror(errno));
            m_connect_error_code = error::kFailedConnect;
            m_connect_error_info = "connect error, sys error = " + std::string(strerror(errno));
            if (done) {
                done();
            }
        }
    }
}

void TcpClient::stop() const {
    if (m_event_loop->isLooping()) {
        m_event_loop->stop();
    }
}

void TcpClient::writeMessage(AbstractProtocol::s_ptr message, std::function<void(AbstractProtocol::s_ptr)> done) {
    m_connection->pushSendMessage(std::move(message), std::move(done));
    m_connection->listenWrite();
}

void TcpClient::readMessage(std::string_view msg_id, std::function<void(AbstractProtocol::s_ptr)> done) {
    m_connection->pushReadMessage(msg_id, std::move(done));
    m_connection->listenRead();
}

int TcpClient::getConnectErrorCode() const noexcept { return m_connect_error_code; }

std::string TcpClient::getConnectErrorInfo() const noexcept { return m_connect_error_info; }

NetAddr::s_ptr TcpClient::getPeerAddr() const noexcept { return m_peer_addr; }

NetAddr::s_ptr TcpClient::getLocalAddr() const noexcept { return m_local_addr; }

void TcpClient::initLocalAddr() {
    sockaddr_in local_addr{};
    socklen_t len = sizeof(local_addr);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const int ret = getsockname(m_fd, reinterpret_cast<sockaddr*>(&local_addr), &len);
    if (ret != 0) {
        ROCKET_LOG_ERROR("initLocalAddr error, getsockname error. errno={}, error={}", errno, strerror(errno));
        return;
    }

    m_local_addr = std::make_shared<IPNetAddr>(local_addr);
}

void TcpClient::addTimerEvent(TimerEvent::s_ptr timer_event) const { m_event_loop->addTimerEvent(timer_event); }

} // namespace rocket