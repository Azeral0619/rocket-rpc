#pragma once

#include "rocket/net/coder/abstract_protocol.h"
#include "rocket/net/rpc/rpc_dispatcher.h"
#include "rocket/net/tcp/net_addr.h"
#include "rocket/net/tcp/tcp_server.h"
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <google/protobuf/service.h>
#include <memory>
#include <mutex>
#include <thread>

namespace rocket {

// One RpcServer binds an address and dispatches TinyPB requests. Business
// methods run inline on their connection's IO thread by default; callers can
// opt blocking methods into the bounded worker pool before start().
class RpcServer {
  public:
    using Services_ptr = std::shared_ptr<google::protobuf::Service>;

    RpcServer(NetAddr::s_ptr local_addr,
              std::size_t worker_threads = ThreadPool::kDefaultThreadCount,
              bool handle_process_signals = true,
              std::size_t max_pending_tasks = ThreadPool::kDefaultMaxPendingTasks);

    ~RpcServer();

    RpcServer(const RpcServer&) = delete;
    RpcServer& operator=(const RpcServer&) = delete;
    RpcServer(RpcServer&&) = delete;
    RpcServer& operator=(RpcServer&&) = delete;

    void registerService(Services_ptr service);

    bool setDefaultExecutionMode(RpcExecutionMode mode);
    bool setMethodExecutionMode(std::string full_method_name, RpcExecutionMode mode);
    [[nodiscard]] std::size_t pendingWorkerTasks() const;

    // Blocks the calling thread on the main reactor loop.
    void start();

    // Signal the server to stop and join all threads.
    void stop();

  private:
    void onMessage(const TcpConnection::s_ptr& conn, std::vector<AbstractProtocol::s_ptr>& messages);
    void startSignalMonitor();
    void stopSignalMonitor();
    void restoreSignalHandlers();

    TcpServer m_server;
    RpcDispatcher m_dispatcher;
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_started{false};
    std::thread m_signal_thread;
    std::mutex m_signal_mutex;
    std::condition_variable m_signal_cv;
    bool m_signal_monitor_stop{false};
    bool m_signal_handlers_installed{false};
    bool m_handle_process_signals{true};
    struct sigaction m_old_sigint {};
    struct sigaction m_old_sigterm {};
};

} // namespace rocket
