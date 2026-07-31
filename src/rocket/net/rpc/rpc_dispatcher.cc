#include "rocket/net/rpc/rpc_dispatcher.h"

#include "rocket/common/ecode.h"
#include "rocket/common/log.h"
#include "rocket/common/runtime.h"
#include "rocket/net/rpc/rpc_controller.h"
#include "rocket/net/tcp/tcp_connection.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rocket {

namespace {

class SelfDeletingClosure final : public google::protobuf::Closure {
  public:
    explicit SelfDeletingClosure(std::function<void()> callback)
        : m_callback(std::move(callback)) {}

    void Run() override {
        std::unique_ptr<SelfDeletingClosure> self(this);
        if (m_callback) m_callback();
    }

  private:
    std::function<void()> m_callback;
};

} // namespace

RpcDispatcher::RpcDispatcher(std::size_t worker_threads, std::size_t max_pending_tasks)
    : m_worker_pool(worker_threads, max_pending_tasks) {
}

void RpcDispatcher::dispatch(AbstractProtocol::s_ptr request, AbstractProtocol::s_ptr response,
                             const TcpConnection::s_ptr& conn) {

    auto req_protocol = std::dynamic_pointer_cast<TinyPBProtocol>(request);
    auto rsp_protocol = std::dynamic_pointer_cast<TinyPBProtocol>(response);

    if (!req_protocol || !rsp_protocol) {
        // Not a TinyPB message — can't handle
        return;
    }

    std::string method_full_name = req_protocol->m_method_name;
    std::string_view service_name;
    std::string_view method_name;

    rsp_protocol->m_msg_id = req_protocol->m_msg_id;
    rsp_protocol->m_method_name = req_protocol->m_method_name;

    auto sendError = [&](int32_t err_code, std::string_view err_info) {
        setTinyPBError(rsp_protocol, err_code, err_info);
        conn->send(rsp_protocol);
    };

    if (m_stopping.load(std::memory_order_acquire)) {
        sendError(error::kRpcShutdown, "server is shutting down");
        return;
    }

    if (!parseServiceFullName(method_full_name, service_name, method_name)) {
        sendError(error::kParseServiceName, "parse service name error");
        return;
    }

    auto it = m_service_map.find(service_name);
    if (it == m_service_map.end()) {
        ROCKET_LOG_ERROR("{} | service name[{}] not found", req_protocol->m_msg_id, service_name);
        sendError(error::kServiceNotFound, "service not found");
        return;
    }

    const auto& service = it->second;
    const auto* method = service->GetDescriptor()->FindMethodByName(method_name);
    if (method == nullptr) {
        ROCKET_LOG_ERROR("{} | method name[{}] not found in service[{}]", req_protocol->m_msg_id, method_name,
                         service_name);
        sendError(error::kMethodNotFound, "method not found");
        return;
    }

    // shared_ptr directly from prototype (avoid unique_ptr → shared_ptr conversion).
    auto req_msg_ptr = std::shared_ptr<google::protobuf::Message>(service->GetRequestPrototype(method).New());
    if (!req_msg_ptr->ParseFromString(req_protocol->m_pb_data)) {
        ROCKET_LOG_ERROR("{} | deserialize error", req_protocol->m_msg_id);
        sendError(error::kFailedDeserialize, "deserialize error");
        return;
    }

    ROCKET_LOG_INFO("{} | get rpc request[{}]", req_protocol->m_msg_id, req_msg_ptr->ShortDebugString());

    auto rsp_msg_ptr = std::shared_ptr<google::protobuf::Message>(service->GetResponsePrototype(method).New());

    // Track in-flight for graceful shutdown.
    conn->incrInFlight();

    const auto mode = executionModeFor(method->full_name());
    if (mode == RpcExecutionMode::Inline) {
        invokeService(service, method, req_msg_ptr, rsp_msg_ptr, req_protocol,
                      rsp_protocol, conn, method->name());
        return;
    }

    const bool queued = m_worker_pool.tryExecute(
        [service, method, req_msg_ptr, rsp_msg_ptr, req_protocol, rsp_protocol, conn] {
            invokeService(service, method, req_msg_ptr, rsp_msg_ptr, req_protocol,
                          rsp_protocol, conn, method->name());
        });
    if (!queued) {
        conn->decrInFlight();
        const bool stopping = m_stopping.load(std::memory_order_acquire);
        const auto err_code =
            stopping ? error::kRpcShutdown : error::kRpcServerOverloaded;
        const std::string_view err_info =
            stopping ? "server is shutting down" : "server worker queue is full";
        ROCKET_LOG_WARN("{} | {}", req_protocol->m_msg_id, err_info);
        sendError(err_code, err_info);
    }
}

