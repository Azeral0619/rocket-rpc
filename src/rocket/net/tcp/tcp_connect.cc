#include "rocket/net/tcp/tcp_connect.h"

#include "rocket/common/log.h"
#include "rocket/net/coder/abstract_protocol.h"
#include "rocket/net/coder/tinypb_coder.h"
#include "rocket/net/coder/tinypb_protocol.h"
#include "rocket/net/event_loop.h"
#include "rocket/net/fd_event.h"
#include "rocket/net/fd_event_group.h"
#include "rocket/net/rpc/rpc_dispatcher.h"
#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/tcp/tcp_buffer.h"

#include <cerrno>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace rocket {

TcpConnection::TcpConnection(EventLoop* event_loop, int fd, int buffer_size, NetAddr::s_ptr peer_addr,
                             NetAddr::s_ptr local_addr, TcpConnectionType type)
    : m_event_loop(event_loop), m_local_addr(std::move(local_addr)), m_peer_addr(std::move(peer_addr)),
      m_fd_event(FdEventGroup::getInstance().getFdEvent(fd)), m_fd(fd), m_connection_type(type) {

    m_in_buffer = std::make_shared<TcpBuffer>(buffer_size);
    m_out_buffer = std::make_shared<TcpBuffer>(buffer_size);
    m_coder = std::make_unique<TinyPBCoder>();
    m_fd_event->setNonBlock();

    if (m_connection_type == TcpConnectionType::TcpConnectionByServer) {
        listenRead();
    }
}

TcpConnection::~TcpConnection() { ROCKET_LOG_DEBUG("~TcpConnection"); }

void TcpConnection::onRead() {
    if (m_state != TcpState::Connected) {
        ROCKET_LOG_ERROR("onRead error, client has already disconneced, addr[{}], clientfd[{}]",
                         m_peer_addr->toString(), m_fd);
        return;
    }

    bool is_read_all = false;
    bool is_close = false;
    while (!is_read_all) {
        if (m_in_buffer->writeAble() == 0) {
            m_in_buffer->resizeBuffer(2 * m_in_buffer->capacity());
        }
        const auto read_count = m_in_buffer->writeAble();

        const ssize_t rt = read(m_fd, m_in_buffer->beginWrite(), read_count);
        ROCKET_LOG_DEBUG("success read {} bytes from addr[{}], client fd[{}]", rt, m_peer_addr->toString(), m_fd);
        if (rt > 0) {
            m_in_buffer->moveWriteIndex(static_cast<std::size_t>(rt));
            if (static_cast<std::size_t>(rt) == read_count) {
                continue;
            }
            if (static_cast<std::size_t>(rt) < read_count) {
                is_read_all = true;
                break;
            }
        } else if (rt == 0) {
            is_close = true;
            break;
        } else if (rt == -1 && errno == EAGAIN) {
            is_read_all = true;
            break;
        }
    }

    if (is_close) {
        ROCKET_LOG_INFO("peer closed, peer addr [{}], clientfd [{}]", m_peer_addr->toString(), m_fd);
        clear();
        return;
    }

    if (!is_read_all) {
        ROCKET_LOG_ERROR("not read all data");
    }

    excute();
}

void TcpConnection::excute() {
    std::vector<AbstractProtocol::s_ptr> result;
    m_coder->decode(result, m_in_buffer);

    if (m_connection_type == TcpConnectionType::TcpConnectionByServer) {
        for (auto& msg : result) {
            ROCKET_LOG_INFO("success get request[{}] from client[{}]", msg->m_msg_id, m_peer_addr->toString());
            auto response = std::make_shared<TinyPBProtocol>();
            RpcDispatcher::getInstance().dispatch(msg, response, this);
        }
    } else {
        for (auto& msg : result) {
            auto it = m_read_dones.find(msg->m_msg_id);
            if (it != m_read_dones.end()) {
                it->second(msg);
                m_read_dones.erase(it);
            }
        }
    }
}

