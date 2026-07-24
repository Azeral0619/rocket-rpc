#pragma once

#include "rocket/net/rpc/rpc_connection_pool.h"
#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/timer_event.h"
#include <google/protobuf/service.h>
#include <google/protobuf/stubs/callback.h>
#include <memory>
#include <string_view>

namespace rocket {

// Forward declarations
class TcpClient;
class RpcController;
class RpcChannel;

// Type-safe helper functions to replace macros
template <typename MessageType>
inline std::shared_ptr<MessageType> NewMessage() {
    return std::make_shared<MessageType>();
}

inline std::shared_ptr<RpcController> NewRpcController();

inline std::shared_ptr<RpcChannel> NewRpcChannel(std::string_view addr);

class RpcChannel : public google::protobuf::RpcChannel, public std::enable_shared_from_this<RpcChannel> {

  public:
    using s_ptr = std::shared_ptr<RpcChannel>;
    using controller_s_ptr = std::shared_ptr<google::protobuf::RpcController>;
    using message_s_ptr = std::shared_ptr<google::protobuf::Message>;
    using closure_s_ptr = std::shared_ptr<google::protobuf::Closure>;

    [[nodiscard]] static NetAddr::s_ptr FindAddr(std::string_view str);

    explicit RpcChannel(NetAddr::s_ptr peer_addr);

    // Channel with shared connection pool (one TcpClient per addr, reused).
    RpcChannel(NetAddr::s_ptr peer_addr, RpcConnectionPool::s_ptr pool);

    // Service-aware channel: discovers addresses via registry + load-balances.
    // The registry pointer must outlive the channel.
    RpcChannel(std::string_view service_name, RpcConnectionPool::s_ptr pool,
               ServiceRegistry* registry);

    ~RpcChannel() override;

    RpcChannel(const RpcChannel&) = delete;
    RpcChannel& operator=(const RpcChannel&) = delete;
    RpcChannel(RpcChannel&&) = delete;
    RpcChannel& operator=(RpcChannel&&) = delete;

    void Init(controller_s_ptr controller, message_s_ptr req, message_s_ptr res, closure_s_ptr done);

    void CallMethod(const google::protobuf::MethodDescriptor* method, google::protobuf::RpcController* controller,
                    const google::protobuf::Message* request, google::protobuf::Message* response,
                    google::protobuf::Closure* done) override;

    google::protobuf::RpcController* getController();

    google::protobuf::Message* getRequest();

    google::protobuf::Message* getResponse();

    google::protobuf::Closure* getClosure();

    TcpClient* getTcpClient();

    // Synchronous call: blocks until response or timeout. Returns 0 on success.
    // Uses the underlying CallMethod async path + promise/future internally.
    int CallMethodBlocking(const google::protobuf::MethodDescriptor* method,
                           google::protobuf::RpcController* controller,
                           const google::protobuf::Message* request,
                           google::protobuf::Message* response,
                           int timeout_ms = 3000);

  private:
    // Called from response/timer callbacks (EventLoop thread).  Atomic flag
    // ensures the RPC closure runs at most once regardless of race.
    void finishRpc();

    std::shared_ptr<NetAddr> m_peer_addr{nullptr};
    std::shared_ptr<NetAddr> m_local_addr{nullptr};

    controller_s_ptr m_controller{nullptr};
    message_s_ptr m_request{nullptr};
    message_s_ptr m_response{nullptr};
    closure_s_ptr m_closure{nullptr};

    bool m_is_init{false};

    // Set to true by the first finisher (response or timer) — ensures
    // the RPC closure runs exactly once regardless of race.
    std::atomic<bool> m_rpc_finished{false};

    std::shared_ptr<TcpClient> m_client{nullptr};
    RpcConnectionPool::s_ptr m_pool;      // optional shared pool
    std::string m_service_name;            // service-aware mode: name to discover
    ServiceRegistry* m_registry{nullptr};  // service-aware mode: registry for discovery
    NetAddr::s_ptr m_actual_peer;          // addr actually used for this call (for release)

    // Hold the current timer so we can cancel it before starting a new
    // request.  This prevents a stale timer callback (from the previous
    // CallMethod) from accessing a freed controller after m_rpc_finished
    // is reset by the next request.
    TimerEvent::s_ptr m_timer_event;
};

// Global default connection pool (lazy-init, thread-safe).
RpcConnectionPool::s_ptr GetDefaultPool();

inline std::shared_ptr<RpcController> NewRpcController() { return std::make_shared<RpcController>(); }

inline std::shared_ptr<RpcChannel> NewRpcChannel(std::string_view addr) {
    return std::make_shared<RpcChannel>(RpcChannel::FindAddr(addr));
}

} // namespace rocket
