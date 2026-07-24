#pragma once

#include "rocket/net/io_thread_group.h"
#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/tcp/tcp_client.h"
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace rocket {

class ServiceRegistry;

enum class LoadBalance {
    RoundRobin,  // cycle through addresses
    Random,      // pick uniformly at random
};

// Per-address connection pool with permanent ownership.
// TcpClients share a fixed set of EventLoops from an internal IOThreadGroup
// instead of each creating their own thread — this is the production approach
// used by brpc/gRPC.
// Defaults to 4 IO threads; pass io_threads to the constructor to tune.
class RpcConnectionPool {
  public:
    using s_ptr = std::shared_ptr<RpcConnectionPool>;

    static constexpr std::size_t kDefaultIOThreads = 4;
    static constexpr std::size_t kDefaultConnsPerAddr = 4;

    // @param io_threads        Number of IO threads in the pool.
    // @param conns_per_addr    Connections to create per destination address
    //                          (brpc-style: N connections share the load).
    explicit RpcConnectionPool(std::size_t io_threads = kDefaultIOThreads,
                               std::size_t conns_per_addr = kDefaultConnsPerAddr);
    ~RpcConnectionPool();

    RpcConnectionPool(const RpcConnectionPool&) = delete;
    RpcConnectionPool& operator=(const RpcConnectionPool&) = delete;
    RpcConnectionPool(RpcConnectionPool&&) = delete;
    RpcConnectionPool& operator=(RpcConnectionPool&&) = delete;

    // Get or create a TcpClient for the given address.
    TcpClient::s_ptr acquire(NetAddr::s_ptr addr, int timeout_ms = 5000);

    // Discover service via registry, then acquire a connection to one of the
    // instances using the configured load-balance strategy.
    TcpClient::s_ptr acquireByService(std::string_view service_name,
                                       ServiceRegistry* registry,
                                       int timeout_ms = 5000);

    void setLoadBalance(LoadBalance lb) { m_lb = lb; }

    // No-op: pool holds a permanent reference to every TcpClient.
    void release(NetAddr::s_ptr addr, TcpClient::s_ptr client);

    // Shut down all connections and stop IO threads.
    void shutdown();

  private:
    // Pick the next IO thread round-robin.
    EventLoop* pickLoop();

    LoadBalance m_lb{LoadBalance::RoundRobin};
    std::mutex m_mutex;
    IOThreadGroup m_io_group{kDefaultIOThreads};
    std::size_t m_conns_per_addr{kDefaultConnsPerAddr};
    // Per-address connection vector (brpc-style: N connections share the load).
    std::map<std::string, std::vector<TcpClient::s_ptr>> m_clients;
    std::map<std::string, size_t, std::less<>> m_rr_counters;   // per-service/service-key RR index
    bool m_shutdown{false};
};

} // namespace rocket
