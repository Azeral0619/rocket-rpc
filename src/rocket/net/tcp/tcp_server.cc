#include "rocket/net/tcp/tcp_server.h"

#include "rocket/common/config.h"
#include "rocket/common/log.h"
#include "rocket/net/io_thread.h"

#include <memory>
#include <unistd.h>
#include <utility>

namespace rocket {

TcpServer::TcpServer(NetAddr::s_ptr local_addr, CoderFactory coder_factory)
    : m_local_addr(std::move(local_addr)), m_coder_factory(std::move(coder_factory)) {
    init();
    ROCKET_LOG_INFO("TcpServer listening on [{}]", m_local_addr->toString());
}

TcpServer::~TcpServer() = default;

void TcpServer::init() {
    m_acceptor = std::make_shared<TcpAcceptor>(m_local_addr);
    m_main_loop = std::make_unique<EventLoop>();

    auto cfg = Config::getInstance().getConfig();
    m_io_group = std::make_unique<IOThreadGroup>(cfg->io_threads);

    m_listen_fd_event = std::make_unique<FdEvent>(m_acceptor->getListenFd());
    m_listen_fd_event->listen(FdEvent::TriggerEvent::IN_EVENT, [this] { onAccept(); });
    m_main_loop->addEpollEvent(m_listen_fd_event.get());
}

void TcpServer::onAccept() {
    if (!m_running.load(std::memory_order_acquire)) return;

    auto re = m_acceptor->accept();
    if (!re.isValid()) {
        ROCKET_LOG_ERROR("accept failed: {}", re.error_msg);
        return;
    }

    int client_fd = re.client_fd;
    auto peer_addr = std::move(re.peer_addr);
    if (!m_io_group) {
        ::close(client_fd);
        return;
    }
    IOThread* io_thread = m_io_group->getIOThread();
    if (!io_thread || !io_thread->getEventLoop()) {
        ::close(client_fd);
        return;
    }

    auto conn = std::make_shared<TcpConnection>(
        io_thread->getEventLoop(), client_fd,
        m_local_addr, peer_addr,
        m_coder_factory(),
        TcpConnectionType::Server);

    conn->setMessageCallback(m_message_cb);
    conn->setConnectionCallback(m_connection_cb);
    if (m_hwm_cb) conn->setHighWaterMarkCallback(m_hwm_cb, m_high_water_mark);

    conn->setCloseCallback([this](const TcpConnection::s_ptr& c) {
        removeConnection(c);
    });

    // Register with the IO thread's loop, then establish.
    io_thread->getEventLoop()->queueInLoop([conn] {
        conn->connectEstablished();
    });

    {
        std::lock_guard<std::mutex> lk(m_connections_mutex);
        m_connections.insert(conn);
    }
    ROCKET_LOG_INFO("TcpServer new client fd={} peer={}", client_fd, peer_addr->toString());
}

void TcpServer::removeConnection(const TcpConnection::s_ptr& conn) {
    {
        std::lock_guard<std::mutex> lk(m_connections_mutex);
        m_connections.erase(conn);
    }
    conn->getLoop()->queueInLoop([conn] {
        conn->connectDestroyed();
    });
}

void TcpServer::start() {
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lk(m_lifecycle_mutex);
        m_main_loop_active = true;
        m_start_thread_id = std::this_thread::get_id();
    }

    m_io_group->start();
    if (m_ready_cb) m_ready_cb();
    m_main_loop->loop();

    {
        std::lock_guard<std::mutex> lk(m_lifecycle_mutex);
        m_main_loop_active = false;
    }
    m_lifecycle_cv.notify_all();

    // A fatal poll error can also end the loop without an explicit stop.
    if (m_running.load(std::memory_order_acquire)) stop();
}

void TcpServer::stop() {
    std::lock_guard<std::mutex> stop_lk(m_stop_mutex);
    if (!m_running.exchange(false, std::memory_order_acq_rel)) return;

    // Stop accepting first.  When called from a foreign thread, wait until
    // onAccept() can no longer be using the IO thread group.
    m_main_loop->stop();
    {
        std::unique_lock<std::mutex> lk(m_lifecycle_mutex);
        if (m_start_thread_id != std::this_thread::get_id()) {
            m_lifecycle_cv.wait(lk, [this] { return !m_main_loop_active; });
        }
    }

    std::set<TcpConnection::s_ptr> connections;
    {
        std::lock_guard<std::mutex> lk(m_connections_mutex);
        connections = m_connections;
    }
    for (auto& conn : connections) {
        conn->getLoop()->queueInLoop([conn] {
            conn->shutdownGracefully();
        });
    }

    // Stop IO thread group (joins all IO threads)
    if (m_io_group) m_io_group.reset();

    ROCKET_LOG_INFO("TcpServer stopped");
}

} // namespace rocket
