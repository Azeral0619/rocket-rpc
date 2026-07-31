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
    std::vector<TcpClient::s_ptr> stale_clients;

    std::unique_lock<std::mutex> lk(m_mutex);
    if (m_shutdown) return nullptr;

    auto it = m_clients.find(key);
    if (it != m_clients.end()) {
        auto& vec = it->second;

        // Healthy steady-state lookup must stay O(1).  The previous code
        // scanned every connection on every request, so a 10k-connection pool
        // performed 10k state checks while holding the global mutex for each
        // RPC.  Check the round-robin slot first and scan only on failover.
        if (vec.size() >= m_conns_per_addr) {
            size_t& idx = m_rr_counters[key];
            idx = idx % vec.size();
            for (std::size_t checked = 0; checked < vec.size(); ++checked) {
                auto client = vec[idx];
                idx = (idx + 1) % vec.size();
                if (client && client->isConnected()) {
                    return client;
                }
            }

            // Only the exceptional all-dead path pays for a full cleanup.
            for (auto& client : vec) {
                ROCKET_LOG_WARN("RpcConnectionPool: evicting dead connection for {}", key);
                auto* loop = client ? client->getLoop() : nullptr;
                if (loop) {
                    loop->m_connection_count.fetch_sub(1, std::memory_order_relaxed);
                }
            }
            stale_clients.swap(vec);
            m_rr_counters[key] = 0;
        }
    }

    // No healthy connections — create one.  If we have fewer than
    // m_conns_per_addr, create a new slot.  Otherwise reuse the first
    // slot (the vector was cleared above due to dead connections).
    EventLoop* loop = pickLoop();
    // connectSync() waits for the selected loop to report writability.  Never
    // wait on the loop currently executing this acquire call.
    if (loop && loop->isInLoopThread()) {
        const std::size_t n = m_io_group.getIOThreadSize();
        EventLoop* alternative = nullptr;
        std::size_t best_count = std::numeric_limits<std::size_t>::max();
        for (std::size_t i = 0; i < n; ++i) {
            auto* io = m_io_group.getIOThreadAt(i);
            auto* candidate = io ? io->getEventLoop() : nullptr;
            if (!candidate || candidate == loop) continue;
            const auto count =
                candidate->m_connection_count.load(std::memory_order_relaxed);
            if (count < best_count) {
                best_count = count;
                alternative = candidate;
            }
        }
        loop = alternative;
    }
    if (!loop) {
        ROCKET_LOG_ERROR(
            "RpcConnectionPool: no non-blocking IO loop available for {}", key);
        lk.unlock();
        for (auto& stale : stale_clients) {
            if (stale) stale->stop();
        }
        return nullptr;
    }
    lk.unlock();

    // TcpClient::stop() can wait for its owning loop, so never call it while
    // holding the pool mutex: an IO callback may concurrently be acquiring.
    for (auto& stale : stale_clients) {
        if (stale) stale->stop();
    }

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
    size_t& idx = m_rr_counters[key];
    idx %= vec.size();
    auto pooled = vec[idx];
    idx = (idx + 1) % vec.size();
    if (loop) {
        loop->m_connection_count.fetch_sub(1, std::memory_order_relaxed);
    }
    lk.unlock();
    client->stop();
    return pooled;
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
