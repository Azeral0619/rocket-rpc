#pragma once

#include "rocket/common/service_registry.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rocket {

// Etcd v3 service registry — talks to etcd's HTTP API (gRPC gateway).
// Usage:
//   auto reg = std::make_shared<EtcdRegistry>("http://127.0.0.1:2379");
//   int64_t lease = reg->registerService("Order", "10.0.0.1:12345", 10);
//   // ... periodically: reg->heartbeat(lease);
//   auto addrs = reg->discover("Order");
class EtcdRegistry final : public ServiceRegistry {
  public:
    explicit EtcdRegistry(std::string endpoints);
    ~EtcdRegistry() override;

    EtcdRegistry(const EtcdRegistry&) = delete;
    EtcdRegistry& operator=(const EtcdRegistry&) = delete;

    // ── Server ──
    [[nodiscard]] int64_t registerService(std::string_view name,
                                           std::string_view addr,
                                           int ttl_seconds) override;
    void heartbeat(int64_t lease_id) override;
    void deregister(std::string_view name, std::string_view addr,
                    int64_t lease_id) override;

    // ── Client ──
    [[nodiscard]] AddrList discover(std::string_view name) override;
    void watch(std::string_view name, WatchCb cb) override;

  private:
    // HTTP POST to etcd v3 endpoint, return response body.
    std::string httpPost(std::string_view path, std::string_view json_body);

    // Simple JSON helpers — sufficient for etcd's predictable response format.
    static std::string jsonField(std::string_view json, std::string_view key);
    static std::string base64Encode(std::string_view data);
    static std::string base64Decode(std::string_view data);
    // base64 of key+1 byte (for range_end prefix queries)
    static std::string base64RangeEnd(std::string_view key);

    std::string m_endpoint;    // "http://127.0.0.1:2379"
    std::string m_host;        // "127.0.0.1"
    int m_port{2379};

    // Background heartbeat thread for registered leases.
    struct LeaseEntry {
        int64_t lease_id;
        int ttl_seconds;
    };
    std::mutex m_lease_mutex;
    std::unordered_map<std::string, LeaseEntry> m_leases; // key = "name/addr"
    std::atomic<bool> m_lease_running{false};
    std::jthread m_lease_thread;
    void leaseLoop();
};

} // namespace rocket
