#pragma once

#include "rocket/net/tcp/net_addr.h"
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rocket {

// ServiceRegistry — abstract service discovery.
//
// Server: register/deregister + heartbeat (lease keep-alive).
// Client: discover + watch (push on address changes).
//
// Implementations:
//   EtcdRegistry  — etcd v3 (HTTP REST API)
//
class ServiceRegistry {
  public:
    using s_ptr = std::shared_ptr<ServiceRegistry>;
    using AddrList = std::vector<NetAddr::s_ptr>;
    using WatchCb = std::function<void(AddrList)>;

    virtual ~ServiceRegistry() = default;

    // ── Server-side ────────────────────────────────────────────────

    // Register a service instance with a TTL (seconds).  The caller
    // must periodically call heartbeat() at < TTL interval.
    // Returns a lease ID (> 0 on success) that must be passed to
    // heartbeat() and deregister().
    [[nodiscard]] virtual int64_t registerService(std::string_view name,
                                                   std::string_view addr,
                                                   int ttl_seconds) = 0;

    // Renew the lease; call every ttl_seconds/3.
    virtual void heartbeat(int64_t lease_id) = 0;

    // Remove the service entry and revoke the lease.
    virtual void deregister(std::string_view name, std::string_view addr,
                            int64_t lease_id) = 0;

    // ── Client-side ────────────────────────────────────────────────

    // Return all registered addresses for a service name.
    [[nodiscard]] virtual AddrList discover(std::string_view name) = 0;

    // Register a callback that fires whenever the address list for
    // `name` changes.  The implementation spawns a background watch.
    virtual void watch(std::string_view name, WatchCb cb) = 0;
};

} // namespace rocket
