#include "rocket/net/rpc/rpc_connection_pool.h"

#include "rocket/common/log.h"
#include "rocket/common/service_registry.h"
#include "rocket/net/coder/tinypb_coder.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rocket {

RpcConnectionPool::RpcConnectionPool(std::size_t io_threads, std::size_t conns_per_addr)
    : m_io_group(io_threads), m_conns_per_addr(std::max<std::size_t>(1, conns_per_addr)) {
    m_io_group.start();
}

RpcConnectionPool::~RpcConnectionPool() { shutdown(); }

TcpClient::s_ptr RpcConnectionPool::acquireByService(std::string_view service_name,
                                                       ServiceRegistry* registry,
                                                       int timeout_ms) {
    auto addrs = registry->discover(service_name);
    if (addrs.empty()) return nullptr;

    NetAddr::s_ptr pick;
    {
        std::lock_guard lk(m_mutex);
        std::string key(service_name);
        size_t& idx = m_rr_counters[key];

        switch (m_lb) {
        case LoadBalance::RoundRobin:
            pick = addrs[idx % addrs.size()];
            idx = (idx + 1) % addrs.size();
            break;
        case LoadBalance::Random:
            pick = addrs[static_cast<size_t>(std::rand()) % addrs.size()];
            break;
        }
    }
    return acquire(std::move(pick), timeout_ms);
}

EventLoop* RpcConnectionPool::pickLoop() {
    const std::size_t n = m_io_group.getIOThreadSize();
    if (n == 0) return nullptr;

    std::size_t best_idx = 0;
    std::size_t best_count = std::numeric_limits<std::size_t>::max();

    for (std::size_t i = 0; i < n; ++i) {
        auto* io = m_io_group.getIOThreadAt(i);
        if (!io) continue;
        auto* loop = io->getEventLoop();
        if (!loop) continue;
        std::size_t cnt = loop->m_connection_count.load(std::memory_order_relaxed);
        if (cnt < best_count) {
            best_count = cnt;
            best_idx = i;
        }
    }

    auto* io = m_io_group.getIOThreadAt(best_idx);
    return io ? io->getEventLoop() : nullptr;
}

TcpClient::s_ptr RpcConnectionPool::acquire(NetAddr::s_ptr addr, int timeout_ms) {
    std::string key = addr->toString();

    std::unique_lock<std::mutex> lk(m_mutex);
    if (m_shutdown) return nullptr;

    auto it = m_clients.find(key);
    if (it != m_clients.end()) {
        auto& vec = it->second;

        // Prune dead connections, but keep at least one slot for reconnection.
        for (auto iter = vec.begin(); iter != vec.end(); ) {
            if (*iter && (*iter)->isConnected()) {
                ++iter;
            } else {
                ROCKET_LOG_WARN("RpcConnectionPool: evicting dead connection for {}", key);
                auto* loop = *iter ? (*iter)->getLoop() : nullptr;
                if (loop) loop->m_connection_count.fetch_sub(1, std::memory_order_relaxed);
                if (*iter) (*iter)->stop();
                iter = vec.erase(iter);
            }
        }

        // Keep filling the configured pool before switching to round-robin.
        // Sequential acquire() calls must not get stuck forever on slot zero.
        if (vec.size() >= m_conns_per_addr) {
            size_t& idx = m_rr_counters[key];
            idx = idx % vec.size();
            auto client = vec[idx];
            idx = (idx + 1) % vec.size();
            return client;
        }
    }

    // No healthy connections — create one.  If we have fewer than
    // m_conns_per_addr, create a new slot.  Otherwise reuse the first
    // slot (the vector was cleared above due to dead connections).
    EventLoop* loop = pickLoop();
    if (!loop) {
        ROCKET_LOG_ERROR("RpcConnectionPool: no IO loop available for {}", key);
        return nullptr;
    }
    lk.unlock();

    auto client = std::make_shared<TcpClient>(
        std::move(addr),
        [] { return std::make_unique<TinyPBCoder>(); },
        loop);

    int err = client->connectSync(timeout_ms);
    if (err != 0) {
        ROCKET_LOG_ERROR("RpcConnectionPool: connect failed for {}", key);
        return nullptr;
    }

    if (loop) {
        loop->m_connection_count.fetch_add(1, std::memory_order_relaxed);
    }

    lk.lock();
    if (m_shutdown) {
        lk.unlock();
        if (loop) loop->m_connection_count.fetch_sub(1, std::memory_order_relaxed);
        client->stop();
        return nullptr;
    }
    auto& vec = m_clients[key];
    if (vec.size() < m_conns_per_addr) {
        vec.push_back(client);
        return client;
    }
    // All slots full (another thread filled them) — use one from the pool.
    // Undo our connection count since we won't keep this extra connection.
    if (loop) {
        loop->m_connection_count.fetch_sub(1, std::memory_order_relaxed);
    }
    client->stop();
    return vec.front();
}

void RpcConnectionPool::release(NetAddr::s_ptr /*addr*/, TcpClient::s_ptr /*client*/) {
    // No-op: the pool holds a permanent reference.
}

void RpcConnectionPool::shutdown() {
    std::vector<TcpClient::s_ptr> clients;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_shutdown) return;
        m_shutdown = true;
        for (auto& [key, vec] : m_clients) {
            clients.insert(clients.end(), vec.begin(), vec.end());
        }
    }

    // First unregister every connection while its owning loop is alive.
    for (auto& client : clients) {
        if (client) {
            auto* loop = client->getLoop();
            if (loop) {
                loop->m_connection_count.fetch_sub(1, std::memory_order_relaxed);
            }
            client->stop();
        }
    }

    // Keep all clients (and therefore their FdEvent storage) alive until no
    // loop can still be polling them.
    m_io_group.join();

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_clients.clear();
        m_rr_counters.clear();
    }
    clients.clear();
}

} // namespace rocket
