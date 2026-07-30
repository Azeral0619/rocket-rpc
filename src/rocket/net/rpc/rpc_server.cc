#include "rocket/net/rpc/rpc_server.h"

#include "rocket/common/log.h"
#include "rocket/net/coder/tinypb_coder.h"
#include "rocket/net/coder/tinypb_protocol.h"
#include "rocket/net/tcp/tcp_connection.h"

#include <csignal>
#include <memory>
#include <utility>

namespace rocket {

RpcServer::RpcServer(NetAddr::s_ptr local_addr, std::size_t worker_threads)
    : m_server(std::move(local_addr), [] { return std::make_unique<TinyPBCoder>(); }),
      m_dispatcher(worker_threads) {

    m_server.setMessageCallback([this](const TcpConnection::s_ptr& conn,
                                       std::vector<AbstractProtocol::s_ptr>& messages) {
        onMessage(conn, messages);
    });
}

RpcServer::~RpcServer() {
    stop();
}

void RpcServer::registerService(Services_ptr service) { m_dispatcher.registerService(std::move(service)); }

void RpcServer::start() {
    // SIGINT / SIGTERM → graceful shutdown.
    // Store `this` in a static so the (stateless) signal handler can reach it.
    // Only the first server to call start() gets the handler installed.
    static RpcServer* s_instance = nullptr;
    s_instance = this;
    std::signal(SIGINT, [](int) {
        if (s_instance) s_instance->stop();
    });
    std::signal(SIGTERM, [](int) {
        if (s_instance) s_instance->stop();
    });

    m_server.start();
}

void RpcServer::stop() {
    m_stopping.store(true, std::memory_order_relaxed);
    m_server.stop();
    m_dispatcher.stop();
}

void RpcServer::onMessage(const TcpConnection::s_ptr& conn, std::vector<AbstractProtocol::s_ptr>& messages) {
    for (auto& msg : messages) {
        auto response = std::make_shared<TinyPBProtocol>();
        m_dispatcher.dispatch(msg, response, conn);
    }
}

} // namespace rocket
