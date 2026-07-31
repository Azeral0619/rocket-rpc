#include "rocket/net/rpc/rpc_server.h"

#include "rocket/common/log.h"
#include "rocket/net/coder/tinypb_coder.h"
#include "rocket/net/coder/tinypb_protocol.h"
#include "rocket/net/tcp/tcp_connection.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <memory>
#include <utility>

namespace rocket {

namespace {

volatile std::sig_atomic_t g_shutdown_signal = 0;

extern "C" void requestShutdown(int signal_number) {
    // Assigning sig_atomic_t is async-signal-safe.  All locking, joining,
    // logging and object access is performed by RpcServer's monitor thread.
    g_shutdown_signal = signal_number;
}

} // namespace

RpcServer::RpcServer(NetAddr::s_ptr local_addr, std::size_t worker_threads,
                     bool handle_process_signals,
                     std::size_t max_pending_tasks)
    : m_server(std::move(local_addr), [] {
          return std::make_unique<TinyPBCoder>(
              TinyPBCoder::PayloadMode::Borrowed);
      }),
      m_dispatcher(worker_threads, max_pending_tasks),
      m_handle_process_signals(handle_process_signals) {

    m_server.setMessageCallback([this](const TcpConnection::s_ptr& conn,
                                       std::vector<AbstractProtocol::s_ptr>& messages) {
        onMessage(conn, messages);
    });
}

RpcServer::~RpcServer() {
    stop();
    stopSignalMonitor();
    restoreSignalHandlers();
}

void RpcServer::registerService(Services_ptr service) { m_dispatcher.registerService(std::move(service)); }

bool RpcServer::setDefaultExecutionMode(RpcExecutionMode mode) {
    std::lock_guard<std::mutex> lock(m_config_mutex);
    if (m_started) return false;
    m_dispatcher.setDefaultExecutionMode(mode);
    return true;
}

bool RpcServer::setMethodExecutionMode(std::string full_method_name,
                                       RpcExecutionMode mode) {
    std::lock_guard<std::mutex> lock(m_config_mutex);
    if (m_started) return false;
    return m_dispatcher.setMethodExecutionMode(std::move(full_method_name), mode);
}

std::size_t RpcServer::pendingWorkerTasks() const {
    return m_dispatcher.pendingWorkerTasks();
}

void RpcServer::start() {
    {
        std::lock_guard<std::mutex> lock(m_config_mutex);
        if (m_started || m_stopping.load(std::memory_order_acquire)) return;
        m_started = true;
    }
    if (m_handle_process_signals) startSignalMonitor();
    m_server.start();
    if (m_handle_process_signals) {
        stopSignalMonitor();
        restoreSignalHandlers();
    }
}

void RpcServer::stop() {
    if (m_stopping.exchange(true, std::memory_order_acq_rel)) return;

    {
        std::lock_guard<std::mutex> lk(m_signal_mutex);
        m_signal_monitor_stop = true;
    }
    m_signal_cv.notify_all();

    // Reject new dispatches and drain queued business work while the IO loops
    // are still alive so worker completions can safely enqueue responses.
    m_dispatcher.stop();
    m_server.stop();
}

void RpcServer::startSignalMonitor() {
    g_shutdown_signal = 0;
    {
        std::lock_guard<std::mutex> lk(m_signal_mutex);
        m_signal_monitor_stop = false;
    }

    struct sigaction action {};
    action.sa_handler = requestShutdown;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    const bool installed_sigint = ::sigaction(SIGINT, &action, &m_old_sigint) == 0;
    const bool installed_sigterm = ::sigaction(SIGTERM, &action, &m_old_sigterm) == 0;
    if (installed_sigint && installed_sigterm) {
        m_signal_handlers_installed = true;
    } else {
        if (installed_sigint) ::sigaction(SIGINT, &m_old_sigint, nullptr);
        if (installed_sigterm) ::sigaction(SIGTERM, &m_old_sigterm, nullptr);
        ROCKET_LOG_ERROR("failed to install RPC server signal handlers: {}", strerror(errno));
    }

    m_signal_thread = std::thread([this] {
        std::unique_lock<std::mutex> lk(m_signal_mutex);
        while (!m_signal_monitor_stop) {
            m_signal_cv.wait_for(lk, std::chrono::milliseconds(20));
            if (g_shutdown_signal != 0) {
                lk.unlock();
                stop();
                return;
            }
        }
    });
}

void RpcServer::stopSignalMonitor() {
    {
        std::lock_guard<std::mutex> lk(m_signal_mutex);
        m_signal_monitor_stop = true;
    }
    m_signal_cv.notify_all();
    if (m_signal_thread.joinable()) {
        if (m_signal_thread.get_id() == std::this_thread::get_id()) {
            m_signal_thread.detach();
        } else {
            m_signal_thread.join();
        }
    }
}

void RpcServer::restoreSignalHandlers() {
    if (!m_signal_handlers_installed) return;
    ::sigaction(SIGINT, &m_old_sigint, nullptr);
    ::sigaction(SIGTERM, &m_old_sigterm, nullptr);
    m_signal_handlers_installed = false;
    g_shutdown_signal = 0;
}

void RpcServer::onMessage(const TcpConnection::s_ptr& conn, std::vector<AbstractProtocol::s_ptr>& messages) {
    for (auto& msg : messages) {
        auto response = std::make_shared<TinyPBProtocol>();
        m_dispatcher.dispatch(msg, response, conn);
    }
}

} // namespace rocket