void TcpConnection::reply(std::vector<AbstractProtocol::s_ptr>& reply_messages) {
    m_coder->encode(reply_messages, m_out_buffer);
    listenWrite();
}

void TcpConnection::onWrite() {
    if (m_state != TcpState::Connected) {
        ROCKET_LOG_ERROR("onWrite error, client has already disconneced, addr[{}], clientfd[{}]",
                         m_peer_addr->toString(), m_fd);
        return;
    }

    if (m_connection_type == TcpConnectionType::TcpConnectionByClient) {
        std::vector<AbstractProtocol::s_ptr> messages;
        messages.reserve(m_write_dones.size());

        for (auto& [msg, _] : m_write_dones) {
            messages.push_back(msg);
        }

        m_coder->encode(messages, m_out_buffer);
    }

    bool is_write_all = false;
    while (true) {
        if (m_out_buffer->readAble() == 0) {
            ROCKET_LOG_DEBUG("no data need to send to client [{}]", m_peer_addr->toString());
            is_write_all = true;
            break;
        }

        const ssize_t rt = write(m_fd, m_out_buffer->peek(), m_out_buffer->readAble());

        if (rt > 0) {
            m_out_buffer->moveReadIndex(static_cast<std::size_t>(rt));
            continue;
        }
        if (rt == -1 && errno == EAGAIN) {
            ROCKET_LOG_ERROR("write data error, errno==EAGAIN and rt == -1");
            break;
        }
    }
    if (is_write_all) {
        m_fd_event->cancel(FdEvent::TriggerEvent::OUT_EVENT);
        m_event_loop->addEpollEvent(m_fd_event);
    }

    if (m_connection_type == TcpConnectionType::TcpConnectionByClient) {
        for (auto& [msg, callback] : m_write_dones) {
            callback(msg);
        }
        m_write_dones.clear();
    }
}

void TcpConnection::setState(TcpState state) noexcept { m_state = state; }

TcpState TcpConnection::getState() const noexcept { return m_state; }

void TcpConnection::clear() {
    if (m_state == TcpState::Closed) {
        return;
    }
    m_fd_event->cancel(FdEvent::TriggerEvent::IN_EVENT);
    m_fd_event->cancel(FdEvent::TriggerEvent::OUT_EVENT);

    m_event_loop->deleteEpollEvent(m_fd_event);

    m_state = TcpState::Closed;
}

void TcpConnection::shutdown() {
    if (m_state == TcpState::Closed || m_state == TcpState::NotConnected) {
        return;
    }

    m_state = TcpState::HalfClosing;

    ::shutdown(m_fd, SHUT_RDWR);
}

void TcpConnection::setConnectionType(TcpConnectionType type) noexcept { m_connection_type = type; }

void TcpConnection::listenWrite() {
    m_fd_event->listen(FdEvent::TriggerEvent::OUT_EVENT, [this] { onWrite(); });
    m_event_loop->addEpollEvent(m_fd_event);
}

void TcpConnection::listenRead() {
    m_fd_event->listen(FdEvent::TriggerEvent::IN_EVENT, [this] { onRead(); });
    m_event_loop->addEpollEvent(m_fd_event);
}

void TcpConnection::pushSendMessage(AbstractProtocol::s_ptr message,
                                    std::function<void(AbstractProtocol::s_ptr)> done) {
    m_write_dones.emplace_back(std::move(message), std::move(done));
}

void TcpConnection::pushReadMessage(std::string_view msg_id, std::function<void(AbstractProtocol::s_ptr)> done) {
    m_read_dones.insert(std::make_pair(msg_id, std::move(done)));
}

NetAddr::s_ptr TcpConnection::getLocalAddr() const { return m_local_addr; }

NetAddr::s_ptr TcpConnection::getPeerAddr() const { return m_peer_addr; }

int TcpConnection::getFd() const noexcept { return m_fd; }

} // namespace rocket