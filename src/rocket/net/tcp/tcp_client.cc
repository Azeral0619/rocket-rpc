#include "rocket/net/tcp/tcp_client.h"

#include "rocket/common/log.h"
#include "rocket/net/tcp/net_addr.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
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
    m_owns_loop = true;
}

// ── Shared mode: external EventLoop (from IOThreadGroup) ─────────────

TcpClient::TcpClient(NetAddr::s_ptr peer_addr, CoderFactory coder_factory,
                     EventLoop* loop, bool direct_output_flush)
    : m_peer_addr(std::move(peer_addr)), m_coder_factory(std::move(coder_factory)),
      m_loop(loop), m_direct_output_flush(direct_output_flush) {
}

TcpClient::~TcpClient() { stop(); }


void TcpClient::connect(std::function<void()> done) {
    m_stopped.store(false, std::memory_order_release);
    setConnectError(0, "");

    int fd = ::socket(m_peer_addr->getFamily(), SOCK_STREAM, 0);
    if (fd < 0) {
        const int saved_errno = errno;
        setConnectError(saved_errno, "socket() failed: " + std::string(strerror(saved_errno)));
        if (done) done();
        return;
    }

    // Set non-blocking before connect so the kernel returns EINPROGRESS
    // immediately and the event loop can poll for completion.
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    const int rt = ::connect(fd, m_peer_addr->getSockAddr(), m_peer_addr->getSockLen());
    const int connect_errno = rt < 0 ? errno : 0;
    if (rt == 0 || connect_errno == EINPROGRESS) {
        // Connection initiated; create TcpConnection on the event loop.
        m_connection = std::make_shared<TcpConnection>(
            m_loop, fd, nullptr, m_peer_addr, m_coder_factory(), TcpConnectionType::Client);
        m_connection->setDirectOutputFlush(m_direct_output_flush);

        m_connection->setMessageCallback([weak = weak_from_this()](const TcpConnection::s_ptr& conn,
                                                                   std::vector<AbstractProtocol::s_ptr>& msgs) {
            if (auto client = weak.lock()) client->onMessage(conn, msgs);
        });
        m_connection->setCloseCallback([weak = weak_from_this()](const TcpConnection::s_ptr& conn) {
            if (auto client = weak.lock()) client->onClose(conn);
        });

        // Start the event loop thread if we own it; shared loops are already running.
        if (m_owns_loop && !m_thread.joinable()) {
            // The thread owns the EventLoop allocation once it starts.  This
            // lets a last TcpClient reference safely disappear on its own loop
            // thread: stop() can detach, loop() returns, and the thread then
            // destroys the EventLoop.
            EventLoop* owned_loop = m_owned_loop.release();
            m_loop = owned_loop;
            m_thread = std::thread([owned_loop] {
                owned_loop->loop();
                delete owned_loop;
            });
        }

        auto connection = m_connection;
        auto weak = weak_from_this();
        if (rt == 0) {
            m_loop->queueInLoop([weak, connection, done = std::move(done)]() mutable {
                connection->connectEstablished();
                if (auto client = weak.lock()) {
                    client->initLocalAddr(connection->getFd());
                    client->setConnectError(0, "");
                }
                if (done) done();
            });
        } else {
            m_loop->queueInLoop([weak, connection, done = std::move(done)]() mutable {
                connection->connectInProgress(
                    [weak, connection, done = std::move(done)](int error_code) mutable {
                        if (auto client = weak.lock()) {
                            if (error_code == 0) {
                                client->initLocalAddr(connection->getFd());
                                client->setConnectError(0, "");
                            } else {
                                client->setConnectError(
                                    error_code, "connect() failed: " +
                                                    std::string(strerror(error_code)));
                            }
                        }
                        if (done) done();
                    });
            });
        }
    } else {
        setConnectError(connect_errno,
                        "connect() failed: " + std::string(strerror(connect_errno)));
        ::close(fd);
        if (done) done();
    }
}

int TcpClient::connectSync(int timeout_ms) {
    struct WaitState {
        std::mutex mutex;
        std::condition_variable cv;
        bool done{false};
        int error{0};
    };
    auto state = std::make_shared<WaitState>();

    connect([this, state] {
        {
            std::lock_guard<std::mutex> lk(state->mutex);
            state->done = true;
            state->error = getConnectErrorCode();
        }
        state->cv.notify_one();
    });

    std::unique_lock<std::mutex> lk(state->mutex);
    if (!state->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                            [&] { return state->done; })) {
        lk.unlock();
        setConnectError(ETIMEDOUT, "connect() timed out");
        stop();
        return -1;
    }
    return state->error;
}

void TcpClient::send(AbstractProtocol::s_ptr msg) {
    // Capture the connection rather than `this`: a queued send remains safe
    // if the TcpClient owner is released before the loop drains the task.
    auto connection = m_connection;
    m_loop->runInLoop([connection = std::move(connection), msg = std::move(msg)]() mutable {
        if (connection) connection->send(std::move(msg));
    });
}

void TcpClient::readMessage(std::string_view msg_id, ReadCallback cb) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_read_callbacks[std::string(msg_id)] = std::move(cb);
}

void TcpClient::cancelRead(std::string_view msg_id) {
    std::lock_guard<std::mutex> lk(m_mutex);
    const auto it = m_read_callbacks.find(msg_id);
    if (it != m_read_callbacks.end()) {
        m_read_callbacks.erase(it);
    }
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
            cancelRead(msg_id);
            return nullptr;
        }
    }
    return response;
}

void TcpClient::stop() {
    if (m_stopped.exchange(true, std::memory_order_acq_rel)) return;

    auto connection = std::move(m_connection);
    if (connection && m_loop) {
        if (m_loop->isInLoopThread()) {
            connection->connectDestroyed();
        } else if (m_loop->isLooping()) {
            struct StopState {
                std::mutex mutex;
                std::condition_variable cv;
                bool done{false};
            };
            auto state = std::make_shared<StopState>();
            m_loop->queueInLoop([connection, state] {
                connection->connectDestroyed();
                {
                    std::lock_guard<std::mutex> lk(state->mutex);
                    state->done = true;
                }
                state->cv.notify_one();
            });
            std::unique_lock<std::mutex> lk(state->mutex);
            state->cv.wait(lk, [&] { return state->done; });
        }
    }

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_read_callbacks.clear();
    }

    // Owned loops must always receive stop, including the short window before
    // loop() has published isLooping=true.
    if (m_owns_loop) {
        m_loop->stop();

        if (m_thread.joinable()) {
            if (m_thread.get_id() == std::this_thread::get_id()) {
                m_thread.detach();
            } else {
                m_thread.join();
                m_loop = nullptr;
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

void TcpClient::setConnectError(int code, std::string info) {
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_connect_error_info = std::move(info);
    }
    m_connect_error_code.store(code, std::memory_order_release);
}

std::string TcpClient::getConnectErrorInfo() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_connect_error_info;
}

} // namespace rocket
