#pragma once

#include "rocket/common/thread_pool.h"
#include "rocket/net/coder/abstract_protocol.h"
#include "rocket/net/coder/tinypb_protocol.h"
#include "rocket/net/tcp/tcp_connection.h"
#include <cstdint>
#include <google/protobuf/service.h>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <atomic>

namespace rocket {

enum class RpcExecutionMode : std::uint8_t {
    Inline = 0,
    WorkerPool = 1,
};

class RpcDispatcher {
  public:
    using Services_ptr = std::shared_ptr<google::protobuf::Service>;
    using s_ptr = std::shared_ptr<RpcDispatcher>;

    explicit RpcDispatcher(
        std::size_t worker_threads = ThreadPool::kDefaultThreadCount,
        std::size_t max_pending_tasks = ThreadPool::kDefaultMaxPendingTasks);

    ~RpcDispatcher() = default;

    RpcDispatcher(const RpcDispatcher&) = delete;
    RpcDispatcher& operator=(const RpcDispatcher&) = delete;
    RpcDispatcher(RpcDispatcher&&) = delete;
    RpcDispatcher& operator=(RpcDispatcher&&) = delete;

    void dispatch(AbstractProtocol::s_ptr request, AbstractProtocol::s_ptr response,
                  const TcpConnection::s_ptr& conn);

    void registerService(Services_ptr service);

    // Configure before RpcServer::start(). Inline keeps the zero-handoff fast
    // path; WorkerPool isolates synchronous or blocking business methods.
    void setDefaultExecutionMode(RpcExecutionMode mode) noexcept;
    bool setMethodExecutionMode(std::string full_method_name, RpcExecutionMode mode);

    [[nodiscard]] std::size_t pendingWorkerTasks() const;

    void stop();

    static void setTinyPBError(TinyPBProtocol::s_ptr msg, int32_t err_code, std::string_view err_info);

  private:
    static bool parseServiceFullName(std::string_view full_name, std::string_view& service_name,
                                     std::string_view& method_name);
    [[nodiscard]] RpcExecutionMode executionModeFor(std::string_view full_method_name) const;
    std::map<std::string, Services_ptr, std::less<>> m_service_map;
    std::map<std::string, RpcExecutionMode, std::less<>> m_method_execution_modes;
    RpcExecutionMode m_default_execution_mode{RpcExecutionMode::Inline};
    ThreadPool m_worker_pool;
    std::atomic<bool> m_stopping{false};
};

} // namespace rocket