void RpcDispatcher::invokeService(
    const Services_ptr& service, const google::protobuf::MethodDescriptor* method,
    const std::shared_ptr<google::protobuf::Message>& req_msg_ptr,
    const std::shared_ptr<google::protobuf::Message>& rsp_msg_ptr,
    const TinyPBProtocol::s_ptr& req_protocol,
    const TinyPBProtocol::s_ptr& rsp_protocol, const TcpConnection::s_ptr& conn,
    std::string_view method_name) {
    auto* runtime = RunTime::GetRunTime();
    runtime->m_msgid = req_protocol->m_msg_id;
    runtime->m_method_name = method_name;

    auto controller_ptr = std::make_shared<RpcController>();
    controller_ptr->SetLocalAddr(conn->getLocalAddr());
    controller_ptr->SetPeerAddr(conn->getPeerAddr());
    controller_ptr->SetMsgId(req_protocol->m_msg_id);

    auto* closure = new SelfDeletingClosure(
        [req_msg_ptr, rsp_msg_ptr, req_protocol, rsp_protocol, conn,
         controller_ptr]() mutable {
            conn->decrInFlight();

            if (!rsp_msg_ptr->SerializeToString(&(rsp_protocol->m_pb_data))) {
                ROCKET_LOG_ERROR("{} | serialize error, origin message [{}]",
                                 req_protocol->m_msg_id,
                                 rsp_msg_ptr->ShortDebugString());
                rsp_protocol->m_err_code = error::kFailedSerialize;
                rsp_protocol->m_err_info = "serialize error";
                rsp_protocol->m_err_info_len =
                    static_cast<std::int32_t>(rsp_protocol->m_err_info.length());
            } else {
                rsp_protocol->m_err_code = 0;
                rsp_protocol->m_err_info = "";
                ROCKET_LOG_INFO(
                    "{} | dispatch success, request[{}], response[{}]",
                    req_protocol->m_msg_id, req_msg_ptr->ShortDebugString(),
                    rsp_msg_ptr->ShortDebugString());
            }
            conn->send(rsp_protocol);
        });

    service->CallMethod(method, controller_ptr.get(), req_msg_ptr.get(),
                        rsp_msg_ptr.get(), closure);
}

bool RpcDispatcher::parseServiceFullName(std::string_view full_name, std::string_view& service_name,
                                         std::string_view& method_name) {
    if (full_name.empty()) {
        ROCKET_LOG_ERROR("full name empty");
        return false;
    }
    size_t i = full_name.rfind('.');
    if (i == std::string::npos) {
        ROCKET_LOG_ERROR("not find . in full name [{}]", full_name);
        return false;
    }
    service_name = full_name.substr(0, i);
    method_name = full_name.substr(i + 1, full_name.length() - i - 1);
    return true;
}

void RpcDispatcher::registerService(Services_ptr service) {
    std::string service_name(service->GetDescriptor()->full_name());
    m_service_map[service_name] = std::move(service);
}

void RpcDispatcher::setDefaultExecutionMode(RpcExecutionMode mode) noexcept {
    m_default_execution_mode = mode;
}

bool RpcDispatcher::setMethodExecutionMode(std::string full_method_name,
                                           RpcExecutionMode mode) {
    if (full_method_name.empty()) return false;
    m_method_execution_modes.insert_or_assign(std::move(full_method_name), mode);
    return true;
}

RpcExecutionMode
RpcDispatcher::executionModeFor(std::string_view full_method_name) const {
    if (m_method_execution_modes.empty()) return m_default_execution_mode;
    const auto it = m_method_execution_modes.find(full_method_name);
    return it == m_method_execution_modes.end() ? m_default_execution_mode
                                                 : it->second;
}

std::size_t RpcDispatcher::pendingWorkerTasks() const {
    return m_worker_pool.pendingCount();
}

void RpcDispatcher::stop() {
    m_stopping.store(true, std::memory_order_release);
    m_worker_pool.stopAndDrain();
}

void RpcDispatcher::setTinyPBError(TinyPBProtocol::s_ptr msg, int32_t err_code, std::string_view err_info) {
    msg->m_err_code = err_code;
    msg->m_err_info = err_info;
    msg->m_err_info_len = static_cast<std::int32_t>(err_info.length());
}

} // namespace rocket
