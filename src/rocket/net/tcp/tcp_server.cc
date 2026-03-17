#include "rocket/net/tcp/tcp_server.h"

#include "rocket/common/config.h"
#include "rocket/common/log.h"
#include "rocket/net/event_loop.h"
#include "rocket/net/fd_event.h"
#include "rocket/net/io_thread.h"
#include "rocket/net/io_thread_group.h"
#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/tcp/tcp_acceptor.h"
#include "rocket/net/tcp/tcp_connect.h"
#include "rocket/net/timer_event.h"

#include <memory>
#include <utility>

namespace rocket {

namespace {
constexpr int kClearClientTimerInterval = 5000; // ms
} // namespace

TcpServer::TcpServer(NetAddr::s_ptr local_addr) : m_local_addr(std::move(local_addr)) {

    init();

    ROCKET_LOG_INFO("rocket TcpServer listen sucess on [{}]", m_local_addr->toString());
}

TcpServer::~TcpServer() = default;

void TcpServer::init() {

    m_acceptor = std::make_shared<TcpAcceptor>(m_local_addr);

    m_main_event_loop = std::make_unique<EventLoop>();

    const auto config = Config::getInstance().getConfig();
    m_io_thread_group = std::make_unique<IOThreadGroup>(config->io_threads);

    m_listen_fd_event = std::make_unique<FdEvent>(m_acceptor->getListenFd());
    m_listen_fd_event->listen(FdEvent::TriggerEvent::IN_EVENT, [this] { onAccept(); });

    m_main_event_loop->addEpollEvent(m_listen_fd_event.get());

    m_clear_client_timer_event =
        std::make_shared<TimerEvent>(kClearClientTimerInterval, true, [this] { clearClientTimerFunc(); });
    m_main_event_loop->addTimerEvent(m_clear_client_timer_event);
}

void TcpServer::onAccept() {
    auto re = m_acceptor->accept();

    if (!re.isValid()) {
        ROCKET_LOG_ERROR("accept client failed: {}", re.error_msg);
        return;
    }

    const int client_fd = re.client_fd;
    auto peer_addr = std::move(re.peer_addr);

    m_client_counts++;

    IOThread* io_thread = m_io_thread_group->getIOThread();

    constexpr int kDefaultBufferSize = 128;
    auto connetion =
        std::make_shared<TcpConnection>(io_thread->getEventLoop(), client_fd, kDefaultBufferSize, std::move(peer_addr),
                                        m_local_addr, TcpConnectionType::TcpConnectionByServer);
    connetion->setState(TcpState::Connected);

    m_client.insert(connetion);

    ROCKET_LOG_INFO("TcpServer succ get client, fd={}", client_fd);
}

void TcpServer::start() {
    m_io_thread_group->start();
    m_main_event_loop->loop();
}

void TcpServer::clearClientTimerFunc() {
    auto it = m_client.begin();
    for (it = m_client.begin(); it != m_client.end();) {
        if ((*it) != nullptr && (*it).use_count() > 0 && (*it)->getState() == TcpState::Closed) {
            ROCKET_LOG_DEBUG("TcpConection [fd:{}] will delete, state={}", (*it)->getFd(),
                             static_cast<int>((*it)->getState()));
            it = m_client.erase(it);
        } else {
            it++;
        }
    }
}

} // namespace rocket