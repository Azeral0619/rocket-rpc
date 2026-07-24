#pragma once

#include "rocket/net/coder/abstract_protocol.h"
#include "rocket/net/rpc/rpc_dispatcher.h"
#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/tcp/tcp_server.h"
#include <google/protobuf/service.h>
#include <memory>
#include <thread>

namespace rocket {

// One RpcServer binds an address, accepts connections, and dispatches incoming
// TinyPB requests on a worker pool.  Users register protobuf Service
// implementations, then call start() (blocks on the main reactor loop).
class RpcServer {
  public:
    using Services_ptr = std::shared_ptr<google::protobuf::Service>;

    RpcServer(NetAddr::s_ptr local_addr, std::size_t worker_threads = ThreadPool::kDefaultThreadCount);

    ~RpcServer();

    RpcServer(const RpcServer&) = delete;
    RpcServer& operator=(const RpcServer&) = delete;
    RpcServer(RpcServer&&) = delete;
    RpcServer& operator=(RpcServer&&) = delete;

    void registerService(Services_ptr service);

    // Blocks the calling thread on the main reactor loop.
    void start();

    // Signal the server to stop and join all threads.
    void stop();

  private:
    void onMessage(const TcpConnection::s_ptr& conn, std::vector<AbstractProtocol::s_ptr>& messages);

    TcpServer m_server;
    RpcDispatcher m_dispatcher;
};

} // namespace rocket
