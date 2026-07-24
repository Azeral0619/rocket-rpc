#include "rocket/net/rpc/rpc_server.h"

#include "rocket/common/log.h"
#include "rocket/net/coder/tinypb_coder.h"
#include "rocket/net/coder/tinypb_protocol.h"
#include "rocket/net/tcp/tcp_connection.h"

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

RpcServer::~RpcServer() = default;

void RpcServer::registerService(Services_ptr service) { m_dispatcher.registerService(std::move(service)); }

void RpcServer::start() { m_server.start(); }

void RpcServer::stop() {
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
