#include "rocket/net/tcp/tcp_client.h"

#include "rocket/common/log.h"
#include "rocket/net/tcp/net_addr.h"

#include <cerrno>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace rocket {

// ── Owned mode: self-managed EventLoop ───────────────────────────────

TcpClient::TcpClient(NetAddr::s_ptr peer_addr, CoderFactory coder_factory)
    : m_peer_addr(std::move(peer_addr)), m_coder_factory(std::move(coder_factory)),
      m_owned_loop(std::make_unique<EventLoop>()) {
    m_loop = m_owned_loop.get();
}

// ── Shared mode: external EventLoop (from IOThreadGroup) ─────────────

TcpClient::TcpClient(NetAddr::s_ptr peer_addr, CoderFactory coder_factory, EventLoop* loop)
    : m_peer_addr(std::move(peer_addr)), m_coder_factory(std::move(coder_factory)),
      m_loop(loop) {
}

TcpClient::~TcpClient() { stop(); }


void TcpClient::connect(std::function<void()> done) {
    int fd = ::socket(m_peer_addr->getFamily(), SOCK_STREAM, 0);
    if (fd < 0) {
        m_connect_error_code = -1;
        m_connect_error_info = "socket() failed: " + std::string(strerror(errno));
        if (done) done();
        return;
    }

    // Set non-blocking before connect so the kernel returns EINPROGRESS
    // immediately and the event loop can poll for completion.
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rt = ::connect(fd, m_peer_addr->getSockAddr(), m_peer_addr->getSockLen());
    if (rt == 0 || (rt < 0 && errno == EINPROGRESS)) {
        // Connection initiated; create TcpConnection on the event loop.
        m_connection = std::make_shared<TcpConnection>(
            m_loop, fd, nullptr, m_peer_addr, m_coder_factory(), TcpConnectionType::Client);

        m_connection->setMessageCallback([this](const TcpConnection::s_ptr& conn,
                                                  std::vector<AbstractProtocol::s_ptr>& msgs) {
            onMessage(conn, msgs);
        });
        m_connection->setCloseCallback([this](const TcpConnection::s_ptr& conn) {
            onClose(conn);
        });

        // Start the event loop thread if we own it; shared loops are already running.
        if (m_owned_loop && !m_thread.joinable()) {
            m_thread = std::thread([this] { m_loop->loop(); });
        }

        // Establish on loop thread
        m_loop->queueInLoop([this, done] {
            if (m_connection->getState() == TcpState::NotConnected) {
                initLocalAddr(m_connection->getFd());
                m_connection->connectEstablished();
            }
            if (done) done();
        });
    } else {
        m_connect_error_code = -1;
        m_connect_error_info = "connect() failed: " + std::string(strerror(errno));
        ::close(fd);
        if (done) done();
        return;
    }
}

int TcpClient::connectSync(int timeout_ms) {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    int err = 0;

    connect([&] {
        std::lock_guard<std::mutex> lk(mtx);
        done = true;
        err = m_connect_error_code;
        cv.notify_one();
    });

    // Start the event loop if we own it; shared loops are already running.
    if ((m_owned_loop != nullptr) && !m_thread.joinable()) {
        m_thread = std::thread([this] { m_loop->loop(); });
    }

    {
        std::unique_lock<std::mutex> lk(mtx);
        if (!cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return done; })) {
            return -1; // timeout
        }
    }
    return err;
}

void TcpClient::send(AbstractProtocol::s_ptr msg) {
    // Serialize through the EventLoop so multiple threads can safely share
    // one TcpClient connection (multiplexing via msg_id routing).
    m_loop->runInLoop([this, msg = std::move(msg)]() mutable {
        if (m_connection) m_connection->send(std::move(msg));
    });
}

void TcpClient::readMessage(std::string_view msg_id, ReadCallback cb) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_read_callbacks[std::string(msg_id)] = std::move(cb);
}

AbstractProtocol::s_ptr TcpClient::requestSync(AbstractProtocol::s_ptr req, int timeout_ms) {
    if (!m_connection || m_connection->getState() != TcpState::Connected) return nullptr;

    std::string msg_id = req->m_msg_id;
    std::mutex mtx;
    std::condition_variable cv;
    AbstractProtocol::s_ptr response;
    bool done = false;

    readMessage(msg_id, [&](AbstractProtocol::s_ptr rsp) {
        std::lock_guard<std::mutex> lk(mtx);
        response = std::move(rsp);
        done = true;
        cv.notify_one();
    });

    send(std::move(req));

    {
        std::unique_lock<std::mutex> lk(mtx);
        if (!cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return done; })) {
            return nullptr;
        }
    }
    return response;
}

void TcpClient::stop() {
    // Only stop the EventLoop/thread if we own them.
    // Shared loops are managed by IOThreadGroup.
    if ((m_owned_loop != nullptr)) {
        if (m_loop->isLooping()) m_loop->stop();

        if (m_thread.joinable()) {
            if (m_thread.get_id() == std::this_thread::get_id()) {
                // Called from the EventLoop thread — can't join ourselves.
                m_thread.detach();
            } else {
                m_thread.join();
            }
        }
    }
}

void TcpClient::onMessage(const TcpConnection::s_ptr& /*conn*/, std::vector<AbstractProtocol::s_ptr>& msgs) {
    for (auto& msg : msgs) {
        ReadCallback cb;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            auto it = m_read_callbacks.find(msg->m_msg_id);
            if (it != m_read_callbacks.end()) {
                cb = it->second;
                m_read_callbacks.erase(it);
            }
        }
        if (cb) cb(msg);
    }
}

void TcpClient::onClose(const TcpConnection::s_ptr& /*conn*/) {
    ROCKET_LOG_DEBUG("TcpClient connection closed");
}

void TcpClient::initLocalAddr(int fd) {
    sockaddr_in local_addr{};
    socklen_t len = sizeof(local_addr);
    const int ret = getsockname(fd, reinterpret_cast<sockaddr*>(&local_addr), &len);
    if (ret == 0) m_local_addr = std::make_shared<IPNetAddr>(local_addr);
}

} // namespace rocket
