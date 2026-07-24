#include "rocket/net/tcp/tcp_server.h"

#include "rocket/common/config.h"
#include "rocket/common/log.h"
#include "rocket/net/io_thread.h"

#include <memory>
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
    auto re = m_acceptor->accept();
    if (!re.isValid()) {
        ROCKET_LOG_ERROR("accept failed: {}", re.error_msg);
        return;
    }

    int client_fd = re.client_fd;
    auto peer_addr = std::move(re.peer_addr);
    IOThread* io_thread = m_io_group->getIOThread();

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

    m_connections.insert(conn);
    ROCKET_LOG_INFO("TcpServer new client fd={} peer={}", client_fd, peer_addr->toString());
}

void TcpServer::removeConnection(const TcpConnection::s_ptr& conn) {
    m_connections.erase(conn);
    conn->getLoop()->queueInLoop([conn] {
        conn->connectDestroyed();
    });
}

void TcpServer::start() {
    if (m_ready_cb) m_ready_cb();
    m_running = true;
    m_io_group->start();
    m_main_loop->loop();
}

void TcpServer::stop() {
    if (!m_running) return;
    m_running = false;

    // Copy connection set — IO threads may call removeConnection() concurrently
    // while we iterate, which would invalidate iterators.
    auto connections = m_connections;
    for (auto& conn : connections) {
        conn->getLoop()->queueInLoop([conn] {
            conn->shutdownGracefully();
        });
    }

    // Stop IO thread group (joins all IO threads)
    m_io_group.reset();

    // Stop main loop
    m_main_loop->stop();

    ROCKET_LOG_INFO("TcpServer stopped");
}

} // namespace rocket
